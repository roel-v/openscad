#pragma once
#include "core/semantic/SemanticScene.h"
#include "core/semantic/SemanticPlacement.h"
#include <tuple>
#include <variant>

namespace Anchors {
inline constexpr const char *extensionId = "anchors";
enum class OwnerRole { Document, Instance, DefinitionDefault };
struct OwnerKey {
  OwnerRole role = OwnerRole::Document;
  std::string id;
  bool operator<(const OwnerKey& other) const { return std::tie(role,id) < std::tie(other.role,other.id); }
  bool operator==(const OwnerKey& other) const { return role == other.role && id == other.id; }
};
struct ReferenceKey {
  OwnerKey owner;
  std::string id;
  bool operator<(const ReferenceKey& other) const { return std::tie(owner,id) < std::tie(other.owner,other.id); }
  bool operator==(const ReferenceKey& other) const { return owner == other.owner && id == other.id; }
};
struct Anchor {
  Vector3d position = Vector3d::Zero(), normal = Vector3d::UnitZ(), up = Vector3d::UnitY();
  Vector3d translation = Vector3d::Zero();
  Eigen::Quaterniond rotation = Eigen::Quaterniond::Identity();
  double spin = 0;
  bool fixedCapture = false, drawingFrame = false;
};
struct Point { Vector3d position = Vector3d::Zero(); };
struct Axis { Vector3d position = Vector3d::Zero(), direction = Vector3d::UnitZ(); };
struct Plane { Vector3d position = Vector3d::Zero(), normal = Vector3d::UnitZ(); };
using ReferenceValue = std::variant<Anchor, Point, Axis, Plane>;
struct ReferenceDeclaration : Design::Payload {
  ReferenceValue value;
  std::optional<std::string> owner;
};
enum class AttachmentMode { Mate, Align };
struct EndpointPath { std::string component, anchor; };
struct AttachmentDeclaration : Design::Payload {
  EndpointPath child, target;
  AttachmentMode mode = AttachmentMode::Mate;
  Vector3d offset = Vector3d::Zero(), offsetAdjustment = Vector3d::Zero();
  double spin = 0, spinAdjustment = 0;
};
struct ReferenceRecord {
  ReferenceKey key;
  std::string label;
  ReferenceValue value;
  std::shared_ptr<const Design::Record> declaration;
  std::string definitionId;
  uint64_t invocationId = 0;
  bool explicitOwner = false;
};
struct Endpoint { ReferenceKey reference; std::string path; };
struct Attachment {
  ReferenceKey key; // Attachment IDs have a namespace separate from reference IDs.
  std::string label;
  Endpoint child, target;
  AttachmentDeclaration value;
  std::shared_ptr<const Design::Record> declaration;
  uint64_t invocationId = 0;
};
struct ReferenceScene : Design::Payload {
  std::string documentPath, defaultDefinitionId;
  std::map<ReferenceKey, ReferenceRecord> references;
  std::map<ReferenceKey, Attachment> attachments;
  std::set<ReferenceKey> invalidReferences, invalidAttachments;
  std::set<OwnerKey> invalidDrawingOwners;
  std::map<std::string, ReferenceKey> drivers;
  std::map<std::string, size_t> sourceUseCounts;
  std::map<std::string, Transform3d> placements;
  std::vector<Design::Diagnostic> diagnostics;
  bool relationsValid = true;
};
std::shared_ptr<const ReferenceScene> referenceScene(const Semantic::Scene&);
std::string sourceIdentity(const Design::DeclarationKey&);
OwnerKey instanceOwner(const std::string& path, const std::string& defaultDefinitionId);
}
