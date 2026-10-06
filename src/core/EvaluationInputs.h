#pragma once
#include "core/NativeExecutionGuard.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

class SourceFileCache;
enum class NativeCapability;
using SourceReader = std::function<std::string(const std::string&)>;

struct SourceInput {
  std::string fileKey, utf8, fingerprint;
  std::optional<std::string> editorId;
  std::optional<uint64_t> editorRevision;
};
class EvaluationInputError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};
class MissingCapturedInput : public EvaluationInputError {
public:
  explicit MissingCapturedInput(std::string key) : EvaluationInputError("Missing captured evaluation input: " + key), key(std::move(key)) {}
  const std::string key;
};

// Exact captured bytes; freeze the consumed set before publication to workers.
class EvaluationInputs {
public:
  enum class ReadPolicy { CaptureMissing, RejectMissing };
  explicit EvaluationInputs(SourceInput root, std::vector<SourceInput> editors = {},
    ReadPolicy policy = ReadPolicy::CaptureMissing, std::map<std::string,SourceInput> seeds = {});
  const SourceInput& read(const std::filesystem::path& resolvedPath);
  const SourceInput& root() const { return consumedInputs.at(rootKey); }
  const std::map<std::string, SourceInput>& consumed() const { return consumedInputs; }
  const std::vector<SourceInput>& capturedEditors() const { return editors; }
  const std::map<std::string,SourceInput>& validationInputs() const { return replacementBase ? *replacementBase : consumedInputs; }
  EvaluationInputs withReplacement(const std::string& fileKey, std::string candidateUtf8) const;
  void freeze() { frozen = true; }
  static std::string fileKey(const std::filesystem::path& path);
  static std::string fingerprint(const std::string& bytes);
  static std::string diskBytes(const std::filesystem::path& path);
private:
  static void validate(SourceInput& input);
  std::string rootKey;
  std::vector<SourceInput> editors;
  std::map<std::string, SourceInput> overrides, consumedInputs;
  ReadPolicy policy;
  std::optional<std::map<std::string,SourceInput>> replacementBase;
  bool frozen = false;
};

extern thread_local EvaluationInputs *nativeEvaluationInputs;
extern thread_local std::function<void(NativeCapability)> nativeParserCapabilityGuard;
// Native compilation is already serialized. Select request-local library ASTs
// together with the reader and restore the previous request on every exit.
class NativeEvaluationScope {
public:
  NativeEvaluationScope(EvaluationInputs& inputs, SourceFileCache& cache,
    std::function<void(NativeCapability)> capabilityGuard = {});
  ~NativeEvaluationScope();
  NativeEvaluationScope(const NativeEvaluationScope&) = delete;
  NativeEvaluationScope& operator=(const NativeEvaluationScope&) = delete;
private:
  NativeExecutionGuard execution;
  EvaluationInputs *previousInputs;
  SourceFileCache *previousCache;
  std::function<void(NativeCapability)> previousCapabilityGuard;
};
