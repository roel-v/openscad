#include "core/design/DesignReferences.h"
#include "core/design/DesignKinds.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <mutex>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>

namespace Design {
namespace {
std::map<std::string, Resolver>& resolvers() { static std::map<std::string, Resolver> value; return value; }
std::mutex& resolverMutex() { static std::mutex value; return value; }
Resolution fail(ResolutionState state, std::string message) { return {state, {}, {}, std::move(message)}; }
bool rigid(const Transform3d& frame) {
  return frame.matrix().allFinite() && (frame.linear().transpose() * frame.linear()).isApprox(Matrix3d::Identity(), 1e-9) && std::abs(frame.linear().determinant() - 1) < 1e-9;
}
}
bool registerResolver(std::string domain, Resolver resolver) {
  if (domain.empty() || !resolver || domain == "world" || domain == "document" || domain == "design" || domain == "geometric") return false;
  std::lock_guard<std::mutex> lock(resolverMutex()); return resolvers().emplace(std::move(domain), std::move(resolver)).second;
}
Resolution resolve(const ReferenceToken& token, ResolveContext& context) {
  if (token.domain == "world") return {ResolutionState::Resolved, Transform3d::Identity(), {}, ""};
  if (token.domain == "document") return {ResolutionState::Resolved, Transform3d::Identity(), context.documentBounds, ""};
  if (token.domain == "design") {
    if (std::find(context.visitedIds.begin(), context.visitedIds.end(), token.id) != context.visitedIds.end()) return fail(ResolutionState::Invalid, "Cyclic design reference: " + token.id);
    auto found = context.records.find(token.id);
    if (found == context.records.end()) return fail(ResolutionState::Missing, "Missing design id: " + token.id);
    context.visitedIds.push_back(token.id);
    struct Pop { std::vector<std::string>& stack; ~Pop() { stack.pop_back(); } } pop{context.visitedIds};
    for(const auto& dependency:found->second->dependencies)if(dependency.domain=="design"){
      auto resolution=resolve(dependency,context);if(resolution.state!=ResolutionState::Resolved)return resolution;
    }
    if (found->second->kind == "Section") return sectionFrame(*found->second, context);
    if (auto view = std::dynamic_pointer_cast<const View>(found->second->evaluated)) return resolve(view->frame, context);
    return fail(ResolutionState::Unsupported, "Design kind has no frame resolver: " + found->second->kind);
  }
  if (token.domain == "geometric") {
    if (token.data.type() != Value::Type::OBJECT) return fail(ResolutionState::Invalid, "Geometric reference requires captured data");
    const auto& data = token.data.toObject();
    if (data.get("inputsFingerprint").type() != Value::Type::STRING || data.get("configurationFingerprint").type() != Value::Type::STRING) return fail(ResolutionState::Invalid, "Geometric reference requires input and configuration fingerprints");
    if (data.get("inputsFingerprint").toString() != context.evaluatedInputsFingerprint || data.get("configurationFingerprint").toString() != context.configurationFingerprint) return fail(ResolutionState::Changed, "Geometric reference inputs changed; deliberately rebind it");
    Vector3d point, normal, up;
    auto vec = [](const Value& value, Vector3d& out) { return value.type() == Value::Type::VECTOR && value.toVector().size() == 3 && value.getVec3(out.x(), out.y(), out.z()) && out.allFinite(); };
    if (!vec(data.get("point"), point) || !vec(data.get("normal"), normal) || normal.norm() < 1e-12) return fail(ResolutionState::Invalid, "Invalid geometric point or normal");
    normal.normalize(); up = normal.cross(Vector3d::UnitZ()).norm() < 1e-12 ? Vector3d::UnitY() : Vector3d::UnitZ();
    up = (up - normal * up.dot(normal)).normalized();
    Transform3d frame = Transform3d::Identity(); frame.translation() = point;
    frame.linear().col(0) = up.cross(normal); frame.linear().col(1) = up; frame.linear().col(2) = normal;
    return {ResolutionState::Resolved, frame, {}, ""};
  }
  Resolver resolver;
  { std::lock_guard<std::mutex> lock(resolverMutex()); auto found = resolvers().find(token.domain); if (found != resolvers().end()) resolver = found->second; }
  return resolver ? resolver(token, context) : fail(ResolutionState::Unsupported, "No resolver for domain: " + token.domain);
}
Resolution sectionFrame(const Record& record, ResolveContext&) {
  auto section = std::dynamic_pointer_cast<const Section>(record.evaluated);
  if (!section) return fail(ResolutionState::Invalid, "Record is not a valid Section");
  Vector3d normal = section->rotation * section->normal, up = section->rotation * section->up;
  Transform3d local = Transform3d::Identity();
  local.translation() = section->origin + section->translation + section->offset * normal;
  local.linear().col(0) = up.cross(normal); local.linear().col(1) = up; local.linear().col(2) = normal;
  return {ResolutionState::Resolved, local, {}, ""};
}
std::optional<Camera> viewCamera(const Record& record, ResolveContext& context, std::string *diagnostic) {
  auto failCamera = [&](const std::string& message) -> std::optional<Camera> { if (diagnostic) *diagnostic = message; return {}; };
  auto view = std::dynamic_pointer_cast<const View>(record.evaluated);
  if (!view) return failCamera("Record is not a valid View");
  Camera camera; camera.setProjection(view->perspective ? Camera::ProjectionType::PERSPECTIVE : Camera::ProjectionType::ORTHOGONAL);
  auto frame = resolve(view->frame, context), target = resolve(view->target, context);
  if (frame.state != ResolutionState::Resolved || !frame.frame || !rigid(*frame.frame)) return failCamera(frame.diagnostic.empty() ? "View frame must be rigid" : frame.diagnostic);
  if (target.state != ResolutionState::Resolved) return failCamera(target.diagnostic);
  if (view->fixed) {
    camera.setVpr(view->cameraRotation.x(), view->cameraRotation.y(), view->cameraRotation.z()); camera.setVpt(view->cameraTranslation.x(), view->cameraTranslation.y(), view->cameraTranslation.z());
    camera.setVpd(view->distance); camera.setVpf(view->fov);
    if(frame.frame->matrix().isIdentity(1e-12))return camera;
    const double radians=std::acos(-1.0)/180;
    Matrix3d local=(Eigen::AngleAxisd(camera.object_rot.x()*radians,Vector3d::UnitX()) * Eigen::AngleAxisd(camera.object_rot.y()*radians,Vector3d::UnitY()) * Eigen::AngleAxisd(camera.object_rot.z()*radians,Vector3d::UnitZ())).toRotationMatrix();
    camera.object_rot=(local*frame.frame->linear().transpose()).eval().eulerAngles(0,1,2)/radians;
    camera.object_trans=-(*frame.frame * view->cameraTranslation);
    return camera;
  }
  if (target.state != ResolutionState::Resolved || !target.bounds || target.bounds->isEmpty() || !target.bounds->min().allFinite() || !target.bounds->max().allFinite()) return failCamera(target.diagnostic.empty() ? "View target bounds are unavailable" : target.diagnostic);
  Vector3d direction = frame.frame->linear() * view->direction, up = frame.frame->linear() * view->up;
  Matrix3d orientation; orientation.row(0) = direction.cross(up).transpose(); orientation.row(1) = direction.transpose(); orientation.row(2) = up.transpose();
  camera.object_rot = orientation.eulerAngles(0,1,2) * (180 / std::acos(-1.0));
  Vector3d expansion = target.bounds->sizes() * view->margin;
  BoundingBox expanded(target.bounds->min() - expansion, target.bounds->max() + expansion);
  camera.autocenter = true; camera.viewAll(expanded); return camera;
}
std::string declarationName(const Record& record) {
  if (record.source && !record.source->hostName.empty()) return record.source->hostName;
  std::string name = record.kind == "Section" ? "section_" : "view_";
  for (unsigned char ch : record.id) name += ch < 128 && std::isalnum(ch) ? char(ch) : '_';
  return name;
}
ClosurePlan persistenceClosure(const std::vector<std::string>& requested, const Records& records, const std::set<std::string>& includedFiles, const std::set<std::string>& destinationVariables) {
  ClosurePlan plan; std::map<std::string, int> visited; std::set<std::string> names = destinationVariables;
  std::function<void(const std::string&)> visit = [&](const std::string& id) {
    if (visited[id] == 2) return;
    if (visited[id] == 1) { plan.diagnostics.push_back({"dependency-cycle", "Cyclic design dependency: " + id, {}}); return; }
    auto found = records.find(id);
    if (found == records.end()) { plan.diagnostics.push_back({"missing-id", "Missing dependency: " + id, {}}); return; }
    auto record = found->second; auto handler = kind(record->kind);
    if (!handler) { plan.diagnostics.push_back({"unknown-kind", "No handler for: " + record->kind, record->source}); return; }
    if (record->source && record->source->host != DeclarationHost::Assignment) { plan.diagnostics.push_back({"unsupported-host", "Assignment persistence requires an assignment declaration", record->source}); return; }
    visited[id] = 1;
    for (const auto& dependency : record->dependencies) if (dependency.domain == "design") visit(dependency.id);
    visited[id] = 2;
    if (record->source) {
      if (!includedFiles.count(record->source->fileKey)) plan.diagnostics.push_back({"missing-include", "Source dependency is outside the current include closure: " + record->source->fileKey, record->source});
      else plan.sourced.push_back(record);
    } else if (!names.insert(declarationName(*record)).second) plan.diagnostics.push_back({"variable-collision", "Destination variable already exists: " + declarationName(*record), {}});
    else plan.insert.push_back(record);
  };
  for (const auto& id : requested) visit(id);
  if (!plan.valid()) { plan.insert.clear(); plan.sourced.clear(); }
  return plan;
}
std::string serialize(const Record& record) {
  if (record.kind != "Section" && record.kind != "View") throw std::invalid_argument("Unsupported assignment serialization kind");
  std::ostringstream stream; stream.imbue(std::locale::classic());stream<<std::setprecision(std::numeric_limits<double>::max_digits10);
  std::function<void(const Value&)> data=[&](const Value& value){
    if(value.type()==Value::Type::NUMBER){double number; if(!value.getFiniteDouble(number))throw std::invalid_argument("Nonfinite declaration data");stream<<number;}
    else if(value.type()==Value::Type::OBJECT){stream<<"{";bool first=true;for(const auto& key:value.toObject().keys()){if(!first)stream<<", ";first=false;stream<<QuotedString(key)<<": ";data(value.toObject().get(key));}stream<<"}";}
    else if(value.type()==Value::Type::VECTOR){stream<<"[";bool first=true;for(const auto& item:value.toVector()){if(!first)stream<<", ";first=false;data(item);}stream<<"]";}
    else stream<<value;
  };
  stream << "@{type: " << record.kind << "}\n" << declarationName(record) << " = ";data(record.definition);stream<<";\n"; return stream.str();
}
}
