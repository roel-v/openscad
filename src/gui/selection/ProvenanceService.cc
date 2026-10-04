#include "gui/selection/ProvenanceService.h"
#include "gui/selection/EvaluationBinding.h"
#include "gui/MainWindow.h"
#include "gui/OpenSCADApp.h"
#include "gui/TabManager.h"
#include "gui/ScintillaEditor.h"
#include <QLabel>
#include <QPushButton>
#include <QComboBox>
#include <QVBoxLayout>
#include <QTimer>
#include <QStatusBar>
#include <QThread>
#include <limits>
#include <algorithm>

namespace {
QSet<EditorInterface *> editors(MainWindow& window) {
  auto result = window.tabManager->editorList;
  if (auto app = qobject_cast<OpenSCADApp *>(QCoreApplication::instance()))
    for (auto other : app->windowManager.getWindows()) result.unite(other->tabManager->editorList);
  return result;
}
std::string key(EditorInterface& editor) {
  return editor.filepath.isEmpty() ? "session:" + editor.documentId().toStdString()
    : EvaluationInputs::fileKey(std::filesystem::path(editor.filepath.toStdString()));
}
bool same(const SourceProvenance::Span& a, const SourceProvenance::Span& b) {
  return a.fileKey == b.fileKey && a.fingerprint == b.fingerprint && a.begin == b.begin && a.end == b.end;
}
bool contains(const SourceProvenance::Site& site, const SourceProvenance::Span& span) {
  if ((site.wholeComplete && same(site.whole, span)) || (site.head && same(*site.head, span))) return true;
  for (const auto& argument : site.arguments)
    if (same(argument.whole, span) || (argument.hasExpression && same(argument.expression, span)) ||
        (argument.nameSpan && same(*argument.nameSpan, span))) return true;
  return false;
}
QString fileLabel(const SourceProvenance::Site& site) {
  const auto& span = site.head ? *site.head : site.whole;
  return QString::fromStdString(span.fileKey) + QString(" · bytes %1–%2").arg(span.begin).arg(span.end);
}
}

