#include "core/anchors/ReferenceKinds.h"
#include "core/anchors/ReferenceCollector.h"
#include "core/design/DesignKinds.h"
#include <algorithm>
#include <cmath>
#include <mutex>
#include <stdexcept>

namespace Anchors {
namespace {
struct Schema {
  Design::DecodeResult output;
  bool valid = true;
  void fail(const std::string& text) { valid = false; output.diagnostics.push_back({"anchor-schema",text,{}}); }
  void fields(const ObjectType& object, const std::set<std::string>& allowed) {
    for (const auto& name : object.keys()) if (!allowed.count(name)) fail("Unknown field: " + name);
    for (const auto& name : object.literalDuplicateKeys()) fail("Duplicate field: " + name);
  }
  bool vector(const Value& value, Vector3d& out, const std::string& name) {
    if (value.type() == Value::Type::VECTOR && value.toVector().size() == 3 &&
        value.getVec3(out.x(),out.y(),out.z()) && out.allFinite()) return true;
    fail(name + " requires three finite numbers"); return false;
  }
  void number(const ObjectType& object, const std::string& name, double& out) {
    if (object.contains(name) && !object.get(name).getFiniteDouble(out)) fail(name + " must be finite");
  }
  bool direction(Vector3d& value, const std::string& name) {
    const double scale = value.cwiseAbs().maxCoeff();
    if (!value.allFinite() || scale == 0) { fail(name + " must be nonzero and finite"); return false; }
    value = (value / scale).normalized(); return true;
  }
  void endpoint(const Value& value, EndpointPath& out, const std::string& name) {
    if (value.type() != Value::Type::OBJECT) { fail(name + " must be an endpoint object"); return; }
    const auto& object = value.toObject(); fields(object,{"component","anchor"});
    if (object.get("component").type() != Value::Type::STRING || object.get("anchor").type() != Value::Type::STRING || object.get("anchor").toString().empty())
      fail(name + " requires component and nonempty anchor strings");
    else { out.component = object.get("component").toString(); out.anchor = object.get("anchor").toString(); }
  }
};
}
Design::DecodeResult decodeReference(const std::string& kind, const Value& value, const Design::DeclarationContext&) {
  Schema schema;
  if (value.type() != Value::Type::OBJECT) { schema.fail("Declaration requires an object"); return std::move(schema.output); }
  const auto& object = value.toObject();
  if (kind == "Attachment") {
    auto result = std::make_shared<AttachmentDeclaration>();
    schema.fields(object,{"id","label","child","target","mode","offset","spin","adjust"});
    schema.endpoint(object.get("child"),result->child,"child"); schema.endpoint(object.get("target"),result->target,"target");
    if (object.contains("mode")) {
      const auto& mode = object.get("mode");
      if (mode.type() != Value::Type::STRING || (mode.toString() != "mate" && mode.toString() != "align")) schema.fail("mode must be mate or align");
      else result->mode = mode.toString() == "mate" ? AttachmentMode::Mate : AttachmentMode::Align;
    }
    if (object.contains("offset")) schema.vector(object.get("offset"),result->offset,"offset");
    schema.number(object,"spin",result->spin);
    if (object.contains("adjust")) {
      if (object.get("adjust").type() != Value::Type::OBJECT) schema.fail("adjust must be an object");
      else {
        const auto& adjust = object.get("adjust").toObject(); schema.fields(adjust,{"offset","spin"});
        if (adjust.contains("offset")) schema.vector(adjust.get("offset"),result->offsetAdjustment,"adjust.offset");
        schema.number(adjust,"spin",result->spinAdjustment);
      }
    }
    if (schema.valid) schema.output.payload = std::move(result);
    return std::move(schema.output);
  }
  auto result = std::make_shared<ReferenceDeclaration>();
  std::set<std::string> allowed{"id","label","owner","position"};
  if (kind == "Anchor") allowed.insert({"normal","up","spin","capture","purpose","adjust"});
  else if (kind == "Axis") allowed.insert("direction");
  else if (kind == "Plane") allowed.insert("normal");
  else if (kind != "Point") schema.fail("Unknown reference kind: " + kind);
  schema.fields(object,allowed);
  if (object.contains("owner")) {
    if (object.get("owner").type() != Value::Type::STRING) schema.fail("owner must be a Component path string");
    else result->owner = object.get("owner").toString();
  }
  Vector3d position = Vector3d::Zero(); schema.vector(object.get("position"),position,"position");
  if (kind == "Anchor") {
    Anchor anchor; anchor.position = position;
    schema.vector(object.get("normal"),anchor.normal,"normal"); schema.vector(object.get("up"),anchor.up,"up");
    if (schema.direction(anchor.normal,"normal") && schema.direction(anchor.up,"up")) {
      anchor.up -= anchor.normal * anchor.up.dot(anchor.normal);
      if (anchor.up.norm() <= 1e-10) schema.fail("normal and up must be nonparallel");
      else anchor.up.normalize();
    }
    schema.number(object,"spin",anchor.spin);
    if (object.contains("capture")) {
      const auto& capture = object.get("capture");
      if (capture.type() != Value::Type::STRING || (capture.toString() != "fixed" && capture.toString() != "parametric")) schema.fail("capture must be fixed or parametric");
      else anchor.fixedCapture = capture.toString() == "fixed";
    }
    if (object.contains("purpose")) {
      if (object.get("purpose").type() != Value::Type::STRING || object.get("purpose").toString() != "drawing") schema.fail("purpose must be drawing");
      else anchor.drawingFrame = true;
    }
    if (object.contains("adjust")) {
      if (object.get("adjust").type() != Value::Type::OBJECT) schema.fail("adjust must be an object");
      else {
        const auto& adjust = object.get("adjust").toObject(); schema.fields(adjust,{"translation","rotation"});
        if (adjust.contains("translation")) schema.vector(adjust.get("translation"),anchor.translation,"adjust.translation");
        if (adjust.contains("rotation")) {
          const auto& rotation = adjust.get("rotation"); double q[4]{};
          bool finite = rotation.type() == Value::Type::VECTOR && rotation.toVector().size() == 4;
          if (finite) for (size_t i=0;i<4;++i) finite = rotation.toVector()[i].getFiniteDouble(q[i]) && finite;
          const double scale = std::max({std::abs(q[0]),std::abs(q[1]),std::abs(q[2]),std::abs(q[3])});
          if (!finite || scale == 0) schema.fail("adjust.rotation requires a finite nonzero xyzw quaternion");
          else anchor.rotation = Eigen::Quaterniond(q[3]/scale,q[0]/scale,q[1]/scale,q[2]/scale).normalized();
        }
      }
    }
    result->value = anchor;
  } else if (kind == "Axis") {
    Axis axis; axis.position = position; schema.vector(object.get("direction"),axis.direction,"direction"); schema.direction(axis.direction,"direction"); result->value = axis;
  } else if (kind == "Plane") {
    Plane plane; plane.position = position; schema.vector(object.get("normal"),plane.normal,"normal"); schema.direction(plane.normal,"normal"); result->value = plane;
  } else result->value = Point{position};
  if (schema.valid) schema.output.payload = std::move(result);
  return std::move(schema.output);
}
void registerKinds() {
  static std::once_flag once;
  std::call_once(once,[] {
    Design::registerBuiltins();
    for (const std::string name : {"Anchor","Point","Axis","Plane","Attachment"}) {
      Design::KindHandler handler;
      handler.name = name; handler.hosts = {Design::DeclarationHost::Assignment};
      handler.scopes = {Design::DeclarationScope::File,Design::DeclarationScope::Local};
      handler.destination = Design::RecordDestination::Consumer;
      handler.decode = [name](const Value& value,const Design::DeclarationContext& context) { return decodeReference(name,value,context); };
      handler.consume = collectReference;
      handler.reject = [name](const auto& diagnostics,const auto& declaration,auto& session) { rejectReference(name,diagnostics,declaration,session); };
      if (!Design::registerKind(std::move(handler))) throw std::logic_error("Duplicate reference kind: " + name);
    }
    registerCollectionPass();
  });
}
}
