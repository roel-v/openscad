#include "core/EvaluationCancellation.h"
#include "gui/ScadApi.h"

#include <QDir>
#include <QFileInfo>
#include <QList>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <string>

#include "core/BuiltinContext.h"
#include "core/Builtins.h"
#include "core/EvaluationSession.h"
#include "core/parsersettings.h"
#include "gui/ScintillaEditor.h"

namespace {

bool isInString(const std::u32string& text, const int col)
{
  // first see if we are in a string literal. if so, don't allow auto complete
  bool lastWasEscape = false;
  bool inString = false;
  int dx = 0;
  int count = col;
  while (count-- > 0) {
    const char32_t ch = text.at(dx++);
    if (ch == '\\') lastWasEscape = true;  // next character will be literal handle \"
    else if (lastWasEscape) lastWasEscape = false;
    else if (ch == '"')  // string toggle
      inString = !inString;
  }
  return inString;
}

bool isUseOrInclude(const QString& text, const int col)
{
  const QRegularExpression re("\\s*(use|include)\\s*<[^>]*$");
  const QRegularExpressionMatch match = re.match(text.left(col));
  return match.hasMatch();
}

template <typename C>
QStringList getSorted(const QFileInfoList& list, C cond)
{
  QStringList result;
  for (const auto& info : list) {
    if (cond(info)) {
      result << info.fileName();
    }
  }
  result.sort();
  return result;
}

}  // namespace

ScadApi::ScadApi(ScintillaEditor *editor, QsciLexer *lexer) : QsciAbstractAPIs(lexer), editor(editor)
{
  for (const auto& iter : Builtins::keywordList) {
    QStringList calltipList;
    for (const auto& it : iter.second) calltipList.append(QString::fromStdString(it));

    funcs.append(ApiFunc(QString::fromStdString(iter.first), calltipList));
  }
}

void ScadApi::updateAutoCompletionList(const QStringList& context, QStringList& list)
{
  int line, col;
  editor->qsci->getCursorPosition(&line, &col);
  const auto& text = editor->qsci->text(line);

  if (isInString(text.toStdU32String(), col)) {
    return;
  } else if (isUseOrInclude(text, col)) {
    autoCompleteFolder(context, text, col, list);
  } else {
    autoCompleteFunctions(context, list);
  }
}

void ScadApi::autoCompleteFolder(const QStringList& context, const QString& text, const int col,
                                 QStringList& list)
{
  const QRegularExpression re(R"(\s*(use|include)\s*<\s*)");
  const auto useDir = QFileInfo{text.left(col).replace(re, "")}.dir().path();

  QFileInfoList dirs;
  dirs << QFileInfo(editor->filepath);
  for (const auto& path : get_library_path()) {
    dirs << QFileInfo(QString::fromStdString(path) + "/");
  }

  for (const auto& info : dirs) {
    const auto dir = QDir{info.dir().filePath(useDir)};
    if (!dir.exists()) {
      continue;
    }

    QFileInfoList result;
    const auto& prefix = context.last();
    const auto& infoList =
      dir.entryInfoList(QDir::Dirs | QDir::Files | QDir::Readable | QDir::NoDotAndDotDot);
    for (const auto& info : infoList) {
      if (info.fileName().startsWith(prefix) && (info.isDir() || info.suffix().toLower() == "scad")) {
        result << info;
      }
    }

    list << getSorted(result, [](const QFileInfo& i) { return i.isDir(); });
    list << getSorted(result, [](const QFileInfo& i) { return i.isFile(); });
    list.removeDuplicates();
  }
}

void ScadApi::autoCompleteFunctions(const QStringList& context, QStringList& list)
{
  const QString& c = context.last();
  // for now we only auto-complete functions and modules
  if (c.isEmpty()) {
    return;
  }

  for (const auto& func : funcs) {
    const QString& name = func.get_name();
    if (name.startsWith(c)) {
      if (!list.contains(name)) {
        list << name;
      }
    }
  }

  // for auto-complete on user varables
  if (list.isEmpty()) {
    foreach (const QString& name, userVariableNames) {
      if (name.contains(c)) {
        list.append(name);
      }
    }
  }
}

void ScadApi::autoCompletionSelected(const QString& /*selection*/)
{
}

QStringList ScadApi::callTips(const QStringList& context, int /*commas*/,
                              QsciScintilla::CallTipsStyle /*style*/, QList<int>& /*shifts*/)
{
  QStringList callTips;
  for (const auto& func : funcs) {
    if (func.get_name() == context.at(context.size() - 2)) {
      callTips = func.get_params();
      break;
    }
  }
  return callTips;
}

QStringList ScadApi::completionFromSource(const SourceFile *sourceFile,
                                                             bool flagAutoCompleteIncludeVariables,
                                                             bool flagAutoCompleteIncludeModules,
                                                             bool flagAutoCompleteIncludeFunctions)
{
  if (!sourceFile) return {};

  QStringList userVariableNames;

  if (flagAutoCompleteIncludeVariables) {
    for (const auto& assignment : sourceFile->scope->assignments) {
      userVariableNames << QString::fromStdString(assignment->getName());
    }
  }

  if (flagAutoCompleteIncludeModules) {
    for (const auto& [fnname, fn] : sourceFile->scope->getUserModules()) {
      userVariableNames << QString::fromStdString(fnname);
    }
  }

  if (flagAutoCompleteIncludeFunctions) {
    for (const auto& [modname, mod] : sourceFile->scope->getUserFunctions()) {
      userVariableNames << QString::fromStdString(modname);
    }
  }

  userVariableNames.removeDuplicates();
  return userVariableNames;
}

QStringList ScadApi::completionFromText(const QString& text, bool flagAutoCompleteIncludeVariables,
                                                            bool flagAutoCompleteIncludeModules,
                                                            bool flagAutoCompleteIncludeFunctions)
{


  auto fnCollectName = [&text](const QRegularExpression& re, QStringList& target) {
    auto it = re.globalMatch(text);
    while (it.hasNext()) {
      evaluationCheckpoint();
      auto match = it.next();
      const QString name = match.captured(1);
      if (!name.isEmpty()) {
        target << name;
      }
    }
  };

  QStringList userVariableNames;

  static QRegularExpression const varPattern(R"(^\s*([a-zA-Z_\$][a-zA-Z0-9_]*)\s*=\s*(?!function\b))",
                                             QRegularExpression::MultilineOption);

  static QRegularExpression const modPattern(R"(\bmodule\s+([a-zA-Z_][a-zA-Z0-9_]*)\s*\()");

  static QRegularExpression const funcPattern(R"(\bfunction\s+([a-zA-Z_][a-zA-Z0-9_]*)\s*[\(=])");

  if (flagAutoCompleteIncludeVariables) {
    fnCollectName(varPattern, userVariableNames);
  }

  if (flagAutoCompleteIncludeModules) {
    fnCollectName(modPattern, userVariableNames);
  }

  if (flagAutoCompleteIncludeFunctions) {
    fnCollectName(funcPattern, userVariableNames);
  }

  userVariableNames.removeDuplicates();
  return userVariableNames;
}
