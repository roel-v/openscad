#include "core/design/DesignKinds.h"
#include <cmath>
#include <mutex>
#include <stdexcept>

namespace Design {
namespace {
std::map<std::string, std::shared_ptr<const KindHandler>>& handlers() {
  static std::map<std::string, std::shared_ptr<const KindHandler>> value = [] {
    std::map<std::string, std::shared_ptr<const KindHandler>> builtins;
    builtins.emplace("Section",std::make_shared<const KindHandler>(KindHandler{"Section",{DeclarationHost::Assignment},{DeclarationScope::File},decodeSection}));
    builtins.emplace("View",std::make_shared<const KindHandler>(KindHandler{"View",{DeclarationHost::Assignment},{DeclarationScope::File},decodeView}));
    return builtins;
  }();
  return value;
}
std::mutex& handlerMutex() { static std::mutex value; return value; }
bool number(const Value& value, double& out) { return value.getFiniteDouble(out); }
bool vector3(const Value& value, Vector3d& out) {
  return value.type() == Value::Type::VECTOR && value.toVector().size() == 3 &&
    value.getVec3(out.x(), out.y(), out.z()) && out.allFinite();
}
bool oriented(Vector3d& normal, Vector3d& up) {
  if (normal.norm() < 1e-12 || up.norm() < 1e-12) return false;
  normal.normalize(); up -= normal * up.dot(normal);
  if (up.norm() < 1e-12) return false;
  up.normalize(); return true;
}
bool members(const ObjectType& object, const std::set<std::string>& allowed, DecodeResult& output) {
  bool valid = true;
  for (const auto& key : object.keys()) if (!allowed.count(key)) {
    output.diagnostics.push_back({"unknown-member", "Unknown design member: " + key, {}}); valid = false;
  }
  return valid;
}
bool references(const ObjectType& object, ReferenceToken& target, ReferenceToken& frame, DecodeResult& output) {
  auto t = referenceToken(object.get("target"), "document"), f = referenceToken(object.get("frame"), "world");
  if (!t || !f) { output.diagnostics.push_back({"invalid-reference", "Reference requires a domain string and optional id string", {}}); return false; }
  target = std::move(*t); frame = std::move(*f);
  auto clone = [](const ReferenceToken& token) { return ReferenceToken{token.domain, token.id, ownedValue(token.data)}; };
  output.dependencies.push_back(clone(target)); output.dependencies.push_back(clone(frame)); return true;
}
void invalid(DecodeResult& output, const std::string& message) { output.diagnostics.push_back({"invalid-schema", message, {}}); }
}
bool registerKind(KindHandler handler) {
  if (handler.name.empty() || !handler.decode || handler.hosts.empty() || handler.scopes.empty() ||
      (handler.destination == RecordDestination::Consumer) != bool(handler.consume) ||
      (handler.destination != RecordDestination::Consumer && handler.reject)) return false;
  std::lock_guard<std::mutex> lock(handlerMutex());
  auto name = handler.name;
  return handlers().emplace(name, std::make_shared<const KindHandler>(std::move(handler))).second;
}
std::shared_ptr<const KindHandler> kind(const std::string& name) {
  std::lock_guard<std::mutex> lock(handlerMutex()); auto found = handlers().find(name);
  return found == handlers().end() ? nullptr : found->second;
}
void registerBuiltins() {
  std::lock_guard<std::mutex> lock(handlerMutex()); (void)handlers();
}
std::optional<ReferenceToken> referenceToken(const Value& value, const std::string& defaultDomain) {
  if (value.type() == Value::Type::UNDEFINED) return ReferenceToken{defaultDomain, "", Value::undefined.clone()};
  if (value.type() != Value::Type::OBJECT) return {};
  const auto& object = value.toObject();
  if (object.get("domain").type() != Value::Type::STRING || object.get("domain").toString().empty() ||
      (object.contains("id") && object.get("id").type() != Value::Type::STRING)) return {};
  try { return ReferenceToken{object.get("domain").toString(), object.contains("id") ? object.get("id").toString() : "", ownedValue(value)}; }
  catch (const std::invalid_argument&) { return {}; }
}
DecodeResult decodeSection(const Value& value, const DeclarationContext&) {
  DecodeResult output; const auto& object = value.toObject(); auto section = std::make_shared<Section>();
  bool valid = members(object, {"id","label","origin","normal","up","adjust"}, output);
  if (!vector3(object.get("origin"), section->origin) || !vector3(object.get("normal"), section->normal)) {
    invalid(output, "Section requires finite origin[3] and normal[3]"); valid = false;
  }
  if (object.contains("up")) {
    if (!vector3(object.get("up"), section->up)) { invalid(output, "Section up must be finite up[3]"); valid = false; }
  } else section->up = section->normal.cross(Vector3d::UnitZ()).norm() < 1e-12 ? Vector3d::UnitY() : Vector3d::UnitZ();
  if (!oriented(section->normal, section->up)) { invalid(output, "Section normal and up must be nonzero and nonparallel"); valid = false; }
  if (object.contains("adjust")) {
    const auto& adjustValue = object.get("adjust");
    if (adjustValue.type() != Value::Type::OBJECT) { invalid(output, "Section adjust must be an object"); valid = false; }
    else {
      const auto& adjust = adjustValue.toObject();
      valid = members(adjust, {"translation","rotation","offset"}, output) && valid;
      if (adjust.contains("translation") && !vector3(adjust.get("translation"), section->translation)) { invalid(output, "Translation must be finite translation[3]"); valid = false; }
      if (adjust.contains("offset") && !number(adjust.get("offset"), section->offset)) { invalid(output, "Offset must be finite"); valid = false; }
      if (adjust.contains("rotation")) {
        const auto& q = adjust.get("rotation"); double components[4]{};
        bool finite = q.type() == Value::Type::VECTOR && q.toVector().size() == 4;
        if (finite) for (size_t i = 0; i < 4; ++i) finite = number(q.toVector()[i], components[i]) && finite;
        Eigen::Quaterniond rotation(components[3], components[0], components[1], components[2]);
        if (!finite || rotation.norm() < 1e-12) { invalid(output, "Rotation must be a finite nonzero xyzw quaternion"); valid = false; }
        else section->rotation = rotation.normalized();
      }
    }
  }
  if (valid) output.payload = std::move(section);
  return output;
}
DecodeResult decodeView(const Value& value, const DeclarationContext&) {
  DecodeResult output; const auto& object = value.toObject(); auto view = std::make_shared<View>();
  bool valid = members(object, {"id","label","target","frame","projection","direction","up","margin","camera"}, output);
  valid = references(object, view->target, view->frame, output) && valid;
  if (object.contains("projection")) {
    const auto& projection = object.get("projection"); auto text = projection.toString();
    if (projection.type() != Value::Type::STRING || (text != "orthographic" && text != "perspective")) { invalid(output, "Invalid View projection"); valid = false; }
    view->perspective = text == "perspective";
  }
  view->fixed = object.contains("camera");
  if (view->fixed) {
    if (object.contains("direction") || object.contains("up") || object.contains("margin")) { invalid(output, "Fixed camera excludes fit fields"); valid = false; }
    const auto& cameraValue = object.get("camera");
    if (cameraValue.type() != Value::Type::OBJECT) { invalid(output, "Camera must be an object"); valid = false; }
    else {
      const auto& camera = cameraValue.toObject();
      valid = members(camera, {"rotation","translation","distance","fov"}, output) && valid;
      if (!vector3(camera.get("rotation"), view->cameraRotation) || !vector3(camera.get("translation"), view->cameraTranslation) ||
          !number(camera.get("distance"), view->distance) || view->distance <= 0 || !number(camera.get("fov"), view->fov) || view->fov <= 0 || view->fov >= 180) {
        invalid(output, "Camera requires finite pose, positive distance and fov between 0 and 180"); valid = false;
      }
    }
  } else {
    if (!vector3(object.get("direction"), view->direction) || !vector3(object.get("up"), view->up) || !oriented(view->direction, view->up)) {
      invalid(output, "Fit View requires finite nonzero nonparallel direction and up"); valid = false;
    }
    if (object.contains("margin") && (!number(object.get("margin"), view->margin) || view->margin < 0)) { invalid(output, "Fit margin must be finite and nonnegative"); valid = false; }
  }
  if (valid) output.payload = std::move(view);
  return output;
}
}
