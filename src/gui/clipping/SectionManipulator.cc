#include "gui/clipping/SectionManipulator.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace Clipping {
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
}
