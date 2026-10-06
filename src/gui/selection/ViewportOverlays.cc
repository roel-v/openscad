#include "gui/selection/ViewportOverlays.h"
#include "gui/selection/EvaluationBinding.h"
#include "gui/MainWindow.h"
#include "gui/QGLView.h"
#include "glview/ScopedGLState.h"
#include <algorithm>
#include <utility>
namespace Selection {
ViewportOverlays::Token::Token(Token&& other) noexcept : registry(other.registry),id(std::exchange(other.id,0)) {}
ViewportOverlays::Token& ViewportOverlays::Token::operator=(Token&& other) noexcept
{
  if (this!=&other) { reset(); registry=other.registry; id=std::exchange(other.id,0); } return *this;
}
void ViewportOverlays::Token::reset() { if (registry && id) registry->remove(std::exchange(id,0)); else id=0; }
ViewportOverlays::Token::operator bool() const { return registry && registry->entries.count(id)!=0; }
ViewportOverlays::ViewportOverlays(MainWindow *window) : QObject(window->qglview),window(window) {}
ViewportOverlays::Token ViewportOverlays::registerOverlay(QObject *owner,OverlayLayer layer,int order,std::function<void(const OverlayContext&)> draw)
{
  if (!owner || !draw) return {};
  const auto id=++next;
  entries.emplace(id,Entry{owner,layer,order,std::move(draw),connect(owner,&QObject::destroyed,this,[this,id] { remove(id); })});
  window->qglview->update(); return Token(this,id);
}
void ViewportOverlays::remove(uint64_t id)
{
  auto found=entries.find(id); if (found==entries.end()) return;
  QObject::disconnect(found->second.lifetime); entries.erase(found); window->qglview->update();
}
void ViewportOverlays::draw()
{
  auto view=window->qglview;
  const OverlayContext context{window->evaluationBinding->stamp(),Eigen::Map<Eigen::Matrix4d>(view->modelview),
    Eigen::Map<Eigen::Matrix4d>(view->projection),view->size(),view->devicePixelRatio()};
  std::vector<std::pair<uint64_t,Entry>> sorted(entries.begin(),entries.end());
  std::sort(sorted.begin(),sorted.end(),[](const auto& a,const auto& b) {
    return a.second.layer!=b.second.layer ? a.second.layer<b.second.layer :
      a.second.order!=b.second.order ? a.second.order<b.second.order : a.first<b.first;
  });
  for (const auto& [id,entry] : sorted) {
    if (!entry.owner || !entries.count(id)) continue;
    ScopedGLState saved;
    glUseProgram(0); glMatrixMode(GL_PROJECTION); glLoadMatrixd(context.projection.data());
    glMatrixMode(GL_MODELVIEW); glLoadMatrixd(context.modelview.data());
    glDepthMask(GL_FALSE);
    if (entry.layer==OverlayLayer::WorldDepth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (entry.layer==OverlayLayer::HUD) {
      glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0,context.logicalViewport.width(),context.logicalViewport.height(),0,-1,1);
      glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    }
    try { entry.draw(context); } catch (const std::exception& error) { qWarning("Viewport overlay failed: %s",error.what()); }
  }
}
}
