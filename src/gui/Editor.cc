#include "gui/Editor.h"
#include "gui/source/EditorUndoRegistry.h"

#include <QWidget>

#include "gui/Preferences.h"
#include "gui/QSettingsCached.h"
#include "platform/PlatformUtils.h"
#include "utils/printutils.h"
#include <QDir>
#include <QPointer>
#include <QSaveFile>
#include <QTemporaryFile>
#include <QTimer>

bool EditorInterface::prepareExternalEdit()
{
  QPointer<EditorInterface> guard(this);
  emit externalEditStarting();
  return !guard.isNull();
}

EditorInterface::EditorInterface(QWidget *parent) : QWidget(parent), annotations(new AgentAnnotations::Store(this))
{
  auto registry=SourceEditing::EditorUndoRegistry::forEditor(*this);
  connect(annotations,&AgentAnnotations::Store::historyCleared,registry,&SourceEditing::EditorUndoRegistry::clear);
  connect(annotations, &AgentAnnotations::Store::commandAdded, this, [this](int token) {
    QPointer<EditorInterface> guard(this);
    appendAnnotationUndo(token);
    if (guard) emit modificationChanged(this);
  });
  connect(annotations, &AgentAnnotations::Store::changed, this, [this] { emit modificationChanged(this); });
  auto timer = new QTimer(this);
  timer->setSingleShot(true);
  timer->setInterval(1500);
  auto backup = new QString;
  connect(this, &QObject::destroyed, [backup] { delete backup; });
  connect(annotations, &AgentAnnotations::Store::changed, timer, [timer, this] {
    if (annotations->dirty()) timer->start();
  });
  connect(timer, &QTimer::timeout, this, [this, backup] {
    if (!annotations->dirty()) return;
    QString directory = QString::fromStdString(PlatformUtils::backupPath());
#ifdef ENABLE_GUI_TESTS
    const auto testBackup = qEnvironmentVariable("OPENSCAD_ANNOTATION_TEST_BACKUP");
    if (!testBackup.isEmpty()) directory = testBackup;
#endif
    if (!QDir().mkpath(directory)) return;
    if (backup->isEmpty()) {
      QTemporaryFile file(directory + "/annotations-backup-XXXXXX.scad");
      if (!file.open()) return;
      file.setAutoRemove(false);
      *backup = file.fileName();
    }
    QSaveFile source(*backup);
    const auto text = toPlainText().toUtf8();
    QString error;
    if (!source.open(QIODevice::WriteOnly) || source.write(text) != text.size() || !source.commit() ||
        !annotations->save(*backup + ".annotations", false, &error, true))
      LOG(message_group::UI_Warning, "Annotation backup failed: %1$s", error.toStdString());
  });
}
