#pragma once
#include "geometry/PolySet.h"
#include "geometry/linalg.h"
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>
#include <cstdint>

struct InspectionPiece {
  uint64_t drawId = 0;
  std::optional<size_t> nodeIndex;
  std::shared_ptr<const PolySet> mesh;
  Transform3d localToWorld = Transform3d::Identity();
  Color4f color;
  enum class Role { Solid, Highlight, Background } role = Role::Solid;
};
struct InspectionMeshSet {
  std::string resultId;
  uint64_t geometryGeneration = 0;
  std::vector<InspectionPiece> pieces;
};
struct InspectionMask { uint64_t serial = 0; std::set<uint64_t> visible; };

// GUI-independent ownership and mask rules. Registrations do not activate display.
class InspectionPresentation {
public:
  using Token = uint64_t;
  Token registerProvider(std::string id, int priority);
  bool enableProvider(Token token, bool enabled);
  void removeProvider(Token token);
  Token selectedProvider() const;
  Token registerEffect(std::string id, int order);
  void removeEffect(Token token);
  std::vector<Token> orderedEffects() const;
  Token request(std::string owner);
  void release(Token token);
  bool requested() const { return !requests.empty(); }
  uint64_t beginJob();
  bool accepts(uint64_t ticket, Token provider) const;
  void invalidate();
  bool install(std::shared_ptr<const InspectionMeshSet> set, const std::string& resultId, std::string *error=nullptr);
  std::shared_ptr<const InspectionMeshSet> meshes() const { return current; }
  bool setMask(Token request, const InspectionMask& mask, const std::string& resultId, uint64_t generation);
  void clearMask(Token request);
  std::set<uint64_t> visible() const;
private:
  struct Provider { std::string id; int priority; bool enabled=true; };
  struct Effect { std::string id; int order; };
  Token nextToken=0;
  uint64_t jobTicket=0;
  std::map<Token, Provider> providers;
  std::map<Token, Effect> effects;
  std::map<Token, std::string> requests;
  std::map<Token, InspectionMask> masks;
  std::shared_ptr<const InspectionMeshSet> current;
};
