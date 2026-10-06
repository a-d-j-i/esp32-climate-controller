#pragma once
#include <Arduino.h>
#include <deque>
#include <mutex>
#include <vector>
#include "commands.h"
#include "line_editor.h"

// Lines waiting to be shown on one console: events and watch status lines. Nothing
// is kept once shown; if a console falls behind, the oldest lines are dropped.
class ConsoleSink {
 public:
  void push(const String& line);
  std::vector<String> take();

 private:
  std::mutex mutex_;
  std::deque<String> lines_;
  uint32_t dropped_ = 0;
};

// Sends a line to every open console (USB, and the Secure Shell session if any).
void consoleBroadcast(const String& line);

// One interactive console: line editing, commands, and live lines printed above the
// prompt. On a terminal the prompt line is cleared, the new lines are printed into the
// terminal's normal scrollback, and the prompt is redrawn with what was being typed.
class ConsoleSession {
 public:
  ConsoleSession(Console kind, bool terminal, Out write);
  ~ConsoleSession();
  void start();
  bool feed(const char* data, int len);  // false when the session should end
  void poll();                           // show pending live lines

 private:
  Console kind_;
  bool terminal_;
  Out write_;
  LineEditor editor_;
  ConsoleSink sink_;
  String prompt_;
};

// Main loop: prints the one-line status every `watch` seconds.
void watchTick();
