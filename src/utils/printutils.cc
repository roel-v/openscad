#include "utils/printutils.h"
#include "utils/ScopedOutput.h"

#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/circular_buffer.hpp>
#include <cassert>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <iostream>
#include <list>
#include <set>
#include <string>

#include "utils/exceptions.h"

namespace fs = std::filesystem;

thread_local std::set<std::string> printedDeprecations;
thread_local std::list<std::string> print_messages_stack;
thread_local OutputHandlerFunc *outputhandler = nullptr;
thread_local void *outputhandler_data = nullptr;
std::string OpenSCAD::debug("");
thread_local bool OpenSCAD::quiet = false;
thread_local bool OpenSCAD::hardwarnings = false;
thread_local int OpenSCAD::traceDepth = 12;
thread_local bool OpenSCAD::traceUsermoduleParameters = true;
thread_local bool OpenSCAD::parameterCheck = true;
thread_local bool OpenSCAD::rangeCheck = false;

namespace {

thread_local std::list<struct Message> log_messages_stack;
thread_local OutputHandlerFunc2 *outputhandler2 = nullptr;
thread_local boost::circular_buffer<std::string> lastmessages(5);
thread_local boost::circular_buffer<struct Message> lastlogmessages(5);

thread_local bool no_throw;
thread_local bool deferred;

}  // namespace

struct ScopedOutput::State {
  OutputHandlerFunc *handler = outputhandler;
  OutputHandlerFunc2 *handler2 = outputhandler2;
  void *data = outputhandler_data;
  bool oldQuiet = OpenSCAD::quiet, oldHard = OpenSCAD::hardwarnings;
  bool oldNoThrow = no_throw, oldDeferred = deferred;
  decltype(printedDeprecations) deprecations = std::move(printedDeprecations);
  decltype(print_messages_stack) messages = std::move(print_messages_stack);
  decltype(log_messages_stack) logs = std::move(log_messages_stack);
  decltype(lastmessages) last = lastmessages;
  decltype(lastlogmessages) lastlog = lastlogmessages;
};
ScopedOutput::ScopedOutput(void (*handler)(const Message&, void *), void *data)
  : state(std::make_unique<State>())
{
  set_output_handler(handler, nullptr, data);
  OpenSCAD::quiet = OpenSCAD::hardwarnings = no_throw = deferred = false;
  printedDeprecations.clear();
  print_messages_stack.clear();
  log_messages_stack.clear();
  lastmessages.clear();
  lastlogmessages.clear();
}
ScopedOutput::~ScopedOutput()
{
  set_output_handler(state->handler, state->handler2, state->data);
  OpenSCAD::quiet = state->oldQuiet;
  OpenSCAD::hardwarnings = state->oldHard;
  no_throw = state->oldNoThrow;
  deferred = state->oldDeferred;
  printedDeprecations = std::move(state->deprecations);
  print_messages_stack = std::move(state->messages);
  log_messages_stack = std::move(state->logs);
  lastmessages = std::move(state->last);
  lastlogmessages = std::move(state->lastlog);
}

void set_output_handler(OutputHandlerFunc *newhandler, OutputHandlerFunc2 *newhandler2, void *userdata)
{
  outputhandler = newhandler;
  outputhandler2 = newhandler2;
  outputhandler_data = userdata;
}

void no_exceptions_for_warnings()
{
  no_throw = true;
  deferred = false;
}

bool would_have_thrown()
{
  const auto would_throw = deferred;
  no_throw = false;
  deferred = false;
  return would_throw;
}

void print_messages_push()
{
  print_messages_stack.emplace_back();
}

void print_messages_pop()
{
  const std::string msg = print_messages_stack.back();
  print_messages_stack.pop_back();
  if (print_messages_stack.size() > 0 && !msg.empty()) {
    if (!print_messages_stack.back().empty()) {
      print_messages_stack.back() += "\n";
    }
    print_messages_stack.back() += msg;
  }
}

void PRINT(const Message& msgObj)
{
  if (msgObj.msg.empty() && msgObj.group != message_group::Echo) return;

  if (print_messages_stack.size() > 0) {
    if (!print_messages_stack.back().empty()) {
      print_messages_stack.back() += "\n";
    }
    print_messages_stack.back() += msgObj.str();
  }

  PRINT_NOCACHE(msgObj);

  // to error log
  if (outputhandler2 &&
      !(msgObj.group == message_group::NONE || msgObj.group == message_group::Echo ||
        msgObj.group == message_group::Trace || msgObj.group == message_group::HtmlLink)) {
    outputhandler2(msgObj, outputhandler_data);
  }
}

