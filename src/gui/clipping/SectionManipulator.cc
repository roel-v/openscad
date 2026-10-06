#include "gui/clipping/SectionManipulator.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace Clipping {
std::optional<double> axisParameter(const Ray& ray, const Vector3d& origin, const Vector3d& axis) {
  if (!ray.origin.allFinite() || !ray.direction.allFinite() || !origin.allFinite() || !axis.allFinite() || ray.direction.norm()==0 || axis.norm()==0) return {};
  Vector3d direction=ray.direction.normalized(), normal=axis.normalized(), delta=ray.origin-origin;
  double dot=direction.dot(normal), denominator=1-dot*dot;
  if (denominator<1e-8) return {};
  double parameter=(normal.dot(delta)-dot*direction.dot(delta))/denominator;
  if ((origin+parameter*normal-ray.origin).dot(direction)<0) return {};
  return parameter;
}
std::optional<double> ringAngle(const Ray& ray, const Vector3d& origin, const Vector3d& normal, const Vector3d& up) {
  const double denominator=ray.direction.dot(normal);
  if (!std::isfinite(denominator) || std::abs(denominator)<1e-8*ray.direction.norm()) return {};
  double distance=(origin-ray.origin).dot(normal)/denominator;
  if (!std::isfinite(distance) || distance<0) return {};
  Vector3d delta=ray.origin+distance*ray.direction-origin;
  if (delta.norm()<1e-10 || !delta.allFinite()) return {};
  Vector3d transported=transportedUp(normal,up), right=transported.cross(normal.normalized());
  return std::atan2(delta.dot(transported),delta.dot(right));
}
Vector3d transportedUp(const Vector3d& normal, const Vector3d& previousUp) {
  if (!normal.allFinite() || normal.norm()==0 || !previousUp.allFinite()) throw std::invalid_argument("Cutting plane orientation must be finite");
  Vector3d n=normal.normalized(), up=previousUp-n*previousUp.dot(n);
  if (up.norm()<1e-8) {
    Eigen::Index index; n.cwiseAbs().minCoeff(&index);Vector3d fallback=Vector3d::Zero();fallback[index]=1;
    up=fallback-n*fallback.dot(n);
  }
  return up.normalized();
}
Eigen::Quaterniond orientation(const Vector3d& baseNormal, const Vector3d& baseUp, const Vector3d& normal, const Vector3d& up) {
  auto basis=[](const Vector3d& n,const Vector3d& u){Matrix3d result;result.col(2)=n.normalized();result.col(1)=transportedUp(result.col(2),u);result.col(0)=result.col(1).cross(result.col(2));return result;};
  return Eigen::Quaterniond(basis(normal,up)*basis(baseNormal,baseUp).transpose()).normalized();
}
double snappedAngle(double radians,bool snap) {const double step=std::acos(-1.)/12;return snap?std::round(radians/step)*step:radians;}
}
