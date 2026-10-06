#include "glview/InspectionPresentation.h"
#include "glview/SurfacePickMath.h"
#include <algorithm>
#include <iterator>

InspectionPresentation::Token InspectionPresentation::registerProvider(std::string id, int priority)
{
  if (id.empty()) return 0;
  for (const auto& [token, provider] : providers)
    if (provider.id == id || provider.priority == priority) return 0;
  const auto previous=selectedProvider(), token=++nextToken;
  providers.emplace(token, Provider{std::move(id),priority});
  if (previous != selectedProvider()) invalidate();
  return token;
}
bool InspectionPresentation::enableProvider(Token token, bool enabled)
{
  auto found=providers.find(token);
  if (found==providers.end()) return false;
  const auto previous=selectedProvider();
  found->second.enabled=enabled;
  if (previous!=selectedProvider()) invalidate();
  return true;
}
void InspectionPresentation::removeProvider(Token token)
{
  const auto previous=selectedProvider(); providers.erase(token);
  if (previous!=selectedProvider()) invalidate();
}
InspectionPresentation::Token InspectionPresentation::selectedProvider() const
{
  Token selected=0;
  for (const auto& [token, provider] : providers)
    if (provider.enabled && (!selected || provider.priority > providers.at(selected).priority)) selected=token;
  return selected;
}
InspectionPresentation::Token InspectionPresentation::registerEffect(std::string id, int order)
{
  if (id.empty()) return 0;
  for (const auto& [token,effect] : effects) if (effect.id==id) return 0;
  const auto token=++nextToken; effects.emplace(token,Effect{std::move(id),order}); return token;
}
void InspectionPresentation::removeEffect(Token token) { effects.erase(token); }
std::vector<InspectionPresentation::Token> InspectionPresentation::orderedEffects() const
{
  std::vector<Token> result;
  for (const auto& [token,effect] : effects) result.push_back(token);
  std::sort(result.begin(),result.end(),[this](Token a,Token b) {
    const auto& first=effects.at(a); const auto& second=effects.at(b);
    return first.order==second.order ? first.id<second.id : first.order<second.order;
  });
  return result;
}
InspectionPresentation::Token InspectionPresentation::request(std::string owner)
{
  if (owner.empty()) return 0;
  for (const auto& [token,name] : requests) if (name==owner) return 0;
  const auto token=++nextToken; requests.emplace(token,std::move(owner)); return token;
}
void InspectionPresentation::release(Token token)
{
  requests.erase(token); masks.erase(token);
  if (requests.empty()) invalidate();
}
uint64_t InspectionPresentation::beginJob() { return ++jobTicket; }
bool InspectionPresentation::accepts(uint64_t ticket, Token provider) const
{
  return requested() && provider && provider==selectedProvider() && ticket==jobTicket;
}
void InspectionPresentation::invalidate() { ++jobTicket; current.reset(); masks.clear(); }
bool InspectionPresentation::install(std::shared_ptr<const InspectionMeshSet> set, const std::string& resultId, std::string *error)
{
  auto fail=[error](const char *text) { if (error) *error=text; return false; };
  if (!set || set->resultId!=resultId || !set->geometryGeneration) return fail("Wrong inspection result or generation");
  if (set->pieces.size()>SurfacePickMath::MaxDrawId) return fail("Inspection draw token capacity exceeded");
  std::set<uint64_t> ids;
  for (const auto& piece : set->pieces) {
    if (!piece.drawId || !ids.insert(piece.drawId).second) return fail("Inspection draw IDs must be nonzero and unique");
    if (!piece.mesh || piece.mesh->getDimension()!=3 || !piece.localToWorld.matrix().allFinite() ||
        (piece.localToWorld.matrix().row(3)-Eigen::RowVector4d(0,0,0,1)).norm()>1e-12)
      return fail("Invalid inspection mesh or affine transform");
  }
  current=std::move(set); masks.clear(); return true;
}
bool InspectionPresentation::setMask(Token token, const InspectionMask& mask, const std::string& resultId, uint64_t generation)
{
  if (!requests.count(token) || !current || current->resultId!=resultId || current->geometryGeneration!=generation) return false;
  std::set<uint64_t> ids;
  for (const auto& piece : current->pieces) ids.insert(piece.drawId);
  if (!std::includes(ids.begin(),ids.end(),mask.visible.begin(),mask.visible.end())) return false;
  if (auto previous=masks.find(token); previous!=masks.end() && mask.serial<=previous->second.serial) return false;
  masks[token]=mask; return true;
}
void InspectionPresentation::clearMask(Token token) { masks.erase(token); }
std::set<uint64_t> InspectionPresentation::visible() const
{
  std::set<uint64_t> result;
  if (!current) return result;
  for (const auto& piece : current->pieces) result.insert(piece.drawId);
  for (const auto& [token,mask] : masks) {
    std::set<uint64_t> intersection;
    std::set_intersection(result.begin(),result.end(),mask.visible.begin(),mask.visible.end(),std::inserter(intersection,intersection.end()));
    result=std::move(intersection);
  }
  return result;
}
