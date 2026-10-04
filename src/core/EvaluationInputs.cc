#include "core/EvaluationInputs.h"
#include "core/SourceFileCache.h"
#include <glib.h>
#include <fstream>
#include <iterator>
#include <utility>
#ifdef _WIN32
#include <windows.h>
#endif

thread_local EvaluationInputs *nativeEvaluationInputs = nullptr;
thread_local std::function<void(NativeCapability)> nativeParserCapabilityGuard;

std::string EvaluationInputs::fileKey(const std::filesystem::path& path)
{
  auto canonical = std::filesystem::weakly_canonical(std::filesystem::absolute(path));
  auto key = canonical.generic_string();
#ifdef _WIN32
  // Ask the filesystem for its canonical spelling. Unicode case folding is not
  // Windows ordinal file identity (for example, it can conflate sharp-s with ss).
  const auto handle = CreateFileW(canonical.c_str(), FILE_READ_ATTRIBUTES,
    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
    FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (handle != INVALID_HANDLE_VALUE) {
    const auto length = GetFinalPathNameByHandleW(handle, nullptr, 0, FILE_NAME_NORMALIZED);
    std::wstring resolved(length, L'\0');
    const auto written = length ? GetFinalPathNameByHandleW(handle, resolved.data(), length, FILE_NAME_NORMALIZED) : 0;
    CloseHandle(handle);
    if (written && written < length) {
      resolved.resize(written);
      if (resolved.rfind(L"\\\\?\\UNC\\", 0) == 0) resolved = L"\\\\" + resolved.substr(8);
      else if (resolved.rfind(L"\\\\?\\", 0) == 0) resolved.erase(0, 4);
      key = std::filesystem::path(resolved).generic_string();
    }
  }
#endif
  return key;
}

std::string EvaluationInputs::fingerprint(const std::string& bytes)
{
  auto digest = g_compute_checksum_for_data(G_CHECKSUM_SHA256,
    reinterpret_cast<const guchar *>(bytes.data()), bytes.size());
  std::string result(digest);
  g_free(digest);
  return result;
}

std::string EvaluationInputs::diskBytes(const std::filesystem::path& path)
{
  std::ifstream stream(path, std::ios::binary);
  if (!stream) throw EvaluationInputError("Cannot read evaluation input: " + path.generic_string());
  std::string result{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
  if (stream.bad()) throw EvaluationInputError("Failed reading evaluation input: " + path.generic_string());
  return result;
}

void EvaluationInputs::validate(SourceInput& input)
{
  if(input.fileKey.empty() || (input.fileKey.rfind("session:",0)!=0 && fileKey(input.fileKey)!=input.fileKey))
    throw EvaluationInputError("Noncanonical evaluation input identity: " + input.fileKey);
  if (input.utf8.find('\0') != std::string::npos ||
      !g_utf8_validate(input.utf8.data(), static_cast<gssize>(input.utf8.size()), nullptr))
    throw EvaluationInputError("Invalid UTF-8 or NUL in evaluation input: " + input.fileKey);
  const auto actual=fingerprint(input.utf8);
  if(!input.fingerprint.empty() && input.fingerprint!=actual)
    throw EvaluationInputError("Evaluation input hash does not match its bytes: " + input.fileKey);
  input.fingerprint = actual;
}

EvaluationInputs::EvaluationInputs(SourceInput root, std::vector<SourceInput> captured, ReadPolicy policy,
                                 std::map<std::string,SourceInput> seeds)
  : rootKey(root.fileKey), editors(std::move(captured)), policy(policy)
{
  if (rootKey.empty()) throw EvaluationInputError("Evaluation root has no identity");
  validate(root);
  for (auto& input : editors) {
    validate(input);
    const auto found = overrides.find(input.fileKey);
    if (found != overrides.end() && found->second.utf8 != input.utf8)
      throw EvaluationInputError("Conflicting open buffers for evaluation input: " + input.fileKey);
    overrides.emplace(input.fileKey, input);
  }
  if (auto found = overrides.find(rootKey); found != overrides.end() && found->second.utf8 != root.utf8)
    throw EvaluationInputError("Conflicting open root buffers: " + rootKey);
  overrides[rootKey] = root;
  for(auto& [key,input]:seeds) {
    if(key!=input.fileKey)throw EvaluationInputError("Seed key differs from input identity: " + key);
    validate(input);
    auto found=overrides.find(key);
    if(found!=overrides.end() && found->second.utf8!=input.utf8)
      throw EvaluationInputError("Conflicting captured source seed: " + key);
    // Captured editor metadata remains authoritative when equal seeded bytes exist.
    if(found==overrides.end())overrides.emplace(key,std::move(input));
  }
  consumedInputs.emplace(rootKey, std::move(root));
}

EvaluationInputs EvaluationInputs::withReplacement(const std::string& key,std::string bytes) const
{
  if(replacementBase)throw EvaluationInputError("An evaluation bundle already has a hypothetical replacement");
  auto found=consumedInputs.find(key);
  if(found==consumedInputs.end())throw MissingCapturedInput(key);
  SourceInput candidate{key,std::move(bytes),{},{},{}};validate(candidate);
  EvaluationInputs replaced=*this;
  replaced.replacementBase=consumedInputs;
  replaced.consumedInputs[key]=candidate;replaced.overrides[key]=std::move(candidate);
  return replaced;
}

const SourceInput& EvaluationInputs::read(const std::filesystem::path& resolvedPath)
{
  const auto key = fileKey(resolvedPath);
  if (auto found = consumedInputs.find(key); found != consumedInputs.end()) return found->second;
  if (frozen) throw EvaluationInputError("Evaluation inputs are frozen: " + key);
  if (auto found = overrides.find(key); found != overrides.end()){
    if(replacementBase)replacementBase->emplace(key,found->second);
    return consumedInputs.emplace(key, found->second).first->second;
  }
  if(policy==ReadPolicy::RejectMissing)throw MissingCapturedInput(key);
  SourceInput input{key, diskBytes(resolvedPath), {}, {}, {}};
  validate(input);
  if(replacementBase)replacementBase->emplace(key,input);
  return consumedInputs.emplace(key, std::move(input)).first->second;
}

NativeEvaluationScope::NativeEvaluationScope(EvaluationInputs& inputs, SourceFileCache& cache,
  std::function<void(NativeCapability)> capabilityGuard)
  : previousInputs(nativeEvaluationInputs), previousCache(SourceFileCache::setActive(&cache)),
    previousCapabilityGuard(std::move(nativeParserCapabilityGuard))
{
  nativeEvaluationInputs = &inputs;
  nativeParserCapabilityGuard = std::move(capabilityGuard);
}
NativeEvaluationScope::~NativeEvaluationScope()
{
  nativeEvaluationInputs = previousInputs;
  SourceFileCache::setActive(previousCache);
  nativeParserCapabilityGuard = std::move(previousCapabilityGuard);
}
