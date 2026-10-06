#pragma once
#include <Arduino.h>
#include <functional>
#include <mutex>
#include <vector>

// Commands typed on any console (USB or Secure Shell), shared by all of them. Kept in
// memory only, so a reboot clears it. Commands that contain secrets are not kept.
class CommandHistory {
 public:
  static const size_t LINES = 20;
  static const size_t MAX_LEN = 512;  // longer lines (e.g. pasted keys) are not kept

  void add(const String& line) {
    if (!line.length() || line.length() > MAX_LEN || secret(line)) return;
    std::lock_guard<std::mutex> lk(mutex_);
    if (!lines_.empty() && lines_.back() == line) return;
    lines_.push_back(line);
    if (lines_.size() > LINES) lines_.erase(lines_.begin());
  }

  size_t size() {
    std::lock_guard<std::mutex> lk(mutex_);
    return lines_.size();
  }

  // back = 1 is the newest entry.
  String fromNewest(size_t back) {
    std::lock_guard<std::mutex> lk(mutex_);
    return back && back <= lines_.size() ? lines_[lines_.size() - back] : String();
  }

 private:
  static bool secret(const String& line) {
    return line.startsWith("passwd") || line.startsWith("wifi set");
  }
  std::mutex mutex_;
  std::vector<String> lines_;
};

inline CommandHistory& commandHistory() {
  static CommandHistory h;
  return h;
}

// Line editing for a terminal sending raw keystrokes, in the style of a shell:
//   Left/Right, Home/End, Ctrl-A/E/B/F     move the cursor
//   Backspace, Delete, Ctrl-U/K/W          delete before / at / to start / to end / a word
//   Up/Down, Ctrl-P/N                      command history shared by all consoles
//   Tab                                    complete; Tab twice lists the choices
//   Ctrl-C discard the line, Ctrl-D end on an empty line, Ctrl-L clear the screen
// Without a terminal (echo off) the client edits locally and sends whole lines.
class LineEditor {
 public:
  enum class Event { None, Line, EndOfInput };

  static const size_t MAX_LINE = 4096;

  LineEditor(bool terminal, const String& prompt) : terminal_(terminal), prompt_(prompt) {}

  // Gets the line up to the start of the word being completed and returns every word
  // that may come next; the editor keeps the ones starting with what was typed.
  using Completer = std::function<std::vector<String>(const String& before)>;
  void setCompleter(Completer c) { completer_ = c; }

  // Processes one byte and appends to `out` what the terminal needs to show.
  Event feed(char c, String& out) {
    bool tabAgain = lastTab_;
    lastTab_ = false;
    if (esc_ != 0) return escape(c, out);
    bool afterCr = lastCr_;
    lastCr_ = c == '\r';
    switch (c) {
      case '\n':
        if (afterCr) return Event::None;  // "\r\n" counts once
        // fall through
      case '\r':
        if (terminal_) out += "\n";
        return Event::Line;
      case 0x1b: esc_ = 1; param_ = ""; return Event::None;
      case 0x03:  // Ctrl-C
        line_ = "";
        cursor_ = 0;
        if (terminal_) out += "^C\n";
        return Event::Line;
      case 0x04:  // Ctrl-D
        if (line_.length() == 0) return Event::EndOfInput;
        deleteAt(out);
        return Event::None;
      case 0x7f:
      case 0x08: backspace(out); return Event::None;
    }
    if (!terminal_) {
      if (c >= 0x20 && c < 0x7f && line_.length() < MAX_LINE) line_ += c;
      cursor_ = line_.length();
      return Event::None;
    }
    switch (c) {
      case 0x01: cursor_ = 0; redraw(out); break;                     // Ctrl-A
      case 0x05: cursor_ = line_.length(); redraw(out); break;        // Ctrl-E
      case 0x02: left(out); break;                                    // Ctrl-B
      case 0x06: right(out); break;                                   // Ctrl-F
      case 0x10: historyUp(out); break;                               // Ctrl-P
      case 0x0e: historyDown(out); break;                             // Ctrl-N
      case 0x15: line_ = line_.substring(cursor_); cursor_ = 0; redraw(out); break;  // Ctrl-U
      case 0x0b: line_ = line_.substring(0, cursor_); redraw(out); break;           // Ctrl-K
      case 0x17: deleteWord(out); break;                              // Ctrl-W
      case 0x0c: out += "\x1b[2J\x1b[H"; out += render(); break;      // Ctrl-L
      case '\t': complete(tabAgain, out); lastTab_ = true; break;
      default:
        if (c >= 0x20 && c < 0x7f && line_.length() < MAX_LINE) insert(c, out);
    }
    return Event::None;
  }

  // The finished line; non-empty lines join the shared history.
  String take() {
    String l = line_;
    l.trim();
    commandHistory().add(l);
    back_ = 0;
    line_ = "";
    cursor_ = 0;
    return l;
  }

  // Prompt and line with the cursor in place, to redraw after printing other output.
  String render() const {
    String s = prompt_ + line_ + "\x1b[K";
    size_t back = line_.length() - cursor_;
    if (back) {
      char buf[16];
      snprintf(buf, sizeof buf, "\x1b[%uD", (unsigned)back);
      s += buf;
    }
    return s;
  }

