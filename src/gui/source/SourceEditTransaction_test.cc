#include "gui/source/SourceEditTransaction.h"
#include "gui/source/EditorUndoRegistry.h"
#include "gui/ScintillaEditor.h"
#include "platform/PlatformUtils.h"
#include <QApplication>
#include <QSettings>
#include <QTemporaryDir>
#include <catch2/catch_all.hpp>
#include <memory>

using namespace SourceEditing;
namespace {
void ensureApplication()
{
  if (QApplication::instance()) return;
  qputenv("QT_QPA_PLATFORM", "offscreen");
  static int argc = 1;
  static char name[] = "source-editing-tests";
  static char *argv[] = {name, nullptr};
  static QTemporaryDir settings;
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
  static auto application = std::make_unique<QApplication>(argc, argv);
  QApplication::setOrganizationName("OpenSCADSourceTests");
  QApplication::setApplicationName("source-editing");
  PlatformUtils::registerApplicationPath(std::filesystem::path(OPENSCAD_TEST_DATA_DIR).parent_path().parent_path().string());
}
EditRequest requestFor(ScintillaEditor& editor, QVector<ByteEdit> edits)
{
  return {editor.documentId(), editor.sourceRevision, editor.sourceFingerprint, edits};
}
}

TEST_CASE("Source transaction context is rechecked after synchronous external edit observers", "[source-editing]")
{
  ensureApplication();ScintillaEditor editor(nullptr);editor.setPlainText("width=30;");editor.clearUndoHistory();
  bool current=true;auto request=requestFor(editor,{{6,8,"30","40"}});request.contextCurrent=[&]{return current;};
  auto connection=QObject::connect(&editor,&EditorInterface::externalEditStarting,&editor,[&]{current=false;});
  const auto result=apply(editor,request);REQUIRE(result.error==EditError::StaleContext);
  REQUIRE(editor.toPlainText()=="width=30;");REQUIRE_FALSE(editor.canUndo());
  QObject::disconnect(connection);
}

TEST_CASE("Read-only editors allow session undo participants but reject source edits", "[source-editing]")
{
  ensureApplication();
  ScintillaEditor editor(nullptr);
  editor.setPlainText("cube(10);");
  editor.clearUndoHistory();
  editor.qsci->setReadOnly(true);
  const auto revision = editor.sourceRevision;
  const auto fingerprint = editor.sourceFingerprint;
  int position = 0;
  auto participant = std::make_shared<UndoParticipant>();
  participant->lifetime = &editor;
  participant->undo = [&] { position = 0; };
  participant->redo = [&] { position = 3; };
  auto request = requestFor(editor, {});
  request.participant = participant;

  const auto result = apply(editor, request);
  REQUIRE(result.error == EditError::None);
  REQUIRE(result.changed);
  REQUIRE(position == 3);
  REQUIRE(editor.qsci->isReadOnly());
  // Scintilla reports undo unavailable while read-only; the session undo seam
  // temporarily unlocks it, so verify the actual operation below.
  editor.undoFromAnnotations(false);
  REQUIRE(position == 0);
  REQUIRE(editor.qsci->isReadOnly());
  editor.undoFromAnnotations(true);
  REQUIRE(position == 3);
  REQUIRE(editor.qsci->isReadOnly());

  auto textEdit = requestFor(editor, {{5, 7, "10", "20"}});
  REQUIRE(apply(editor, textEdit).error == EditError::ReadOnly);
  textEdit.participant = participant;
  REQUIRE(apply(editor, textEdit).error == EditError::ReadOnly);
  REQUIRE(editor.toPlainText() == "cube(10);");
  REQUIRE(editor.sourceRevision == revision);
  REQUIRE(editor.sourceFingerprint == fingerprint);
  REQUIRE(editor.qsci->isReadOnly());
}

