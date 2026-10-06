// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace Providers {
struct Diagnostic {
  std::string code, path, pointer, message;
  std::string libraryId, effectiveRoot, detectedMarker, firstMismatchingFile;
  std::vector<std::string> expectedProfiles;
  std::map<std::string,std::string> detectedHashes;
};
template<class T> struct Result {
  std::optional<T> value;
  std::optional<Diagnostic> diagnostic;
  explicit operator bool() const { return value.has_value(); }
};
using ScalarValue = std::variant<double, int64_t, bool, std::string>;
using ParameterValues = std::map<std::string, ScalarValue>;
using SymbolBindings = std::map<std::string, std::string>;
enum class Operation { Additive, Subtractive };
enum class InsertionRole { Geometry, Component };
enum class Availability {
  Available, MissingLibrary, UnsupportedRevision, ModifiedLibrary, ResolutionMismatch,
  InvalidCatalog, DuplicateCatalog, AmbiguousProfile, StaleBinding
};
struct ParameterSpec {
  enum class Type { Number, Integer, Bool, String, Enum } type;
  std::string id, label, unit;
  ScalarValue defaultValue;
  std::optional<double> min, max;
  bool exclusiveMin = false;
  size_t maxLength = 1024;
  std::vector<std::string> enumValues;
};
struct Expression {
  enum class Kind { Constant, Parameter, Vector, Arithmetic } kind;
  ScalarValue constant = 0.0;
  std::string name;
  std::vector<std::shared_ptr<const Expression>> arguments;
};
struct Statement {
  std::string module;
  bool helper = false;
  std::vector<std::pair<std::string, std::shared_ptr<const Expression>>> arguments;
  std::vector<Statement> children;
};
struct ImportRequest {
  std::string libraryId, path;
  enum class Kind { Include, Use } kind;
  enum class Scope { File, Helper } scope;
  std::string helperId;
};
struct HelperDefinition {
  std::string id;
  std::vector<std::string> parameters;
  Statement body;
  std::vector<ImportRequest> localImports;
};
struct ModuleBinding { std::string module, libraryId, path; };
struct ImportEdge { std::string from, kind, spelling, to; };
struct LibraryProfile {
  std::string id, releaseLabel, revisionEvidence, digest;
  std::map<std::string, std::string> fileHashes;
  std::vector<ImportEdge> importEdges;
};
struct LibraryRequirement {
  std::string id, label, defaultImportPrefix;
  std::vector<LibraryProfile> profiles;
};
struct Recipe {
  std::vector<std::string> profileIds;
  std::vector<ImportRequest> imports;
  std::vector<HelperDefinition> helpers;
  std::vector<ModuleBinding> moduleBindings;
  Statement geometry;
  std::array<std::shared_ptr<const Expression>, 3> origin, normal, up;
  std::string frameLabel;
};
struct ItemDefinition {
  std::string id, qualifiedId, label, description, providerId, providerVersion, catalogPath, catalogDigest;
  std::vector<std::string> category;
  std::vector<ParameterSpec> parameters;
  Operation operation;
  InsertionRole insertionRole = InsertionRole::Geometry;
  std::vector<Recipe> recipes;
  std::vector<LibraryRequirement> libraries;
};
struct ProviderDefinition {
  std::string id, version, label, path, digest;
  std::vector<ItemDefinition> items;
};
struct RegistrySnapshot {
  uint64_t generation = 0;
  std::vector<ProviderDefinition> providers;
  std::map<std::string, ItemDefinition> items;
  std::map<std::string, Diagnostic> disabledProviders;
  std::vector<Diagnostic> diagnostics;
};
struct SourceBytes { std::string bytes, revision; };
struct LibraryLocation { std::string root, importPrefix; };
struct LibraryResolutionContext {
  std::string sourceDirectory, revision;
  std::vector<std::string> searchPaths;
  std::map<std::string, LibraryLocation> libraryBindings;
  std::function<Result<std::string>(const std::string&, const std::string&)> resolveImport;
  std::function<Result<SourceBytes>(const std::string&)> readBytes;
};
struct FileEvidence { std::string relativePath, canonicalPath, normalizedHash, rawRevision; };
struct ResolvedLibrary {
  std::string libraryId, profileId, releaseLabel, detectedMarker, root, importPrefix, digest;
  std::vector<FileEvidence> files;
};
struct ProviderBinding {
  std::shared_ptr<const RegistrySnapshot> snapshot;
  std::string qualifiedId, catalogDigest, contextRevision, contextDigest;
  size_t recipeIndex = 0;
  std::vector<ResolvedLibrary> libraries;
};
struct LocalFrame { std::array<double, 3> origin, normal, up; std::string label; };
struct GeneratedFragment {
  ProviderBinding binding;
  std::vector<ImportRequest> fileImports;
  std::vector<HelperDefinition> helpers;
  Statement geometry;
  std::vector<ModuleBinding> moduleBindings;
  ParameterValues parameters;
  LocalFrame frame;
  Operation operation;
  InsertionRole insertionRole;
  std::string helperSource, geometrySource, provenanceComment, standaloneSource;
};
struct ItemStatus {
  std::string qualifiedId, label;
  Availability availability;
  std::optional<Diagnostic> diagnostic;
  std::optional<ProviderBinding> binding;
};
struct CatalogSources {
  std::vector<std::string> paths;
};
const char *availabilityName(Availability value);
const char *roleName(InsertionRole value);
} // namespace Providers
