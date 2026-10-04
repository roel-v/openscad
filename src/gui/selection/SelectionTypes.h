#pragma once
#include "geometry/linalg.h"
#include <optional>
#include <cstdint>
#include <QString>
#include <QtGlobal>

namespace Selection {
struct ObjectKey {
  QString resultId;
  uint64_t drawId = 0;
  std::optional<size_t> nodeIndex;
  bool operator==(const ObjectKey& other) const {
    return resultId == other.resultId && drawId == other.drawId && nodeIndex == other.nodeIndex;
  }
};
struct Stamp {
  QString documentId, resultId, sourceFingerprint, configurationFingerprint;
  quint64 sourceRevision = 0, viewportGeneration = 0;
  bool operator==(const Stamp& other) const {
    return documentId == other.documentId && resultId == other.resultId &&
      sourceFingerprint == other.sourceFingerprint && configurationFingerprint == other.configurationFingerprint &&
      sourceRevision == other.sourceRevision && viewportGeneration == other.viewportGeneration;
  }
};
struct Hit {
  Stamp stamp;
  ObjectKey object;
  Vector3d worldPoint, outwardNormal;
  std::optional<Transform3d> localToWorld;
  std::optional<Vector3d> localPoint;
};
enum class PickError { None, Miss, Cancelled, Stale, Unsupported, Busy, InvalidViewport, BackendFailure };
struct PickResult {
  PickError error = PickError::None;
  std::optional<Hit> hit;
  QString detail;
};
struct State { std::optional<ObjectKey> object; std::optional<Hit> surface; };
}