namespace SourceProvenance {
ProvenanceService::ProvenanceService(MainWindow *window) : QObject(window), window(window) {
  setObjectName("provenanceService");
  contribution = window->inspectorDock->registerContribution({"source-provenance", 10, this,
    [this](const auto& state, const auto& stamp) { return build(state, stamp); }});
}
ProvenanceService *ProvenanceService::forWindow(MainWindow *window) {
  return window ? dynamic_cast<ProvenanceService *>(window->findChild<QObject *>("provenanceService", Qt::FindDirectChildrenOnly)) : nullptr;
}
Resolved ProvenanceService::resolve(const Selection::Hit& hit) const {
  Resolved resolved{hit, {}, {}};
  QString detail;
  if (!window || !window->evaluationBinding->validate(hit.stamp, &detail) || hit.object.resultId != hit.stamp.resultId) {
    resolved.mapping.confidence = Confidence::Stale;
    resolved.mapping.reason = detail.isEmpty() ? "Evaluation or object identity changed" : detail.toStdString();
    return resolved;
  }
  auto scene = window->evaluationBinding->scene(hit.stamp);
  if (!scene || !scene->provenance) {
    resolved.mapping.reason = "This result has no captured source provenance";
    return resolved;
  }
  resolved.result = scene->provenance;
  if (window->isPreview && hit.object.nodeIndex) {
    resolved.mapping = resolved.result->forNode(*hit.object.nodeIndex);
    if (hit.localToWorld && (!resolved.mapping.localToWorld ||
        !resolved.mapping.localToWorld->matrix().isApprox(hit.localToWorld->matrix(), 1e-10))) {
      resolved.mapping.confidence = Confidence::Unavailable;
      resolved.mapping.candidateInstances.clear();
      resolved.mapping.invertible = false;
      resolved.mapping.localToWorld.reset();
      resolved.mapping.reason = "The displayed transform has no matching source coordinate frame";
    }
  } else {
    resolved.mapping = resolved.result->finalGeometry();
  }
  return resolved;
}

std::vector<uint64_t> ProvenanceService::navigationCandidates(const Resolved& resolved) const {
  std::vector<uint64_t> candidates;
  if (!resolved.result) return candidates;
  for (const auto emitting : resolved.mapping.candidateInstances) {
    auto instance = resolved.result->instance(emitting);
    while (instance) {
      if (std::find(candidates.begin(), candidates.end(), instance->id) == candidates.end()) candidates.push_back(instance->id);
      instance = instance->parent ? resolved.result->instance(*instance->parent) : nullptr;
    }
  }
  return candidates;
}

std::optional<EditableTarget> ProvenanceService::editableSpan(const Resolved& resolved, uint64_t id,
  const Span& span, const QByteArray& replacement, QString *detail) const {
  auto fail = [detail](const QString& reason) -> std::optional<EditableTarget> {
    if (detail) *detail = reason; return {};
  };
  const auto current = resolve(resolved.hit);
  const auto candidates = navigationCandidates(current);
  if (!current.result || current.result != resolved.result || current.mapping.confidence != Confidence::Unique ||
      std::find(candidates.begin(), candidates.end(), id) == candidates.end())
    return fail(tr("A current, unique source invocation is required"));
  const auto instance = current.result->instance(id);
  if (!instance || (!contains(instance->call, span) && (!instance->definition || !contains(*instance->definition, span))))
    return fail(tr("Range is not part of this invocation's recorded syntax"));
  const auto bytes = current.result->syntaxTable().original(span);
  if (!bytes || span.end > static_cast<uint64_t>(std::numeric_limits<qsizetype>::max()))
    return fail(tr("Captured source range is unavailable"));
  auto inputs = current.result->syntaxTable().inputs();
  const auto source = inputs->consumed().find(span.fileKey);
  if (source == inputs->consumed().end() || !source->second.editorId || !source->second.editorRevision)
    return fail(tr("Open this source and evaluate again before editing it"));
  QPointer<EditorInterface> target;
  for (auto editor : editors(*window)) if (editor->documentId().toStdString() == *source->second.editorId) {
    if (target) return fail(tr("Source editor identity is ambiguous"));
    target = editor;
  }
  if (!target || key(*target) != span.fileKey || target->sourceRevision != *source->second.editorRevision ||
      EvaluationInputs::fingerprint(target->toPlainText().toUtf8().toStdString()) != span.fingerprint)
    return fail(tr("The captured source editor changed or closed"));
  EditableTarget result{target, current.hit.stamp,
    {target->documentId(), target->sourceRevision, target->sourceFingerprint,
      {{static_cast<qsizetype>(span.begin), static_cast<qsizetype>(span.end), QByteArray::fromStdString(*bytes), replacement}}}};
  if (!validateTarget(result, detail)) return {};
  return result;
}

bool ProvenanceService::validateTarget(const EditableTarget& target, QString *detail) const {
  if (!window || !target.editor || !window->evaluationBinding->validate(target.stamp, detail)) return false;
  QVector<SourceEditing::ByteEdit> changes;
  auto status = SourceEditing::validate(target.editor->toPlainText().toUtf8(), target.editor->documentId(),
    target.editor->sourceRevision, target.editor->sourceFingerprint, target.editor->isSourceReadOnly(), target.request, changes);
  if (detail) *detail = status.detail;
  return status.error == SourceEditing::EditError::None;
}

bool ProvenanceService::navigate(const Resolved& resolved, uint64_t id, NavigationSite role, QString *detail) {
  auto fail = [detail](const QString& reason) { if (detail) *detail = reason; return false; };
  if (!window) return fail(tr("Source window closed"));
  const auto current = resolve(resolved.hit);
  const auto candidates = navigationCandidates(current);
  if (!current.result || current.result != resolved.result ||
      std::find(candidates.begin(), candidates.end(), id) == candidates.end())
    return fail(tr("Source navigation requires a current recorded candidate"));
  const auto instance = current.result->instance(id);
  if (!instance || (role == NavigationSite::Definition && !instance->definition)) return fail(tr("Definition source is unavailable"));
  const auto& site = role == NavigationSite::Definition ? *instance->definition : instance->call;
  const auto span = site.head ? site.head : site.wholeComplete ? std::optional<Span>(site.whole) : std::nullopt;
  if (!span || !current.result->syntaxTable().original(*span) || span->end > static_cast<uint64_t>(std::numeric_limits<int>::max()))
    return fail(tr("Source range is unavailable"));
  const auto inputs = current.result->syntaxTable().inputs();
  auto source = inputs->consumed().find(span->fileKey);
  if (source == inputs->consumed().end()) return fail(tr("Source was not captured by this evaluation"));
  QPointer<MainWindow> guard = window;
  QPointer<EditorInterface> target;
  for (auto editor : window->tabManager->editorList)
    if (key(*editor) == span->fileKey && (!source->second.editorId || editor->documentId().toStdString() == *source->second.editorId)) {
      if (target) return fail(tr("Source document is ambiguous"));
      target = editor;
    }
  if (!target) {
    if (span->fileKey.starts_with("session:")) return fail(tr("Untitled source is no longer open"));
    window->tabManager->open(QString::fromStdString(span->fileKey));
    if (!guard) return fail(tr("Source window closed during navigation"));
    for (auto editor : guard->tabManager->editorList) if (key(*editor) == span->fileKey) {
      if (target) return fail(tr("Source document is ambiguous"));
      target = editor;
    }
  } else if (guard->activeEditor != target) {
    // Normal tab handling activates the exact existing document.
    if (target->filepath.isEmpty()) return fail(tr("The untitled source tab is not active"));
    guard->tabManager->open(target->filepath);
  }
  // Activating a library intentionally changes the displayed document. Recheck the retained
  // input identities rather than requiring the old root tab to remain active.
  if (!guard || !target || guard->activeEditor != target || key(*target) != span->fileKey ||
      !Selection::EvaluationBinding::inputsCurrent(*inputs, *guard, detail) ||
      EvaluationInputs::fingerprint(target->toPlainText().toUtf8().toStdString()) != span->fingerprint ||
      (source->second.editorId && (target->documentId().toStdString() != *source->second.editorId ||
                                target->sourceRevision != *source->second.editorRevision)))
    return fail(tr("Source changed or the target tab changed during navigation"));
  auto editor = qobject_cast<ScintillaEditor *>(target.data());
  if (!editor) return fail(tr("This editor cannot select exact UTF-8 byte ranges"));
  editor->qsci->SendScintilla(QsciScintillaBase::SCI_SETSEL, static_cast<unsigned long>(span->begin), static_cast<long>(span->end));
  editor->qsci->SendScintilla(QsciScintillaBase::SCI_SCROLLCARET);
  editor->setFocus();
  return true;
}

QWidget *ProvenanceService::build(const Selection::State& state, const Selection::Stamp&) {
  auto body = new QWidget;
  body->setObjectName("sourceProvenance");
  auto layout = new QVBoxLayout(body); layout->setContentsMargins(0, 0, 0, 0);
  auto label = new QLabel(body); label->setObjectName("sourceProvenanceStatus"); label->setWordWrap(true);
  layout->addWidget(label);
  if (!state.surface) { label->setText(tr("Source: select a surface to inspect its evaluated invocation.")); return body; }
  const auto resolved = resolve(*state.surface);
  const QString status = resolved.mapping.confidence == Confidence::Unique ? tr("one producing invocation")
    : resolved.mapping.confidence == Confidence::Multiple ? tr("multiple candidates")
    : resolved.mapping.confidence == Confidence::Stale ? tr("stale") : tr("unavailable");
  label->setText(tr("Source: %1\n%2").arg(status, QString::fromStdString(resolved.mapping.reason)));
  if (!resolved.result || resolved.mapping.candidateInstances.empty()) return body;
  auto candidates = new QComboBox(body); candidates->setObjectName("provenanceCandidates");
  candidates->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
  candidates->setMinimumContentsLength(20);
  candidates->addItem(tr("Choose an invocation"), QVariant());
  for (const auto id : navigationCandidates(resolved)) if (auto instance = resolved.result->instance(id))
    candidates->addItem(QString::fromStdString(instance->call.callable) + QString(" · invocation %1 · ").arg(id) + fileLabel(instance->call), QVariant::fromValue<qulonglong>(id));
  layout->addWidget(candidates);
  auto expressions = new QLabel(body); expressions->setObjectName("provenanceExpressions"); expressions->setWordWrap(true);
  expressions->setTextInteractionFlags(Qt::TextSelectableByMouse); layout->addWidget(expressions);
  auto call = new QPushButton(tr("Go to call"), body); call->setObjectName("provenanceGoToCall"); layout->addWidget(call);
  auto definition = new QPushButton(tr("Go to definition"), body); definition->setObjectName("provenanceGoToDefinition"); layout->addWidget(definition);
  connect(candidates, &QComboBox::currentIndexChanged, body, [=](int) {
    const auto instance = resolved.result->instance(candidates->currentData().toULongLong());
    call->setEnabled(instance); definition->setEnabled(instance && instance->definition);
    QString text;
    if (instance) {
      text = tr("Call: %1\nPath depth: %2").arg(fileLabel(instance->call)).arg(instance->path.size());
      if (instance->definition) text += tr("\nDefinition: %1").arg(fileLabel(*instance->definition));
      for (const auto& argument : instance->call.arguments)
        text += QString("\n%1: %2").arg(argument.name ? QString::fromStdString(*argument.name) : QString::number(argument.position+1),
          argument.hasExpression ? QString::fromStdString(argument.originalExpression) : tr("no expression"));
    }
    expressions->setText(text);
  });
  call->setEnabled(false); definition->setEnabled(false);
  if (resolved.mapping.confidence == Confidence::Unique) candidates->setCurrentIndex(1);
  auto queue = [this, resolved, candidates](NavigationSite role) {
    const auto id = candidates->currentData().toULongLong();
    QTimer::singleShot(0, this, [guard = QPointer<ProvenanceService>(this), resolved, id, role] {
      if (!guard) return;
      QString detail;
      const bool success = guard->navigate(resolved, id, role, &detail);
      if (!success && guard && guard->window) guard->window->statusBar()->showMessage(detail, 8000);
    });
  };
  connect(call, &QPushButton::clicked, this, [queue] { queue(NavigationSite::Call); });
  connect(definition, &QPushButton::clicked, this, [queue] { queue(NavigationSite::Definition); });
  return body;
}
}
