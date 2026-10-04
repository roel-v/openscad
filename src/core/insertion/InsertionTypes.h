// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "core/EvaluationInputs.h"
#include "core/RenderVariables.h"
#include "core/customizer/ParameterSet.h"
#include "core/provenance/SourceProvenance.h"
#include "glview/InspectionPresentation.h"
#include <map>
#include <optional>
#include <stdexcept>

namespace Insertion {
inline constexpr uint32_t protocolVersion=1;
inline constexpr uint64_t maxFrameBytes=256ULL*1024*1024;
inline constexpr uint64_t maxSourceBytes=128ULL*1024*1024;
inline constexpr size_t maxSourceFiles=4096, maxTriangles=2000000, maxDiscoveryRounds=32;
class ProtocolError:public std::runtime_error {public:using std::runtime_error::runtime_error;};
class LimitError:public ProtocolError {public:using ProtocolError::ProtocolError;};
struct NativeConfiguration {
  RenderVariables variables{true,0,Camera()};
  double fn=0,fa=12,fs=2;
  std::vector<std::string> features,libraryPaths;
  std::string defines,backend="Manifold";
  ParameterSet parameters;
  bool hardWarnings=false,parameterCheck=true,rangeCheck=true;
  int traceDepth=0;
  bool traceParameters=false;
};
struct ByteEdit {uint64_t begin=0,end=0;std::string replacement;};
struct GeneratedBinding {
  std::string module,fileKey;
  SourceProvenance::Span call;
};
struct VolumeCheck {
  std::string instancePath;
  SourceProvenance::Span base,candidate;
};
struct PreviewRequest {
  uint64_t serial=0;
  std::string build,rootKey,directory,targetKey,candidate,candidateHash,configurationHash;
  NativeConfiguration configuration;
  std::map<std::string,SourceInput> sources;
  std::vector<ByteEdit> edits;
  std::vector<GeneratedBinding> expectedBindings;
  std::vector<VolumeCheck> subtractTargets;
  std::vector<std::string> affectedPaths;
  std::string itemModule;
};
enum class PreviewStatus {Success,NeedInput,Invalid,Unsupported,LimitExceeded};
struct PreviewDiagnostic {
  std::string code,message,fileKey;
  uint64_t offset=0;
  bool warning=false,generated=false;
};
struct VolumeEvidence {std::string path;double before=0,after=0;};
struct PreviewResponse {
  uint64_t serial=0;
  PreviewStatus status=PreviewStatus::Invalid;
  std::string build,candidateHash,configurationHash,missingKey;
  std::vector<PreviewDiagnostic> diagnostics;
  std::map<std::string,std::string> consumed;
  std::vector<GeneratedBinding> resolvedBindings;
  std::vector<std::string> affectedPaths;
  std::vector<VolumeEvidence> volumes;
  std::shared_ptr<const InspectionMeshSet> meshes,itemMeshes;
  std::map<uint64_t,std::string> piecePaths;
  bool empty=false;
};
}
