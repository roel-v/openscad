#include "gui/selection/InspectorDock.h"
#include "gui/selection/EvaluationBinding.h"
#include "gui/selection/SelectionService.h"
#include "gui/selection/ViewportToolRouter.h"
#include "gui/MainWindow.h"
#include "gui/QGLView.h"
#include "glview/ColorMap.h"
#include "gui/annotations/AnnotationController.h"
#include "gui/Dock.h"
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QScrollArea>
#include <QMenu>
#include <algorithm>
#include <utility>
namespace Selection {
InspectorDock::Token::Token(Token&& other) noexcept : dock(other.dock),id(std::exchange(other.id,{})) {}
InspectorDock::Token& InspectorDock::Token::operator=(Token&& other) noexcept
{
  if (this!=&other) { reset(); dock=other.dock; id=std::exchange(other.id,{}); } return *this;
}
InspectorDock::Token::operator bool() const { return dock && dock->entries.count(id)!=0; }
void InspectorDock::Token::reset() { if (dock && !id.isEmpty()) dock->remove(std::exchange(id,{})); else id.clear(); }
InspectorDock::InspectorDock(MainWindow *window) : QDockWidget(tr("Inspector"),window),window(window)
{
  setObjectName("inspectorDock");
  auto body=new QWidget(this); auto layout=new QVBoxLayout(body);
  auto help=new QLabel(tr("Preview (F5), then select a surface in the viewport or a component in Model structure. "
    "The box marks the selected component, preview primitive, or final result bounds; the dot marks the picked point. "
    "Use the source links or component controls below when available."),body);
  help->setWordWrap(true);layout->addWidget(help);
  auto select=new QPushButton(tr("Select geometry"),body); select->setObjectName("selectGeometry"); layout->addWidget(select);
  auto clear=new QPushButton(tr("Clear selection"),body); layout->addWidget(clear);
  message=new QLabel(body); message->setObjectName("selectionFeedback");message->setWordWrap(true); layout->addWidget(message);
  panels=new QVBoxLayout; layout->addLayout(panels);
  properties=new QLabel(body); properties->setObjectName("geometricProperties"); properties->setWordWrap(true);
  properties->setTextInteractionFlags(Qt::TextSelectableByMouse); layout->addWidget(properties);
  contributions=new QVBoxLayout; layout->addLayout(contributions); layout->addStretch();
  auto scroll=new QScrollArea(this); scroll->setWidgetResizable(true); scroll->setWidget(body); setWidget(scroll);
  window->addDockWidget(Qt::RightDockWidgetArea,this);
  window->tabifyDockWidget(window->annotationController->dock,this); hide();
  window->menuWindow->addAction(toggleViewAction());
  selectAction=window->menu_View->addAction(tr("Select geometry")); selectAction->setObjectName("viewActionSelectGeometry");
  connect(selectAction,&QAction::triggered,this,&InspectorDock::beginSelection);
  connect(select,&QPushButton::clicked,this,&InspectorDock::beginSelection);
  connect(clear,&QPushButton::clicked,window->selectionService,&SelectionService::clear);
  connect(window->selectionService,&SelectionService::selectionChanged,this,&InspectorDock::refresh);
  connect(window->evaluationBinding,&EvaluationBinding::installed,this,&InspectorDock::refresh);
  connect(window->evaluationBinding,&EvaluationBinding::invalidated,this,&InspectorDock::refresh);
  connect(window->viewportTools,&ViewportToolRouter::feedback,this,[this](const auto& feedback) { message->setText(feedback); });
  connect(window->selectionService,&SelectionService::selectionChanged,this,[this](const State& state) {
    if (!state.object) selectionOverlay.reset();
    else if (!selectionOverlay) selectionOverlay=this->window->viewportOverlays->registerOverlay(this,OverlayLayer::WorldOverlay,50,
      [this](const OverlayContext& context){drawSelection(context);});
    this->window->qglview->update();
  });
  refresh();
}
void InspectorDock::addPanel(QWidget *panel) { panels->addWidget(panel); }
void InspectorDock::beginSelection()
{
  show(); raise();
  message->setText(tr("Click a surface. Drag to navigate, or press Escape to cancel."));
  tool=window->viewportTools->beginPick({"Inspector",window->evaluationBinding->stamp(),true},this,[this](const PickResult& result) {
    tool=0;
    if (result.error==PickError::None) message->setText(tr("Selected. Picking is finished; you can use Inspector or Sections."));
    else if (result.error==PickError::Cancelled) message->setText(tr("Selection cancelled."));
    else message->setText(result.detail.isEmpty() ? tr("Surface picking is unavailable for this display.") : result.detail);
  });
}
void InspectorDock::drawSelection(const OverlayContext& context)
{
  if(!window->evaluationBinding->current())return;
  const auto& state=window->selectionService->state();if(!state.object)return;
  bool light=false;
  if(const auto scheme=window->qglview->colorscheme){
    const auto background=scheme->find(RenderColor::BACKGROUND_COLOR);
    if(background!=scheme->end())light=background->second.r()*.2126+background->second.g()*.7152+background->second.b()*.0722>.5;
  }
  glDisable(GL_LIGHTING);glDisable(GL_TEXTURE_2D);
  // A contrasting halo keeps the selection visible against both geometry and background.
  for(int pass=0;pass<2;++pass){
    if(!pass)glColor3f(light?1:0,light?1:0,light?1:0);
    else if(light)glColor3f(0,.25f,.65f);else glColor3f(1,.85f,.1f);
    glLineWidth(static_cast<float>((pass?2:4)*context.dpr));
    if(state.bounds && !state.bounds->isEmpty() && state.bounds->min().allFinite() && state.bounds->max().allFinite()){
      glBegin(GL_LINES);
      for(int corner=0;corner<8;++corner)for(int bit=0;bit<3;++bit)if(!(corner&(1<<bit))){
        const Vector3d a=state.bounds->corner(static_cast<BoundingBox::CornerType>(corner));
        const Vector3d b=state.bounds->corner(static_cast<BoundingBox::CornerType>(corner|(1<<bit)));
        glVertex3dv(a.data());glVertex3dv(b.data());
      }glEnd();
    }
    if(state.surface){glPointSize(static_cast<float>((pass?6:10)*context.dpr));glBegin(GL_POINTS);glVertex3dv(state.surface->worldPoint.data());glEnd();}
  }
}
InspectorDock::~InspectorDock() { if (window && window->viewportTools) window->viewportTools->finish(tool); }
InspectorDock::Token InspectorDock::registerContribution(InspectorContribution contribution)
{
  if (contribution.id.isEmpty() || !contribution.lifetime || !contribution.build || entries.count(contribution.id)) return {};
  const auto id=contribution.id; auto owner=contribution.lifetime;
  entries.emplace(id,Entry{std::move(contribution),owner,{},connect(owner,&QObject::destroyed,this,[this,id] { remove(id); })});
  refresh(); return Token(this,id);
}
void InspectorDock::remove(const QString& id)
{
  auto found=entries.find(id); if (found==entries.end()) return;
  QObject::disconnect(found->second.lifetime); delete found->second.widget.data(); entries.erase(found);
}
void InspectorDock::refresh()
{
  const auto state=window->selectionService->state(); const auto stamp=window->evaluationBinding->stamp();
  if (!state.object && !tool) message->setText(tr("No selection. Choose Select geometry, then click a surface."));
  auto vector=[](const Vector3d& point) { return QString("[%1, %2, %3]").arg(point.x(),0,'g',10).arg(point.y(),0,'g',10).arg(point.z(),0,'g',10); };
  QString text=tr("Evaluation: %1\nResult: %2\nObject: %3\nWorld point: %4\nOutward normal: %5\nLocal point: %6\nLocal frame: %7\nSemantic ownership: unavailable")
    .arg(window->evaluationBinding->current() ? tr("current") : tr("stale or unavailable"),stamp.resultId.isEmpty() ? tr("unavailable") : stamp.resultId,
      state.object ? QString::number(state.object->drawId) : tr("unavailable"),state.surface ? vector(state.surface->worldPoint) : tr("unavailable"),
      state.surface ? vector(state.surface->outwardNormal) : tr("unavailable"),state.surface && state.surface->localPoint ? vector(*state.surface->localPoint) : tr("unavailable"),
      state.surface && state.surface->localToWorld ? tr("available") : tr("unavailable"));
  if (state.surface && state.surface->localToWorld) {
    const auto& matrix=state.surface->localToWorld->matrix();
    for (int row=0;row<4;++row) text+=QString("\n[%1, %2, %3, %4]").arg(matrix(row,0),0,'g',8).arg(matrix(row,1),0,'g',8).arg(matrix(row,2),0,'g',8).arg(matrix(row,3),0,'g',8);
  }
  properties->setText(text);
  std::vector<std::pair<QString,Entry>> sorted(entries.begin(),entries.end());
  std::sort(sorted.begin(),sorted.end(),[](const auto& a,const auto& b) {
    return a.second.contribution.order!=b.second.contribution.order ? a.second.contribution.order<b.second.contribution.order : a.first<b.first;
  });
  for (const auto& [id,copy] : sorted) {
    auto found=entries.find(id); if (found==entries.end() || !copy.owner) continue;
    delete found->second.widget.data(); found->second.widget.clear();
    QWidget *widget=nullptr;
    try { widget=copy.contribution.build(state,stamp); } catch (const std::exception& error) { widget=new QLabel(QString::fromUtf8(error.what()),this); }
    found=entries.find(id);
    if (found==entries.end() || !copy.owner) { delete widget; continue; }
    if (widget) { widget->setParent(this->widget()); contributions->addWidget(widget); found->second.widget=widget; }
  }
}
}