void PRINT_NOCACHE(const Message& msgObj)
{
  if (msgObj.msg.empty() && msgObj.group != message_group::Echo) return;

  const auto msg = msgObj.str();

  if (msgObj.group == message_group::Warning || msgObj.group == message_group::Error ||
      msgObj.group == message_group::Trace) {
    size_t i;
    for (i = 0; i < lastmessages.size(); ++i) {
      if (lastmessages[i] != msg) break;
    }
    if (i == 5) return;  // Suppress output after 5 equal ERROR or WARNING outputs.
    lastmessages.push_back(msg);
  }
  if (!deferred)
    if (!OpenSCAD::quiet || msgObj.group == message_group::Error) {
      if (!outputhandler) {
        std::cerr << msg << "\n";
      } else {
        outputhandler(msgObj, outputhandler_data);
      }
    }
  if (!std::current_exception()) {
    if ((OpenSCAD::hardwarnings && msgObj.group == message_group::Warning) ||
        (no_throw && msgObj.group == message_group::Error)) {
      if (no_throw) deferred = true;
      else throw HardWarningException(msgObj.msg);
    }
  }
}

void PRINTDEBUG(const std::string& filename, const std::string& msg)
{
  // see printutils.h for usage instructions
  if (OpenSCAD::debug == "") return;
  const std::string shortfname = fs::path(filename).stem().generic_string();
  std::string lowshortfname(shortfname);
  boost::algorithm::to_lower(lowshortfname);
  std::string lowdebug(OpenSCAD::debug);
  boost::algorithm::to_lower(lowdebug);
  if (OpenSCAD::debug == "all" || lowdebug.find(lowshortfname) != std::string::npos) {
    const Message msgObj{shortfname + ": " + msg, message_group::NONE, Location::NONE, ""};
    PRINT_NOCACHE(msgObj);
  }
}

const std::string& quoted_string(const std::string& str)
{
  static std::string buf;
  buf = str;
  boost::replace_all(buf, "\n", "\\n");
  return buf;
}

std::string two_digit_exp_format(std::string doublestr)
{
#ifdef _WIN32
  size_t exppos = doublestr.find('e');
  if (exppos != std::string::npos) {
    exppos += 2;
    if (doublestr[exppos] == '0') doublestr.erase(exppos, 1);
  }
#endif
  return doublestr;
}

std::string two_digit_exp_format(double x)
{
  return two_digit_exp_format(std::to_string(x));
}

void resetSuppressedMessages()
{
  printedDeprecations.clear();
  lastmessages.clear();
}

std::string getGroupName(const enum message_group& group)
{
  switch (group) {
  case message_group::NONE:
  case message_group::Warning:
  case message_group::HtmlLink:
  case message_group::UI_Warning:     return "WARNING";
  case message_group::Error:
  case message_group::UI_Error:       return "ERROR";
  case message_group::Font_Warning:   return "FONT-WARNING";
  case message_group::Export_Warning: return "EXPORT-WARNING";
  case message_group::Export_Error:   return "EXPORT-ERROR";
  case message_group::Parser_Error:   return "PARSER-ERROR";
  case message_group::Trace:          return "TRACE";
  case message_group::Deprecated:     return "DEPRECATED";
  case message_group::Echo:           return "ECHO";
  default:                            assert(false && "Unhandled message group name"); return "";
  }
}

std::string getGroupColor(const enum message_group& group)
{
  switch (group) {
  case message_group::Warning:
  case message_group::Deprecated:
  case message_group::UI_Warning:
  case message_group::Font_Warning: return "#ffffb0";
  case message_group::Error:
  case message_group::UI_Error:
  case message_group::Export_Error:
  case message_group::Parser_Error: return "#ffb0b0";
  case message_group::Trace:        return "#d0d0ff";
  default:                          return "transparent";
  }
}

bool getGroupTextPlain(const enum message_group& group)
{
  return group == message_group::NONE || group == message_group::Echo;
}

std::string quoteVar(const std::string& varname)
{
  return '"' + varname + '"';
}
