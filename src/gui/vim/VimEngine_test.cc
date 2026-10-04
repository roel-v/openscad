#include "gui/vim/engine/Engine.h"
#include <QApplication>
#include <QSettings>
#include <QTemporaryDir>
#include <catch2/catch_all.hpp>
#include <memory>

using namespace Vim;
namespace Catch {
template<> struct StringMaker<QString> {
  static std::string convert(const QString& value) {
    return value.size() > 512 ? value.first(128).toUtf8().toStdString() + "... (" + std::to_string(value.size()) + " UTF-16 units)" : value.toUtf8().toStdString();
  }
};
}
namespace {
void application()
{
  if (QApplication::instance()) return;
  qputenv("QT_QPA_PLATFORM", "offscreen");
  static int argc = 1;
  static char name[] = "vim-engine-test";
  static char *argv[] = {name, nullptr};
  static QTemporaryDir settings;
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
  static auto app = std::make_unique<QApplication>(argc, argv);
}
struct Fixture {
  std::unique_ptr<QsciScintilla> widget;
  std::unique_ptr<Engine> engine;
  std::unique_ptr<ScintillaAccess> sci;
  explicit Fixture(const QString& text, Position caret = 0) {
    application(); widget = std::make_unique<QsciScintilla>(); widget->setUtf8(true);
    widget->setAutoIndent(true);
    widget->setEolMode(QsciScintilla::EolUnix); widget->setText(text);
    sci = std::make_unique<ScintillaAccess>(widget.get()); sci->send(S::SCI_EMPTYUNDOBUFFER); sci->position(caret);
    engine = std::make_unique<Engine>(widget.get());
    QObject::connect(widget.get(), &S::SCN_MODIFIED, widget.get(), [this](int pos, int type, const char *bytes, int length, int, int, int, int, int, int) {
      engine->modified(pos, type, bytes, length);
    });
    engine->setEnabled(true); unnamedRegister() = {};
  }
  void keys(const QString& keys) {
    for (const auto scalar : keys.toUcs4()) {
      const char32_t character = scalar;
      const auto value = QString::fromUcs4(&character, 1);
      const auto result = engine->key({0, {}, value});
      if (result == InputResult::PassToEditor) {
        engine->beginNativeInput();
        const auto pos = sci->pos(); sci->replace(pos, pos, value.toUtf8()); sci->position(pos + value.toUtf8().size());
        engine->endNativeInput();
      }
    }
  }
  void escape() { engine->key({Qt::Key_Escape, {}, {}}); }
  void insert(const QString& value) {
    engine->beginNativeInput();
    const auto pos = sci->pos(); sci->replace(pos, pos, value.toUtf8()); sci->position(pos + value.toUtf8().size());
    engine->endNativeInput();
  }
  void backspace() {
    engine->beginNativeInput(); const auto pos = sci->pos(), begin = sci->prev(pos);
    sci->replace(begin, pos, {}); sci->position(begin); engine->endNativeInput();
  }
  QString text() const { return widget->text(); }
};
}

