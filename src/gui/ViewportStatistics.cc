#include "gui/ViewportStatistics.h"
#include "gui/QGLView.h"
#include "gui/CameraNavigation.h"
#include <numeric>

ViewportStatistics::ViewportStatistics(QGLView *view) : QLabel(view),view(view)
{
  setObjectName("viewportStatistics");
  setAttribute(Qt::WA_TransparentForMouseEvents);
  setAutoFillBackground(false);
  move(10,10);
  clock.start();
  idle.setSingleShot(true);
  connect(&idle,&QTimer::timeout,this,[this] { previous=-1; intervals.clear(); refresh(); });
  connect(&view->navigation()->settings,&NavigationSettings::changed,this,[this] {
    if (!this->view->navigation()->settings.showInformation) {idle.stop();previous=-1;intervals.clear();}
    refresh();
  });
  refresh();
}
void ViewportStatistics::frameCompleted()
{
  const auto& settings=view->navigation()->settings;
  if (!settings.showInformation) return;
  if (view->colorscheme) {
    const auto background=ColorMap::getColor(*view->colorscheme,RenderColor::BACKGROUND_COLOR);
    const double brightness=0.2126*background.r()+0.7152*background.g()+0.0722*background.b();
    auto colors=palette(); colors.setColor(QPalette::WindowText,brightness<0.5 ? Qt::white : Qt::black);
    if (colors!=palette()) setPalette(colors);
  }
  const qint64 now=clock.nsecsElapsed();
  if (previous>=0) {
    const double delta=(now-previous)/1000000.0;
    if (delta>settings.statsIdle) intervals.clear();
    else if (delta>0) intervals.append(delta);
  }
  previous=now;
  while (intervals.size()>settings.statsWindow) intervals.removeFirst();
  idle.start(settings.statsIdle);
  if (lastText<0 || (now-lastText)/1000000>=settings.statsRefresh) {
    lastText=now;
    QTimer::singleShot(0,this,&ViewportStatistics::refresh);
  }
}
void ViewportStatistics::refresh()
{
  const auto& settings=view->navigation()->settings;
  setVisible(settings.showInformation);
  if (!settings.showInformation) return;
  QStringList lines;
  const auto renderer=view->getRenderer();
  if (settings.triangles) {
    const auto stats=renderer ? renderer->triangleStatistics() : Renderer::TriangleStatistics{Renderer::TriangleStatistics::Kind::Preview,std::nullopt};
    const QString kind=stats.kind==Renderer::TriangleStatistics::Kind::Preview ? tr("preview") : tr("final");
    lines.append(tr("Triangles (%1): %2%3").arg(kind,stats.count ? QString::number(*stats.count) : tr("unavailable"),retained ? tr(" — stale") : QString()));
  }
  const double mean=intervals.isEmpty() ? 0 : std::accumulate(intervals.begin(),intervals.end(),0.0)/intervals.size();
  if (settings.fps) lines.append(mean>0 ? tr("Viewport FPS: %1").arg(1000.0/mean,0,'f',1) : tr("Viewport FPS: Idle"));
  if (settings.frameTime) lines.append(mean>0 ? tr("Frame interval: %1 ms").arg(mean,0,'f',1) : tr("Frame interval: Idle"));
  const QString next=lines.join('\n');
  if (text()!=next) { setText(next); adjustSize(); }
}