TEST_CASE("Source transactions reject invalid original snapshots atomically", "[source-editing]")
{
  const auto source = QString::fromUtf8("// \xc3\xa9 \xf0\x9f\x98\x80\na=10; b=20;\n").toUtf8();
  EditRequest request{"document", 7, "hash", {{source.indexOf("10"), source.indexOf("10") + 2, "10", "11"}}};
  QVector<ByteEdit> changes;
  auto validateRequest = [&](bool readOnly = false) { return validate(source, "document", 7, "hash", readOnly, request, changes); };
  SECTION("wrong document") { request.documentId = "other"; REQUIRE(validateRequest().error == EditError::WrongDocument); }
  SECTION("stale revision") { --request.sourceRevision; REQUIRE(validateRequest().error == EditError::StaleSource); }
  SECTION("stale fingerprint") { request.sourceFingerprint = "old"; REQUIRE(validateRequest().error == EditError::StaleSource); }
  SECTION("read-only") { REQUIRE(validateRequest(true).error == EditError::ReadOnly); }
  SECTION("negative range") { request.edits[0].begin = -1; REQUIRE(validateRequest().error == EditError::InvalidRange); }
  SECTION("past end") { request.edits[0].end = source.size() + 1; REQUIRE(validateRequest().error == EditError::InvalidRange); }
  SECTION("UTF-8 boundary") { request.edits = {{4, 5, {}, "x"}}; REQUIRE(validateRequest().error == EditError::InvalidUtf8); }
  SECTION("invalid UTF-8 replacement") { request.edits[0].replacement = QByteArray::fromHex("c080"); REQUIRE(validateRequest().error == EditError::InvalidUtf8); }
  SECTION("overlap") { request.edits.append({request.edits[0].begin + 1, request.edits[0].end, "0", "3"}); REQUIRE(validateRequest().error == EditError::Overlap); }
  SECTION("duplicate insertion start") { request.edits = {{0, 0, {}, "a"}, {0, 0, {}, "b"}}; REQUIRE(validateRequest().error == EditError::Overlap); }
  SECTION("changed expected bytes") { request.edits[0].expected = "xx"; REQUIRE(validateRequest().error == EditError::ExpectedTextMismatch); }
  REQUIRE(changes.isEmpty());
}

TEST_CASE("Source transactions preserve comments Unicode EOL and one undo", "[source-editing]")
{
  ensureApplication();
  ScintillaEditor editor(nullptr);
  const auto eol = GENERATE(QString("\n"), QString("\r\n"));
  const auto original = QString::fromUtf8("// \xc3\xa9 \xf0\x9f\x98\x80") + eol + "a = 10; /* retain */ b=20;" + eol;
  editor.setPlainText(original);
  const auto bytes = original.toUtf8();
  const auto first = bytes.indexOf("10"), second = bytes.indexOf("20");
  auto request = requestFor(editor, {{first, first + 2, "10", "123"}, {second, second + 2, "20", "4"}});
  const auto before = editor.sourceRevision;
  int notifications = 0;
  QString observed;
  QObject::connect(&editor, &EditorInterface::contentsChanged, [&] { ++notifications; observed = editor.toPlainText(); });
  editor.qsci->SendScintilla(QsciScintillaBase::SCI_SETSEL, static_cast<unsigned long>(second + 2), static_cast<long>(first));
  REQUIRE(apply(editor, request).changed);
  auto updated = original;
  updated.replace("10", "123").replace("20", "4");
  REQUIRE(editor.toPlainText() == updated);
  REQUIRE(observed == updated);
  REQUIRE(notifications == 1);
  REQUIRE(editor.sourceRevision > before);
  REQUIRE(editor.sourceFingerprint == QString::fromLatin1(AgentAnnotations::Store::hash(updated.toUtf8())));
  REQUIRE(editor.isContentModified());
  REQUIRE(editor.qsci->SendScintilla(QsciScintillaBase::SCI_GETANCHOR) > editor.qsci->SendScintilla(QsciScintillaBase::SCI_GETCURRENTPOS));
  editor.undo();
  REQUIRE(editor.toPlainText().toUtf8() == bytes);
  REQUIRE(editor.sourceRevision > before + 1);
  editor.redo();
  REQUIRE(editor.toPlainText() == updated);
}