TEST_CASE("Vim bridge uses real UTF-8 Scintilla positions and native undo", "[vim-engine]")
{
  Fixture f("abcd"); f.keys("l"); REQUIRE(f.sci->pos() == 1);
  f.keys("h"); REQUIRE(f.sci->pos() == 0); REQUIRE_FALSE(f.widget->isUndoAvailable());
  f.keys("iZ"); f.escape(); REQUIRE(f.text() == "Zabcd"); REQUIRE(f.engine->mode() == Mode::Normal);
  f.keys("x"); REQUIRE(f.text() == "abcd"); f.keys("u"); REQUIRE(f.text() == "Zabcd");
  f.keys("u"); REQUIRE(f.text() == "abcd"); REQUIRE_FALSE(f.widget->isUndoAvailable());
}
TEST_CASE("Visual line repeats preserve captured line count and text objects become characterwise", "[vim-engine]")
{
  SECTION("two line delete repeat") {
    Fixture f("a\nb\nc\nd\ne\nf\n"); f.keys("Vjd"); REQUIRE(f.text() == "c\nd\ne\nf\n");
    f.keys("."); REQUIRE(f.text() == "e\nf\n"); f.keys("u"); REQUIRE(f.text() == "c\nd\ne\nf\n");
  }
  SECTION("line visual selects inner word") {
    Fixture f("one two\nthree"); f.keys("Viw"); REQUIRE(f.engine->mode() == Mode::VisualChar);
    f.keys("d"); REQUIRE(f.text() == " two\nthree");
  }
}
TEST_CASE("Counted opened lines and ordinary autoindent replay as one action", "[vim-engine]")
{
  SECTION("o") {
    Fixture f("  one"); f.keys("3oX"); f.escape(); REQUIRE(f.text() == "  one\n  X\n  X\n  X");
    f.keys("u"); REQUIRE(f.text() == "  one"); f.keys("."); REQUIRE(f.text() == "  one\n  X\n  X\n  X");
  }
  SECTION("O") {
    Fixture f("  one"); f.keys("2OX"); f.escape(); REQUIRE(f.text() == "  X\n  X\n  one");
    f.keys("u"); REQUIRE(f.text() == "  one");
  }
  SECTION("indent disabled") {
    Fixture f("  one"); f.widget->setAutoIndent(false); f.keys("oX"); f.escape(); REQUIRE(f.text() == "  one\nX");
    f.keys("u"); f.keys("ccX"); f.escape(); REQUIRE(f.text() == "X");
  }
  SECTION("blank counted opened lines") {
    Fixture f("one"); f.keys("3o"); f.escape(); REQUIRE(f.text() == "one\n\n\n");
    f.keys("u"); REQUIRE(f.text() == "one"); f.keys("."); REQUIRE(f.text() == "one\n\n\n");
    REQUIRE(f.engine->mode() == Mode::Normal);
  }
}
TEST_CASE("Vim composed counts delete six words as one native action", "[vim-engine]")
{
  Fixture f("one two three four five six seven"); f.keys("2d3w"); REQUIRE(f.text() == "seven");
  REQUIRE(f.sci->pos() == 0); f.keys("u"); REQUIRE(f.text() == "one two three four five six seven");
  REQUIRE_FALSE(f.widget->isUndoAvailable());
}
TEST_CASE("Vim engine contexts do not share pending input or buffer state", "[vim-engine]")
{
  Fixture a("one two"), b("red blue"); a.keys("2d"); b.keys("x");
  REQUIRE(a.text() == "one two"); REQUIRE(b.text() == "ed blue");
  a.escape(); a.keys("x"); REQUIRE(a.text() == "ne two"); REQUIRE(b.text() == "ed blue");
}

