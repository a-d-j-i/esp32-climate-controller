#include "console_session.h"
#include "config.h"
#include "control.h"
#include "state.h"
#include <algorithm>

static const size_t SINK_MAX_LINES = 32;

static std::mutex s_sinksMutex;
static std::vector<ConsoleSink*> s_sinks;

void ConsoleSink::push(const String& line) {
  std::lock_guard<std::mutex> lk(mutex_);
  if (lines_.size() >= SINK_MAX_LINES) {
    lines_.pop_front();
    dropped_++;
  }
  lines_.push_back(line);
}

std::vector<String> ConsoleSink::take() {
  std::lock_guard<std::mutex> lk(mutex_);
  std::vector<String> out;
  if (dropped_) out.push_back("(" + String(dropped_) + " lines skipped)");
  dropped_ = 0;
  out.insert(out.end(), lines_.begin(), lines_.end());
  lines_.clear();
  return out;
}

void consoleBroadcast(const String& line) {
  std::lock_guard<std::mutex> lk(s_sinksMutex);
  for (auto* s : s_sinks) s->push(line);
}

ConsoleSession::ConsoleSession(Console kind, bool terminal, Out write)
    : kind_(kind), terminal_(terminal), write_(write), editor_(terminal, String(HOSTNAME) + "> "), prompt_(String(HOSTNAME) + "> ") {
  editor_.setCompleter([kind](const String& before) { return commandCompletions(before, kind); });
  std::lock_guard<std::mutex> lk(s_sinksMutex);
  s_sinks.push_back(&sink_);
}

ConsoleSession::~ConsoleSession() {
  std::lock_guard<std::mutex> lk(s_sinksMutex);
  s_sinks.erase(std::remove(s_sinks.begin(), s_sinks.end(), &sink_), s_sinks.end());
}

void ConsoleSession::start() { write_(commandBanner() + prompt_); }

bool ConsoleSession::feed(const char* data, int len) {
  String echo;
  for (int i = 0; i < len; i++) {
    auto ev = editor_.feed(data[i], echo);
    if (ev == LineEditor::Event::EndOfInput) {
      write_(echo + "\n");
      return kind_ == Console::Usb;  // Ctrl-D ends a Secure Shell session only
    }
    if (ev == LineEditor::Event::Line) {
      write_(echo);
      echo = "";
      poll();  // events from before the command, so its output stays together
      if (runCommand(editor_.take(), kind_, write_) == CmdResult::Exit && kind_ == Console::Ssh) return false;
      write_(prompt_);
    }
  }
  if (echo.length()) write_(echo);
  return true;
}

void ConsoleSession::poll() {
  std::vector<String> lines = sink_.take();
  if (lines.empty()) return;
  String text;
  if (terminal_) text = "\r\x1b[2K";  // start of the prompt line, cleared
  for (const auto& l : lines) text += l + "\n";
  if (terminal_) text += editor_.render();
  write_(text);
}

static String compactStatus() {
  Settings s;
  Status st;
  {
    Lock lk(g_lock);
    s = g_settings;
    st = g_status;
  }
  String o = sensorReadings();  // `status` says why a sensor is off
  o += String(" · ac ") + (!st.acKnown ? "unknown" : st.acOn ? "on" : "off");
  if (st.acForce.active) o += " (forced)";
  o += String(" · fan ") + (st.fanOn ? "on" : "off");
  if (st.fanForce.active) o += " (forced)";
  o += String(" · ") + (s.mode == Mode::Auto ? "auto" : "manual");
  return o;
}

void watchTick() {
  static uint32_t last = 0;
  uint32_t interval;
  {
    Lock lk(g_lock);
    interval = g_settings.watchS;
  }
  if (!interval || millis() - last < interval * 1000UL) return;
  last = millis();
  consoleBroadcast(timestamp() + " " + compactStatus());
}
