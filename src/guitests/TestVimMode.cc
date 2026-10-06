#include "TestVimMode.h"
#include "gui/ScintillaEditor.h"
#include "gui/Preferences.h"
#include "gui/vim/VimEditorController.h"
#include "gui/vim/VimPrompt.h"
#include "gui/source/SourceEditTransaction.h"
#include <QApplication>
#include <QAction>
#include <QBuffer>
#include <QCheckBox>
#include <QClipboard>
#include <QDateTime>
#include <QDialog>
#include <QFileDialog>
#include <QFile>
#include <QDir>
#include <QImage>
#include <QInputMethodEvent>
#include <QJsonArray>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QUuid>
using Vim::Mode;
using S = QsciScintillaBase;
namespace {
void keys(ScintillaEditor *editor, const char *text) { QTest::keyClicks(editor->qsci, QString::fromLatin1(text)); }
void escape(ScintillaEditor *editor) { QTest::keyClick(editor->qsci, Qt::Key_Escape); }
void command(ScintillaEditor *editor, const QString& text) {
  keys(editor, ":"); auto prompt = editor->vimController->promptWidget();
  prompt->setText(text); QPointer<Vim::Prompt> guard(prompt);
  QKeyEvent press(QEvent::KeyPress, Qt::Key_Return, {});
  QApplication::sendEvent(prompt, &press);
  if (guard) { QKeyEvent release(QEvent::KeyRelease, Qt::Key_Return, {}); QApplication::sendEvent(guard, &release); }
  QApplication::processEvents();
}
void source(ScintillaEditor *editor, const QString& text) {
  editor->setPlainText(text); editor->qsci->SendScintilla(S::SCI_EMPTYUNDOBUFFER);
  editor->qsci->setCursorPosition(0, 0); editor->setFocus();
}
bool write(const QString& path, const QByteArray& bytes) {
  QFile file(path); return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size();
}
QByteArray read(const QString& path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray(); }
QByteArray readSourceText(const QString& path) { QFile file(path); return file.open(QIODevice::ReadOnly | QIODevice::Text) ? file.readAll() : QByteArray(); }
struct DialogChoice {
  QTimer poll, deadline;
  bool seen = false, timedOut = false;
  explicit DialogChoice(std::function<bool(QWidget *)> choose) {
    poll.setInterval(20); deadline.setSingleShot(true); deadline.setInterval(4000);
    QObject::connect(&poll, &QTimer::timeout, [this, choose] {
      if (auto dialog = QApplication::activeModalWidget(); dialog && choose(dialog)) {
        seen = true; poll.stop(); deadline.stop();
      }
    });
    QObject::connect(&deadline, &QTimer::timeout, [this] {
      timedOut = true; poll.stop();
      if (auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) dialog->reject();
    });
    poll.start(); deadline.start();
  }
};
bool cancelDialog(QWidget *widget) {
  if (auto box = qobject_cast<QMessageBox *>(widget)) {
    if (auto button = box->button(QMessageBox::Cancel)) { button->click(); return true; }
  }
  if (auto dialog = qobject_cast<QFileDialog *>(widget)) { dialog->reject(); return true; }
  return false;
}
AgentAnnotations::Record annotation() {
  QImage image(8, 8, QImage::Format_RGB32); image.fill(Qt::blue);
  QByteArray png; QBuffer buffer(&png); buffer.open(QIODevice::WriteOnly); image.save(&buffer, "PNG");
  return {{{"id", QUuid::createUuid().toString(QUuid::WithoutBraces)}, {"comment", "Vim undo composition"},
    {"created_utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
    {"width", 8}, {"height", 8}, {"png_sha256", QString::fromLatin1(AgentAnnotations::Store::hash(png))},
    {"camera", QJsonObject{{"translation", QJsonArray{0,0,0}}, {"rotation", QJsonArray{0,0,0}},
      {"distance",100}, {"fov",45}, {"projection","perspective"}}},
    {"strokes", QJsonArray{}}, {"result", QJsonObject{}}}, png};
}
}
void TestVimMode::initTestCase()
{
  QVERIFY(!Settings::Settings::vimMode.value());
  window->hide(); window->show(); window->raise(); window->activateWindow();
  QVERIFY(QTest::qWaitForWindowExposed(window));
  QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
}
void TestVimMode::init()
{
  window->designActionAutoReload->setChecked(false);
  Settings::Settings::vimMode.setValue(true); GlobalPreferences::inst()->fireEditorConfigChanged();
  GlobalPreferences::inst()->updateGUI();
  window->tabManager->actionNew(); editor = dynamic_cast<ScintillaEditor *>(window->activeEditor);
  QVERIFY(editor); source(editor, "one two three");
  QTRY_VERIFY(editor->qsci->hasFocus());
  QVERIFY(editor->vimController->engine().enabled());
}
void TestVimMode::cleanup()
{
  auto checkbox = GlobalPreferences::inst()->findChild<QCheckBox *>("vimModeCheckBox");
  if (checkbox) checkbox->setChecked(false);
  Settings::Settings::vimMode.setValue(false); GlobalPreferences::inst()->fireEditorConfigChanged();
  for (auto document : window->tabManager->editorList) document->setContentModified(false);
  DialogChoice discard([](QWidget *widget) {
    auto box = qobject_cast<QMessageBox *>(widget);
    if (!box || !box->button(QMessageBox::Discard)) return false;
    box->button(QMessageBox::Discard)->click(); return true;
  });
  restoreWindowInitialState();
}
void TestVimMode::preferencePreservesTextHistoryAndUpdatesWindows()
{
  auto checkbox = GlobalPreferences::inst()->findChild<QCheckBox *>("vimModeCheckBox"); QVERIFY(checkbox);
  checkbox->setChecked(false); GlobalPreferences::inst()->fireEditorConfigChanged();
  QVERIFY(!editor->vimController->engine().enabled());
  QTest::keyClicks(editor->qsci, "Z"); const auto dirty = editor->toPlainText(); QVERIFY(editor->canUndo());
  const auto revision = editor->sourceRevision;
  checkbox->setChecked(true); QSettings settings; settings.sync();
  QVERIFY(settings.value("editor/vimMode").toBool());
  const auto output = qEnvironmentVariable("OPENSCAD_GUI_TEST_OUTPUT");
  if (!output.isEmpty()) QVERIFY(QFile::copy(settings.fileName(), QDir(output).filePath("enabled-settings.ini")));
  QCOMPARE(editor->toPlainText(), dirty); QCOMPARE(editor->sourceRevision, revision); QVERIFY(editor->isContentModified());
  auto otherWindow = new MainWindow(QStringList{}); QPointer<MainWindow> other(otherWindow);
  otherWindow->designActionAutoReload->setChecked(false);
  auto otherEditor = dynamic_cast<ScintillaEditor *>(otherWindow->activeEditor); QVERIFY(otherEditor);
  QVERIFY(otherEditor->vimController->engine().enabled());
  checkbox->setChecked(false);
  QVERIFY(!editor->vimController->engine().enabled()); QVERIFY(!otherEditor->vimController->engine().enabled());
  QCOMPARE(editor->toPlainText(), dirty); editor->undo(); QCOMPARE(editor->toPlainText(), QString("one two three"));
  otherWindow->close(); QTRY_VERIFY(other.isNull());
  checkbox->setChecked(true); editor->setFocus();
}
void TestVimMode::shortcutPriorityAndOtherFields()
{
  QAction competing(window); competing.setShortcut(QKeySequence("Ctrl+R")); window->addAction(&competing);
  int fired = 0; connect(&competing, &QAction::triggered, [&] { ++fired; });
  QAction printable(window); printable.setShortcut(QKeySequence("X")); window->addAction(&printable);
  connect(&printable, &QAction::triggered, [&] { ++fired; });
  keys(editor, "dw"); keys(editor, "u"); QTest::keyClick(editor->qsci, Qt::Key_R, Qt::ControlModifier);
  QCOMPARE(editor->toPlainText(), QString("two three")); QCOMPARE(fired, 0);
  keys(editor, "X"); QCOMPARE(fired, 0); QVERIFY(editor->vimController->displayText().contains("Unsupported"));
  QTest::keyClick(editor->qsci, Qt::Key_Right, Qt::KeypadModifier);
  QCOMPARE(editor->qsci->SendScintilla(S::SCI_GETCURRENTPOS), 1L);
  QDialog dialog(window); QLineEdit field(&dialog); dialog.setWindowModality(Qt::ApplicationModal); dialog.show(); field.setFocus();
  QTRY_VERIFY(field.hasFocus()); QTest::keyClicks(&field, "ixw"); QCOMPARE(field.text(), QString("ixw"));
  QTest::keyClick(&field, Qt::Key_A, Qt::ControlModifier); QCOMPARE(field.selectedText(), QString("ixw"));
  dialog.close(); window->activateWindow(); window->raise(); editor->setFocus();
  QTRY_VERIFY(!QApplication::activeModalWidget());
  QTRY_COMPARE(QApplication::activeWindow(), window);
  QTRY_VERIFY(window->isActiveWindow());
  QTRY_VERIFY(editor->qsci->hasFocus());
  QAction compile(window); compile.setShortcut(QKeySequence(Qt::Key_F24)); window->addAction(&compile);
  int compiled = 0; connect(&compile, &QAction::triggered, [&] { ++compiled; });
  QTest::keyClick(editor->qsci, Qt::Key_F24); QCOMPARE(compiled, 1);
  source(editor, "cube(2);\n"); QSignalSpy preview(window->designActionPreview, &QAction::triggered);
  QTest::keyClick(editor->qsci, Qt::Key_F5); QTRY_COMPARE(preview.size(), 1);
  QTRY_VERIFY(!GuiLocker::isLocked()); QCOMPARE(editor->toPlainText(), QString("cube(2);\n"));
  QAction *applicationCtrlR = nullptr;
  for (auto action : window->findChildren<QAction *>()) {
    if (action != &competing && action->shortcuts().contains(QKeySequence("Ctrl+R"))) {
      QVERIFY(!applicationCtrlR); applicationCtrlR = action;
    }
  }
  QVERIFY(applicationCtrlR);
  window->removeAction(&competing); // Restore the actual application binding before disabling Vim.
  QSignalSpy restoredShortcut(applicationCtrlR, &QAction::triggered);
  Settings::Settings::vimMode.setValue(false); editor->applySettings();
  QTest::keyClick(editor->qsci, Qt::Key_R, Qt::ControlModifier); QTRY_COMPARE(restoredShortcut.size(), 1);
  QTRY_VERIFY(!GuiLocker::isLocked()); QCOMPARE(fired, 0);
  Settings::Settings::vimMode.setValue(true); editor->applySettings();
  window->removeAction(&compile); window->removeAction(&competing); window->removeAction(&printable);
}
void TestVimMode::completionAndCalltipOneEscape()
{
  source(editor, ""); keys(editor, "icu");
  editor->qsci->SendScintilla(S::SCI_AUTOCSHOW, 2, "cube cylinder");
  QVERIFY(editor->qsci->SendScintilla(S::SCI_AUTOCACTIVE)); escape(editor);
  QVERIFY(!editor->qsci->SendScintilla(S::SCI_AUTOCACTIVE)); QCOMPARE(editor->toPlainText(), QString("cu"));
  QCOMPARE(editor->vimController->engine().mode(), Mode::Normal);
  keys(editor, "u"); QCOMPARE(editor->toPlainText(), QString());
  keys(editor, "iX"); editor->qsci->SendScintilla(S::SCI_CALLTIPSHOW, uintptr_t{0}, "cube(size)");
  QVERIFY(editor->qsci->SendScintilla(S::SCI_CALLTIPACTIVE)); escape(editor);
  QVERIFY(!editor->qsci->SendScintilla(S::SCI_CALLTIPACTIVE)); QCOMPARE(editor->vimController->engine().mode(), Mode::Normal);
  keys(editor, "u"); QCOMPARE(editor->toPlainText(), QString());
}
void TestVimMode::nativeCompletionImeAndRepeat()
{
  source(editor, ""); keys(editor, "icu");
  editor->qsci->SendScintilla(S::SCI_AUTOCSHOW, 2, "cube cylinder");
  QTest::keyClick(editor->qsci, Qt::Key_Return); escape(editor);
  QCOMPARE(editor->toPlainText(), QString("cube")); keys(editor, "A");
  QInputMethodEvent ime; ime.setCommitString(QString::fromUtf8("é😀")); QApplication::sendEvent(editor->qsci, &ime);
  escape(editor); QCOMPARE(editor->toPlainText(), QString::fromUtf8("cubeé😀"));
  keys(editor, "."); QCOMPARE(editor->toPlainText(), QString::fromUtf8("cubeé😀é😀"));
  keys(editor, "u"); QCOMPARE(editor->toPlainText(), QString::fromUtf8("cubeé😀"));
  keys(editor, "u"); QCOMPARE(editor->toPlainText(), QString("cube"));
  keys(editor, "u"); QCOMPARE(editor->toPlainText(), QString());
  keys(editor, "a"); QApplication::clipboard()->setText("pasted");
  QTest::keyClick(editor->qsci, Qt::Key_V, Qt::ControlModifier); escape(editor);
  QCOMPARE(editor->toPlainText(), QString("pasted"));
  keys(editor, "."); QCOMPARE(editor->toPlainText(), QString("pastedpasted"));
  keys(editor, "u"); keys(editor, "u"); QCOMPARE(editor->toPlainText(), QString());
  keys(editor, "i"); QKeyEvent altgr(QEvent::KeyPress, Qt::Key_E, Qt::ControlModifier | Qt::AltModifier, "€");
  QApplication::sendEvent(editor->qsci, &altgr); escape(editor); QCOMPARE(editor->toPlainText(), QString::fromUtf8("€"));
}
void TestVimMode::pendingInputAndPromptStayWithTheirDocument()
{
  QPointer<ScintillaEditor> origin(editor); keys(editor, "2d");
  window->tabManager->actionNew(); auto other = dynamic_cast<ScintillaEditor *>(window->activeEditor); QVERIFY(other);
  source(other, "red blue"); keys(other, "x"); QCOMPARE(other->toPlainText(), QString("ed blue"));
  auto tabs = qobject_cast<QTabWidget *>(window->tabManager->getTabContent()); tabs->setCurrentWidget(origin);
  origin->setFocus(); keys(origin, "x"); QCOMPARE(origin->toPlainText(), QString("ne two three"));
  keys(origin, "/"); QVERIFY(origin->vimController->promptWidget()->isVisible());
  tabs->setCurrentWidget(other); QVERIFY(!origin->vimController->promptWidget()->isVisible());
  tabs->setCurrentWidget(origin); origin->setFocus(); keys(origin, ":");
  origin->setContentModified(false); QCOMPARE(window->tabManager->closeDocument(origin), Vim::HostResult::Success);
  QVERIFY(origin.isNull()); QCOMPARE(other->toPlainText(), QString("ed blue"));
  other->setFocus(); keys(other, "x"); QCOMPARE(other->toPlainText(), QString("d blue"));
}
void TestVimMode::annotationSourceTransactionAndRepeatHistory()
{
  source(editor, "one two three");
  Settings::Settings::vimMode.setValue(false); editor->applySettings(); keys(editor, "Z");
  const auto typed = editor->toPlainText(); QVERIFY(editor->annotations->add(annotation()));
  Settings::Settings::vimMode.setValue(true); editor->applySettings(); editor->qsci->setCursorPosition(0, 0);
  keys(editor, "cwX"); escape(editor); const auto changed = editor->toPlainText(); QCOMPARE(changed, QString("X two three"));
  SourceEditing::EditRequest transaction{editor->documentId(), editor->sourceRevision, editor->sourceFingerprint, {{2, 5, "two", "EXTERNAL"}}};
  const auto revision = editor->sourceRevision;
  QCOMPARE(SourceEditing::apply(*editor, transaction).error, SourceEditing::EditError::None);
  QCOMPARE(editor->sourceRevision, revision + 1); const auto external = editor->toPlainText();
  keys(editor, "w."); QCOMPARE(editor->toPlainText(), QString("X X three"));
  keys(editor, "u"); QCOMPARE(editor->toPlainText(), external);
  keys(editor, "u"); QCOMPARE(editor->toPlainText(), changed);
  keys(editor, "u"); QCOMPARE(editor->toPlainText(), typed); QCOMPARE(editor->annotations->records().size(), 1);
  keys(editor, "u"); QVERIFY(editor->annotations->records().isEmpty()); QCOMPARE(editor->toPlainText(), typed);
  keys(editor, "u"); QCOMPARE(editor->toPlainText(), QString("one two three")); QVERIFY(!editor->canUndo());
  for (int i = 0; i < 5; ++i) QTest::keyClick(editor->qsci, Qt::Key_R, Qt::ControlModifier);
  QCOMPARE(editor->toPlainText(), QString("X X three")); QCOMPARE(editor->annotations->records().size(), 1);
  for (int i = 0; i < 5; ++i) keys(editor, "u");
}
void TestVimMode::externalActionClosesInsertionBeforeUndoGroup()
{
  source(editor, "one two"); keys(editor, "cwX"); escape(editor); keys(editor, "wiY");
  const auto insertion = editor->toPlainText(); bool notified = false;
  const auto connection = connect(editor, &EditorInterface::externalEditStarting, editor, [&] {
    notified = !editor->vimController->engine().undoGroupOpen();
  });
  QAction action(window); connect(&action, &QAction::triggered, editor, [&] { editor->insertOrReplaceText("EXT"); });
  action.trigger(); QVERIFY(notified); QCOMPARE(editor->vimController->engine().mode(), Mode::Normal);
  const auto external = editor->toPlainText(); QVERIFY(external.contains("EXT"));
  keys(editor, "u"); QCOMPARE(editor->toPlainText(), insertion);
  keys(editor, "u"); QCOMPARE(editor->toPlainText(), QString("X two"));
  editor->qsci->setCursorPosition(0, 2); keys(editor, "."); QCOMPARE(editor->toPlainText(), QString("X X"));
  QVERIFY(!editor->toPlainText().contains("EXT"));
  disconnect(connection);
}
void TestVimMode::literalSearchAndCancellation()
{
  source(editor, "one TWO two one"); keys(editor, "/");
  auto prompt = editor->vimController->promptWidget(); QTest::keyClicks(prompt, "two"); QTest::keyClick(prompt, Qt::Key_Return);
  QCOMPARE(editor->qsci->SendScintilla(S::SCI_GETCURRENTPOS), 8L); QVERIFY(!editor->canUndo());
  keys(editor, "n"); QCOMPARE(editor->qsci->SendScintilla(S::SCI_GETCURRENTPOS), 8L);
  QVERIFY(editor->vimController->displayText().contains("wrapped"));
  keys(editor, "?"); QTest::keyClicks(prompt, "one"); QTest::keyClick(prompt, Qt::Key_Escape);
  QCOMPARE(editor->qsci->SendScintilla(S::SCI_GETCURRENTPOS), 8L); QTRY_VERIFY(editor->qsci->hasFocus());
  keys(editor, "/"); QTest::keyClicks(prompt, "missing"); QTest::keyClick(prompt, Qt::Key_Return);
  QVERIFY(editor->vimController->displayText().contains("not found"));
}
void TestVimMode::protectedSaveCloseReloadAndUnicodeOpen()
{
  QTemporaryDir dir; const auto path = dir.filePath(QString::fromUtf8("日本語 file with spaces.scad"));
  keys(editor, "iA"); escape(editor); // Existing open machinery reuses a clean untitled tab.
  QVERIFY(write(path, "cube(3);\n")); command(editor, "e \"" + path + "\"");
  auto opened = dynamic_cast<ScintillaEditor *>(window->activeEditor); QVERIFY(opened); QCOMPARE(opened->filepath, path);
  QVERIFY(opened != editor);
  QCOMPARE(opened->toPlainText(), QString("cube(3);\n")); keys(opened, "iZ"); escape(opened);
  const auto dirty = opened->toPlainText();
  { DialogChoice choice(cancelDialog); command(opened, "e"); QVERIFY(choice.seen && !choice.timedOut); }
  QCOMPARE(opened->toPlainText(), dirty); QVERIFY(opened->isContentModified());
  { DialogChoice choice(cancelDialog); command(opened, "q"); QVERIFY(choice.seen && !choice.timedOut); }
  QVERIFY(window->tabManager->editorList.contains(opened)); QCOMPARE(opened->toPlainText(), dirty);
  command(opened, "w"); QCOMPARE(readSourceText(path), dirty.toUtf8()); QVERIFY(!opened->isContentModified());
  QVERIFY(write(path, "sphere(4);\n")); command(opened, "e"); QCOMPARE(opened->toPlainText(), QString("sphere(4);\n"));
  auto tabs = qobject_cast<QTabWidget *>(window->tabManager->getTabContent()); tabs->setCurrentWidget(editor); editor->setFocus();
  keys(editor, "iQ"); escape(editor);
  QPointer<ScintillaEditor> captured(editor); auto other = opened;
  { DialogChoice choice([&](QWidget *widget) {
      auto box = qobject_cast<QMessageBox *>(widget); if (!box || !box->button(QMessageBox::Discard)) return false;
      tabs->setCurrentWidget(other); box->button(QMessageBox::Discard)->click(); return true;
    }); command(editor, "q"); QVERIFY(choice.seen && !choice.timedOut); }
  QVERIFY(captured.isNull()); QVERIFY(window->tabManager->editorList.contains(other)); QCOMPARE(other->toPlainText(), QString("sphere(4);\n"));
}
void TestVimMode::saveAsConflictAndFailedSaveFeedback()
{
  command(editor, "e"); QVERIFY(editor->vimController->displayText().contains("no filename"));
  const auto before = editor->toPlainText(); command(editor, "e Z:/missing-document.scad");
  QVERIFY(editor->vimController->displayText().contains("Open failed")); QCOMPARE(editor->toPlainText(), before);
  { DialogChoice cancel(cancelDialog); command(editor, "w"); QVERIFY(cancel.seen && !cancel.timedOut); }
  QVERIFY(editor->filepath.isEmpty()); QVERIFY(editor->vimController->displayText().contains("cancelled or failed"));
  QTemporaryDir dir; const auto path = dir.filePath("saved.scad");
  { DialogChoice choose([&](QWidget *widget) {
      auto dialog = qobject_cast<QFileDialog *>(widget); if (!dialog) return false;
      dialog->selectFile(path); QMetaObject::invokeMethod(dialog, "accept"); return true;
    }); command(editor, "w"); QVERIFY(choose.seen && !choose.timedOut); }
  QCOMPARE(editor->filepath, path); QCOMPARE(read(path), editor->toPlainText().toUtf8());
  keys(editor, "iX"); escape(editor); const auto dirty = editor->toPlainText(); QVERIFY(write(path, "EXTERNAL"));
  { DialogChoice cancel(cancelDialog); command(editor, "w"); QVERIFY(cancel.seen && !cancel.timedOut); }
  QCOMPARE(read(path), QByteArray("EXTERNAL")); QCOMPARE(editor->toPlainText(), dirty); QVERIFY(editor->isContentModified());
  QVERIFY(editor->vimController->displayText().contains("cancelled or failed"));
  const auto failed = dir.filePath("missing-directory/fail.scad");
  editor->filepath = failed; editor->sourceDiskBaseline.reset(); editor->sourceDiskObserved.reset();
  { DialogChoice dismiss([](QWidget *widget) {
      auto box = qobject_cast<QMessageBox *>(widget); if (!box) return false;
      if (auto button = box->button(QMessageBox::Ok)) { button->click(); return true; } return false;
    }); command(editor, "w"); }
  QVERIFY(!QFile::exists(failed)); QVERIFY(editor->isContentModified()); QVERIFY(editor->vimController->displayText().contains("cancelled or failed"));
  window->tabManager->actionNew();
  auto closing = dynamic_cast<ScintillaEditor *>(window->activeEditor); QVERIFY(closing);
  QPointer<ScintillaEditor> origin(closing);
  { DialogChoice closeDuringSave([&](QWidget *widget) {
      auto dialog = qobject_cast<QFileDialog *>(widget); if (!dialog) return false;
      closing->setContentModified(false); window->tabManager->closeDocument(closing); dialog->reject(); return true;
    }); command(closing, "w"); QVERIFY(closeDuringSave.seen && !closeDuringSave.timedOut); }
  QVERIFY(origin.isNull()); QVERIFY(window->tabManager->editorList.contains(editor));
}
void TestVimMode::unsupportedAndReadOnly()
{
  for (auto key : {Qt::Key_Return, Qt::Key_Backspace, Qt::Key_Delete, Qt::Key_Tab})
    QTest::keyClick(editor->qsci, key);
  QCOMPARE(editor->toPlainText(), QString("one two three")); QVERIFY(!editor->canUndo());
  for (const char *unsupported : {"q", "\"a", "rZ", "R", "mA"}) {
    keys(editor, unsupported); escape(editor); QCOMPARE(editor->toPlainText(), QString("one two three")); QVERIFY(!editor->canUndo());
  }
  command(editor, "!echo should-never-run"); QVERIFY(editor->vimController->displayText().contains("Unsupported"));
  command(editor, "w!"); QVERIFY(editor->vimController->displayText().contains("Unsupported"));
  command(editor, "set keymap=anything"); QVERIFY(editor->vimController->displayText().contains("Unsupported"));
  editor->qsci->setCursorPosition(0, 0);
  editor->qsci->setReadOnly(true); keys(editor, "w"); QCOMPARE(editor->qsci->SendScintilla(S::SCI_GETCURRENTPOS), 4L);
  keys(editor, "dwicwXp"); escape(editor); QCOMPARE(editor->toPlainText(), QString("one two three")); QVERIFY(!editor->canUndo());
  keys(editor, "/"); auto prompt = editor->vimController->promptWidget(); prompt->setText("three"); QTest::keyClick(prompt, Qt::Key_Return);
  QCOMPARE(editor->qsci->SendScintilla(S::SCI_GETCURRENTPOS), 8L); editor->qsci->setReadOnly(false);
  const auto output = qEnvironmentVariable("OPENSCAD_GUI_TEST_OUTPUT");
  if (!output.isEmpty()) QVERIFY(editor->grab().save(QDir(output).filePath("vim-editor.png")));
}
void TestVimPersistence::persistedPreferenceIsAppliedAtStartup()
{
  const bool expected = qEnvironmentVariable("OPENSCAD_VIM_EXPECT_ENABLED") == "1";
  QCOMPARE(Settings::Settings::vimMode.value(), expected);
  auto editor = dynamic_cast<ScintillaEditor *>(window->activeEditor); QVERIFY(editor);
  QCOMPARE(editor->vimController->engine().enabled(), expected);
  const auto text = editor->toPlainText();
  QTest::keyClick(editor->qsci, Qt::Key_I);
  if (expected) { QCOMPARE(editor->toPlainText(), text); QCOMPARE(editor->vimController->engine().mode(), Mode::Insert); escape(editor); }
  else QVERIFY(editor->toPlainText() != text);
}