TEST_CASE("VC01 to VC06 motions words lines and counted finds", "[vim-engine]")
{
  SECTION("VC01") { Fixture f("one two three"); f.keys("w"); REQUIRE(f.sci->pos() == 4); REQUIRE_FALSE(f.widget->isUndoAvailable()); }
  SECTION("VC02") { Fixture f("one two three", 4); f.keys("e"); REQUIRE(f.sci->pos() == 6); f.keys("b"); REQUIRE(f.sci->pos() == 4); }
  SECTION("VC03") { Fixture f("  cube();", 5); f.keys("0"); REQUIRE(f.sci->pos() == 0); f.keys("^"); REQUIRE(f.sci->pos() == 2); f.keys("$"); REQUIRE(f.sci->pos() == 8); }
  SECTION("VC04") { Fixture f("one\n  two\nthree", 11); f.keys("gg"); REQUIRE(f.sci->pos() == 0); f.keys("2G"); REQUIRE(f.sci->pos() == 6); f.keys("G"); REQUIRE(f.sci->pos() == 10); }
  SECTION("VC05") { Fixture f("abcabc"); f.keys("2fc"); REQUIRE(f.sci->pos() == 5); }
  SECTION("VC06") { Fixture f("abc;def;"); f.keys("2t;"); REQUIRE(f.sci->pos() == 6); }
}
TEST_CASE("VC08 VC09 linewise delete yank and paste have coherent history", "[vim-engine]")
{
  const auto eol = GENERATE(QString("\n"), QString("\r\n"));
  const auto original = "a" + eol + "b" + eol + "c" + eol;
  Fixture f(original, 1 + eol.size());
  SECTION("VC08") { f.keys("dd"); REQUIRE(f.text() == "a" + eol + "c" + eol); REQUIRE(unnamedRegister().text == ("b" + eol).toUtf8()); REQUIRE(unnamedRegister().linewise); }
  SECTION("VC09") { f.keys("yy"); REQUIRE_FALSE(f.widget->isUndoAvailable()); f.keys("p"); REQUIRE(f.text() == "a" + eol + "b" + eol + "b" + eol + "c" + eol); REQUIRE(f.sci->pos() == 2 * (1 + eol.size())); }
  f.keys("u"); REQUIRE(f.text() == original); REQUIRE_FALSE(f.widget->isUndoAvailable());
}
TEST_CASE("VC10 VC11 text objects and change plus insertion are one native undo", "[vim-engine]")
{
  SECTION("VC10") { Fixture f("one two", 1); f.keys("diw"); REQUIRE(f.text() == " two"); REQUIRE(f.sci->pos() == 0); f.keys("u"); REQUIRE(f.text() == "one two"); REQUIRE_FALSE(f.widget->isUndoAvailable()); }
  SECTION("VC11") { Fixture f("cube([1, 2, 3]);", 8); f.keys("ci[9,9,9"); f.escape(); REQUIRE(f.text() == "cube([9,9,9]);"); REQUIRE(f.sci->pos() == 10); REQUIRE(f.engine->mode() == Mode::Normal); f.keys("u"); REQUIRE(f.text() == "cube([1, 2, 3]);"); REQUIRE_FALSE(f.widget->isUndoAvailable()); }
}
TEST_CASE("VC12 Visual highlight and deletion include both endpoints", "[vim-engine]")
{
  SECTION("forward") { Fixture f("abcd", 1); f.keys("vl"); REQUIRE(f.sci->range(f.sci->send(S::SCI_GETSELECTIONSTART), f.sci->send(S::SCI_GETSELECTIONEND)) == "bc"); f.keys("d"); REQUIRE(f.text() == "ad"); REQUIRE(f.sci->pos() == 1); }
  SECTION("backward") { Fixture f("abcd", 2); f.keys("vh"); REQUIRE(f.sci->range(f.sci->send(S::SCI_GETSELECTIONSTART), f.sci->send(S::SCI_GETSELECTIONEND)) == "bc"); f.keys("d"); REQUIRE(f.text() == "ad"); REQUIRE(f.sci->pos() == 1); }
}
TEST_CASE("VC13 dot resolves the current word and insertion journal", "[vim-engine]")
{
  Fixture f("one two three"); f.keys("cwX"); f.escape(); REQUIRE(f.text() == "X two three");
  f.keys("w."); REQUIRE(f.text() == "X X three"); REQUIRE(f.sci->pos() == 2); REQUIRE(f.engine->mode() == Mode::Normal);
  f.keys("u"); REQUIRE(f.text() == "X two three"); f.keys("u"); REQUIRE(f.text() == "one two three"); REQUIRE_FALSE(f.widget->isUndoAvailable());
}
TEST_CASE("VC14 ordinary undo redo and VC16 cancellation preserve completed repeat", "[vim-engine]")
{
  Fixture f("one two"); f.keys("dw"); REQUIRE(f.text() == "two");
  f.keys("u"); REQUIRE(f.text() == "one two"); f.engine->key({Qt::Key_R, Qt::ControlModifier, {}}); REQUIRE(f.text() == "two");
  f.keys("u2d"); f.escape(); REQUIRE(f.text() == "one two"); REQUIRE_FALSE(f.widget->isUndoAvailable());
  f.keys("."); REQUIRE(f.text() == "two");
}
TEST_CASE("VC15 literal search repeat wraps and cancellation is nonmodifying", "[vim-engine]")
{
  Fixture f("one two one"); f.engine->submitSearch("two", true); REQUIRE(f.sci->pos() == 4);
  f.keys("n"); REQUIRE(f.sci->pos() == 4); REQUIRE(f.engine->feedback().contains("wrapped"));
  f.keys("?"); f.escape(); REQUIRE(f.sci->pos() == 4); REQUIRE_FALSE(f.widget->isUndoAvailable());
  f.engine->submitSearch("Two", true); REQUIRE(f.engine->feedback().contains("not found")); REQUIRE(f.sci->pos() == 4);
  f.engine->submitSearch("one", false); REQUIRE(f.sci->pos() == 0); f.keys("N"); REQUIRE(f.sci->pos() == 8);
}
TEST_CASE("VC17 Unicode deletion preserves complete non-BMP characters", "[vim-engine]")
{
  Fixture f(QString::fromUtf8("é😀x")); f.keys("lx"); REQUIRE(f.text() == QString::fromUtf8("éx")); REQUIRE(f.sci->pos() == 2);
  f.keys("u"); REQUIRE(f.text() == QString::fromUtf8("é😀x"));
}
TEST_CASE("Vim paired quoted nested and malformed text objects", "[vim-engine]")
{
  const auto pair = GENERATE(QString("()"), QString("[]"), QString("{}"), QString("\"\""), QString("''"));
  const auto original = QString("before ") + pair[0] + "two words" + pair[1] + " after";
  Fixture f(original, 10); f.keys(QString("di") + pair[0]); REQUIRE(f.text() == "before " + pair + " after");
  f.keys("u"); f.keys(QString("da") + pair[1]); REQUIRE(f.text() == "before  after");
  SECTION("unterminated leaves register repeat and text") {
    Fixture broken(QString("abc ") + pair[0] + "unclosed", 7); unnamedRegister() = {"keep", true}; broken.keys(QString("di") + pair[0]);
    REQUIRE(broken.text() == QString("abc ") + pair[0] + "unclosed"); REQUIRE_FALSE(broken.widget->isUndoAvailable()); REQUIRE(unnamedRegister().text == "keep");
  }
}
TEST_CASE("Vim finds repeat reverse Unicode and till boundaries", "[vim-engine]")
{
  SECTION("backward find") { Fixture f("abc;def;", 7); f.keys("F;"); REQUIRE(f.sci->pos() == 3); f.keys(","); REQUIRE(f.sci->pos() == 7); f.keys("T;"); REQUIRE(f.sci->pos() == 4); }
  SECTION("repeat till") { Fixture f("a;a;a;"); f.keys("t;"); REQUIRE(f.sci->pos() == 0); f.keys(";"); REQUIRE(f.sci->pos() == 2); f.keys(";"); REQUIRE(f.sci->pos() == 4); }
  SECTION("Unicode argument") { Fixture f(QString::fromUtf8("a😀b😀")); f.keys(QString::fromUtf8("2f😀")); REQUIRE(f.sci->pos() == 6); }
}
TEST_CASE("Vim unsupported commands overflow read-only and empty buffers", "[vim-engine]")
{
  SECTION("excluded") { const auto keys = GENERATE("R", "rX", "q", "\"", "dR", "gU", ">>", "~"); Fixture f("one two"); f.keys(keys); REQUIRE(f.text() == "one two"); REQUIRE_FALSE(f.widget->isUndoAvailable()); }
  SECTION("overflow") { Fixture f("one two"); f.keys("18446744073709551616dw"); REQUIRE(f.text() == "one two"); REQUIRE_FALSE(f.widget->isUndoAvailable()); }
  SECTION("readonly") { Fixture f("one two"); f.widget->setReadOnly(true); f.keys("w"); REQUIRE(f.sci->pos() == 4); f.keys("diwip"); REQUIRE(f.text() == "one two"); REQUIRE_FALSE(f.widget->isUndoAvailable()); }
  SECTION("empty") { Fixture f(""); f.keys("hjklwbeGggx"); REQUIRE(f.text().isEmpty()); REQUIRE(f.sci->pos() == 0); REQUIRE_FALSE(f.widget->isUndoAvailable()); }
}
TEST_CASE("Vim journal records backspace completion Unicode and long input", "[vim-engine]")
{
  Fixture f("one two"); f.keys("cw"); f.insert(QString::fromUtf8("é😀Z")); f.backspace(); f.insert("completion"); f.escape();
  REQUIRE(f.text() == QString::fromUtf8("é😀completion two")); f.keys("w.");
  REQUIRE(f.text() == QString::fromUtf8("é😀completion é😀completion"));
  f.keys("u"); REQUIRE(f.text() == QString::fromUtf8("é😀completion two"));
  SECTION("long input exceeds old fixed buffer") {
    Fixture big("x y"); big.keys("cw"); const QString many(140000, 'A'); big.insert(many); big.escape(); big.keys("w."); REQUIRE(big.text() == many + " " + many);
  }
}
TEST_CASE("Vim external pre-group hook keeps insertion and transaction undo separate", "[vim-engine]")
{
  Fixture f("one two"); f.keys("cwX"); f.escape();
  f.keys("Ai"); f.engine->beforeExternalMutation();
  f.sci->send(S::SCI_BEGINUNDOACTION); f.sci->replace(0, 0, "external "); f.sci->send(S::SCI_ENDUNDOACTION);
  REQUIRE(f.text() == "external X twoi");
  f.keys("u"); REQUIRE(f.text() == "X twoi"); f.keys("u"); REQUIRE(f.text() == "X two");
  f.sci->position(2); f.keys("."); REQUIRE(f.text() == "X X");
}