  const String& prompt() const { return prompt_; }

 private:
  void redraw(String& out) { out += "\r" + render(); }

  void insert(char c, String& out) {
    if (cursor_ == line_.length()) {
      line_ += c;
      cursor_++;
      out += c;
      return;
    }
    line_ = line_.substring(0, cursor_) + String(c) + line_.substring(cursor_);
    cursor_++;
    redraw(out);
  }

  void backspace(String& out) {
    if (cursor_ == 0) return;
    if (!terminal_ || cursor_ == line_.length()) {
      line_.remove(line_.length() - 1);
      cursor_ = line_.length();
      if (terminal_) out += "\b \b";
      return;
    }
    line_ = line_.substring(0, cursor_ - 1) + line_.substring(cursor_);
    cursor_--;
    redraw(out);
  }

  void deleteAt(String& out) {
    if (!terminal_ || cursor_ >= line_.length()) return;
    line_ = line_.substring(0, cursor_) + line_.substring(cursor_ + 1);
    redraw(out);
  }

  void deleteWord(String& out) {
    size_t start = cursor_;
    while (start > 0 && line_[start - 1] == ' ') start--;
    while (start > 0 && line_[start - 1] != ' ') start--;
    line_ = line_.substring(0, start) + line_.substring(cursor_);
    cursor_ = start;
    redraw(out);
  }

  void insertText(const String& text, String& out) {
    if (!text.length() || line_.length() + text.length() > MAX_LINE) return;
    line_ = line_.substring(0, cursor_) + text + line_.substring(cursor_);
    cursor_ += text.length();
    redraw(out);
  }

  void complete(bool tabAgain, String& out) {
    if (!completer_) return;
    size_t start = cursor_;
    while (start > 0 && line_[start - 1] != ' ') start--;
    String word = line_.substring(start, cursor_);
    std::vector<String> matches;
    for (const auto& c : completer_(line_.substring(0, start)))
      if (c.startsWith(word)) matches.push_back(c);
    if (matches.empty()) {
      out += "\a";
      return;
    }
    if (matches.size() == 1) {
      bool spaceFollows = cursor_ < line_.length() && line_[cursor_] == ' ';
      insertText(matches[0].substring(word.length()) + (spaceFollows ? "" : " "), out);
      return;
    }
    String common = matches[0];
    for (const auto& m : matches) {
      size_t i = 0;
      while (i < common.length() && i < m.length() && common[i] == m[i]) i++;
      common = common.substring(0, i);
    }
    if (common.length() > word.length()) {
      insertText(common.substring(word.length()), out);
      return;
    }
    if (!tabAgain) {
      out += "\a";
      return;
    }
    out += "\n";
    for (size_t i = 0; i < matches.size(); i++) out += (i ? "  " : "") + matches[i];
    out += "\n" + render();
  }

  void left(String& out) {
    if (cursor_ == 0) return;
    cursor_--;
    out += "\x1b[D";
  }

  void right(String& out) {
    if (cursor_ >= line_.length()) return;
    cursor_++;
    out += "\x1b[C";
  }

  // back_ counts entries from the newest; 0 is the line being typed (the draft).
  void historyUp(String& out) {
    if (back_ >= commandHistory().size()) return;
    if (back_ == 0) draft_ = line_;
    line_ = commandHistory().fromNewest(++back_);
    cursor_ = line_.length();
    redraw(out);
  }

  void historyDown(String& out) {
    if (back_ == 0) return;
    back_--;
    line_ = back_ == 0 ? draft_ : commandHistory().fromNewest(back_);
    cursor_ = line_.length();
    redraw(out);
  }

  // ESC [ ... final  or  ESC O final: arrows, Home, End, Delete. Others are ignored.
  Event escape(char c, String& out) {
    if (esc_ == 1) {
      esc_ = (c == '[' || c == 'O') ? 2 : 0;
      return Event::None;
    }
    if ((c >= '0' && c <= '9') || c == ';') {
      if (param_.length() < 8) param_ += c;
      return Event::None;
    }
    esc_ = 0;
    if (!terminal_) return Event::None;
    switch (c) {
      case 'A': historyUp(out); break;
      case 'B': historyDown(out); break;
      case 'C': right(out); break;
      case 'D': left(out); break;
      case 'H': cursor_ = 0; redraw(out); break;
      case 'F': cursor_ = line_.length(); redraw(out); break;
      case '~':
        if (param_ == "1" || param_ == "7") { cursor_ = 0; redraw(out); }
        else if (param_ == "4" || param_ == "8") { cursor_ = line_.length(); redraw(out); }
        else if (param_ == "3") deleteAt(out);
        break;
    }
    return Event::None;
  }

  bool terminal_;
  String prompt_;
  String line_;
  size_t cursor_ = 0;
  size_t back_ = 0;
  String draft_;
  int esc_ = 0;
  String param_;
  bool lastCr_ = false;
  bool lastTab_ = false;
  Completer completer_;
};

// Terminals expect "\r\n".
inline String toCrlf(const String& text) {
  String t;
  t.reserve(text.length() + 16);
  for (size_t i = 0; i < text.length(); i++) {
    if (text[i] == '\n') t += '\r';
    t += text[i];
  }
  return t;
}
