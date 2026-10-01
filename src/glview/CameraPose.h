#pragma once

#include "glview/Camera.h"
#include <Eigen/Geometry>
#include <cmath>

// Only camera values belong in a saved view. Execution flags and viewport dimensions do not.
struct CameraPose {
  Eigen::Vector3d rotation, translation;
  double distance, fov;
  Camera::ProjectionType projection;

  static CameraPose from(const Camera& camera)
  {
    return {camera.object_rot, camera.object_trans, camera.viewer_distance, camera.fov,
            camera.projection};
  }
  void apply(Camera& camera) const
  {
    camera.object_rot = rotation;
    camera.object_trans = translation;
    camera.viewer_distance = distance;
    camera.fov = fov;
    camera.projection = projection;
  }
  bool valid() const
  {
    return rotation.allFinite() && translation.allFinite() && std::isfinite(distance) &&
           distance > 0 && std::isfinite(fov) && fov > 0 && fov < 180;
  }
  bool operator==(const CameraPose& other) const
  {
    return rotation == other.rotation && translation == other.translation &&
           distance == other.distance && fov == other.fov && projection == other.projection;
  }

  static constexpr double radians = 3.14159265358979323846 / 180.0;
  // GLView applies Rx * Ry * Rz before looking along +Y with +Z up.
  static Eigen::Matrix3d matrix(const Eigen::Vector3d& angles)
  {
    return (Eigen::AngleAxisd(angles.x() * radians, Eigen::Vector3d::UnitX()) *
            Eigen::AngleAxisd(angles.y() * radians, Eigen::Vector3d::UnitY()) *
            Eigen::AngleAxisd(angles.z() * radians, Eigen::Vector3d::UnitZ())).toRotationMatrix();
  }
  static Eigen::Vector3d angles(const Eigen::Matrix3d& rotation)
  {
    return rotation.eulerAngles(0, 1, 2) / radians;
  }
  static Eigen::Vector3d upright(const Eigen::Vector3d& outward)
  {
    const Eigen::Vector3d normal = outward.normalized();
    Eigen::Vector3d right = Eigen::Vector3d::UnitZ().cross(normal);
    if (right.squaredNorm() < 1e-12) right = Eigen::Vector3d::UnitX();
    else right.normalize();
    Eigen::Matrix3d rotation;
    rotation.row(0) = right.transpose();
    rotation.row(1) = -normal.transpose();
    rotation.row(2) = normal.cross(right).transpose();
    return angles(rotation);
  }
};