TEST_CASE("Vim insert entry counts indent complete lines and EOF", "[vim-engine]")
{
  SECTION("counted insertion") { Fixture f("one"); f.keys("3iX"); f.escape(); REQUIRE(f.text() == "XXXone"); REQUIRE(f.sci->pos() == 2); f.keys("u"); REQUIRE(f.text() == "one"); REQUIRE_FALSE(f.widget->isUndoAvailable()); f.keys("."); REQUIRE(f.text() == "XXXone"); }
  SECTION("entry variants") {
    const auto command = GENERATE("i", "a", "I", "A"); Fixture f("  abc", 3); f.keys(command); f.keys("X"); f.escape();
    const QString expected = QString(command) == "i" ? "  aXbc" : QString(command) == "a" ? "  abXc" : QString(command) == "I" ? "  Xabc" : "  abcX";
    REQUIRE(f.text() == expected); f.keys("u"); REQUIRE(f.text() == "  abc"); REQUIRE_FALSE(f.widget->isUndoAvailable());
  }
  SECTION("open below above") {
    const auto command = GENERATE("o", "O"); Fixture f("\tone\ntwo", 2); f.keys(command); f.keys("X"); f.escape();
    REQUIRE(f.text() == (QString(command) == "o" ? "\tone\n\tX\ntwo" : "\tX\n\tone\ntwo"));
    f.keys("u"); REQUIRE(f.text() == "\tone\ntwo"); REQUIRE_FALSE(f.widget->isUndoAvailable());
  }
  SECTION("cc and counted cc") { Fixture f("  one\n  two\nthree", 4); f.keys("2ccX"); f.escape(); REQUIRE(f.text() == "  X\nthree"); REQUIRE(f.sci->pos() == 2); f.keys("u"); REQUIRE(f.text() == "  one\n  two\nthree"); }
  SECTION("Visual line selection") { Fixture f("a\nb\nc", 2); f.keys("Vk"); REQUIRE(f.sci->range(f.sci->send(S::SCI_GETSELECTIONSTART), f.sci->send(S::SCI_GETSELECTIONEND)) == "a\nb\n"); f.keys("d"); REQUIRE(f.text() == "c"); REQUIRE(unnamedRegister().linewise); f.keys("u"); REQUIRE(f.text() == "a\nb\nc"); }
  SECTION("final line yank paste preserves no trailing EOL") { Fixture f("a"); f.keys("yyp"); REQUIRE(f.text() == "a\na"); f.keys("u"); REQUIRE(f.text() == "a"); }
  SECTION("last line delete") { Fixture f("a\nb", 2); f.keys("dd"); REQUIRE(f.text() == "a"); f.keys("u"); REQUIRE(f.text() == "a\nb"); }
  SECTION("unterminated line counted paste") { Fixture f("a"); f.keys("yy2p"); REQUIRE(f.text() == "a\na\na"); f.keys("u"); REQUIRE(f.text() == "a"); }
}
TEST_CASE("Vim nested escaped objects character columns folds and CRLF journals", "[vim-engine]")
{
  SECTION("nested") { Fixture f("x([1,[2,3],4])", 7); f.keys("2di["); REQUIRE(f.text() == "x([])"); f.keys("u"); REQUIRE(f.text() == "x([1,[2,3],4])"); }
  SECTION("escaped quote") { Fixture f("say(\"a\\\"b\");", 9); f.keys("di\""); REQUIRE(f.text() == "say(\"\");"); }
  SECTION("combining and tab character columns") { Fixture f(QString::fromUtf8("é́😀\n\txy\nabcdef")); f.keys("2lj"); REQUIRE(f.sci->pos() == 11); f.keys("j"); REQUIRE(f.sci->pos() == 15); f.keys("kk"); REQUIRE(f.sci->pos() == 4); }
  SECTION("logical folded line") { Fixture f("a\nbb\ncc\ndd"); f.sci->send(S::SCI_SETFOLDLEVEL, 0, S::SC_FOLDLEVELBASE | S::SC_FOLDLEVELHEADERFLAG); f.sci->send(S::SCI_SETFOLDLEVEL, 1, S::SC_FOLDLEVELBASE + 1); f.sci->send(S::SCI_SETFOLDLEVEL, 2, S::SC_FOLDLEVELBASE + 1); f.sci->send(S::SCI_SETFOLDEXPANDED, 0, 0); f.sci->send(S::SCI_HIDELINES, 1, 2); f.keys("j"); REQUIRE(f.sci->pos() == 2); REQUIRE(f.sci->send(S::SCI_GETLINEVISIBLE, 1) != 0); }
  SECTION("CRLF inserted bytes replay exactly") { Fixture f("one two"); f.widget->setEolMode(QsciScintilla::EolWindows); f.keys("cw"); f.insert("A\r\nB"); f.escape(); f.sci->position(f.text().toUtf8().indexOf("two")); f.keys("."); REQUIRE(f.text() == "A\r\nB A\r\nB"); }
  SECTION("failed repeat leaves source register and last repeat") {
    Fixture f("abc", 2); f.keys("ix"); f.backspace(); f.backspace(); f.escape(); REQUIRE(f.text() == "ac");
    unnamedRegister() = {"keep", false}; f.sci->position(0); f.keys("."); REQUIRE(f.text() == "ac"); REQUIRE(unnamedRegister().text == "keep");
  }
}