TEST_CASE("Source transactions insert delete and preserve no-op or rejected editor state", "[source-editing]")
{
  ensureApplication();
  ScintillaEditor editor(nullptr), other(nullptr);
  editor.setPlainText("abc def");
  auto request = requestFor(editor, {{0, 0, {}, "XYZ "}, {4, 7, "def", {}}});
  REQUIRE(apply(editor, request).changed);
  REQUIRE(editor.toPlainText() == "XYZ abc ");
  editor.undo(); REQUIRE(editor.toPlainText() == "abc def");
  const auto revision = editor.sourceRevision;
  const auto dirty = editor.isContentModified();
  const auto undo = editor.canUndo();
  auto noop = requestFor(editor, {{0, 3, "abc", "abc"}});
  REQUIRE_FALSE(apply(editor, noop).changed);
  REQUIRE(editor.sourceRevision == revision);
  REQUIRE(editor.isContentModified() == dirty);
  REQUIRE(editor.canUndo() == undo);
  other.setPlainText("unaffected");
  REQUIRE(apply(other, noop).error == EditError::WrongDocument);
  editor.qsci->setReadOnly(true);
  REQUIRE(apply(editor, requestFor(editor, {{0, 3, "abc", "q"}})).error == EditError::ReadOnly);
  REQUIRE(editor.qsci->isReadOnly());
  editor.qsci->setReadOnly(false);
  auto stale = requestFor(editor, {{0, 3, "abc", "q"}});
  editor.qsci->SendScintilla(QsciScintillaBase::SCI_APPENDTEXT, uintptr_t(1), "!");
  const auto typed = editor.toPlainText();
  REQUIRE(apply(editor, stale).error == EditError::StaleSource);
  REQUIRE(editor.toPlainText() == typed);
  REQUIRE(other.toPlainText() == "unaffected");
}

TEST_CASE("External edit observers close insertion groups before one source transaction", "[source-editing]")
{
  ensureApplication();
  ScintillaEditor editor(nullptr);
  editor.setPlainText("a=1; b=2;");
  editor.qsci->SendScintilla(QsciScintillaBase::SCI_EMPTYUNDOBUFFER);
  editor.qsci->beginUndoAction();
  editor.qsci->SendScintilla(QsciScintillaBase::SCI_APPENDTEXT, uintptr_t(1), "!");
  const auto typed = editor.toPlainText();
  bool insertionGroupOpen = true;
  int starts = 0;
  QString observed;
  QObject::connect(&editor, &EditorInterface::externalEditStarting, &editor, [&] {
    ++starts;
    observed = editor.toPlainText();
    if (insertionGroupOpen) {
      editor.qsci->endUndoAction();
      insertionGroupOpen = false;
    }
  });
  const auto noop = requestFor(editor, {{2, 3, "1", "1"}});
  REQUIRE_FALSE(apply(editor, noop).changed);
  auto rejected = noop; --rejected.sourceRevision;
  REQUIRE(apply(editor, rejected).error == EditError::StaleSource);
  REQUIRE(starts == 0);
  REQUIRE(insertionGroupOpen);
  REQUIRE(apply(editor, requestFor(editor, {{2, 3, "1", "10"}, {7, 8, "2", "20"}})).changed);
  REQUIRE(starts == 1);
  REQUIRE(observed == typed);
  REQUIRE_FALSE(insertionGroupOpen);
  REQUIRE(editor.toPlainText() == "a=10; b=20;!");
  editor.undo(); REQUIRE(editor.toPlainText() == typed);
  editor.undo(); REQUIRE(editor.toPlainText() == "a=1; b=2;");
  editor.appendAnnotationUndo(123);
  REQUIRE(starts == 4); // Each undo and the annotation container action uses the same seam.
  editor.undoFromAnnotations(false);
  REQUIRE(starts == 5);
}

TEST_CASE("Source transactions recheck synchronous observer changes and lifetime", "[source-editing]")
{
  ensureApplication();
  auto *editor = new ScintillaEditor(nullptr);
  editor->setPlainText("cube(1);");
  const auto request = requestFor(*editor, {{5, 6, "1", "2"}});
  SECTION("observer changes source") {
    QObject::connect(editor, &EditorInterface::externalEditStarting, editor, [editor] {
      editor->qsci->SendScintilla(QsciScintillaBase::SCI_APPENDTEXT, uintptr_t(1), "!");
    });
    REQUIRE(apply(*editor, request).error == EditError::StaleSource);
    REQUIRE(editor->toPlainText() == "cube(1);!");
    delete editor;
  }
  SECTION("observer changes read-only state") {
    QObject::connect(editor, &EditorInterface::externalEditStarting, editor, [editor] { editor->qsci->setReadOnly(true); });
    REQUIRE(apply(*editor, request).error == EditError::ReadOnly);
    REQUIRE(editor->toPlainText() == "cube(1);");
    delete editor;
  }
  SECTION("observer closes captured editor") {
    QObject::connect(editor, &EditorInterface::externalEditStarting, [editor] { delete editor; });
    const auto result = apply(*editor, request);
    REQUIRE(result.error == EditError::WrongDocument);
    REQUIRE_FALSE(result.changed);
  }
}
