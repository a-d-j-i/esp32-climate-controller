#pragma once
#include <Arduino.h>
#include <functional>
#include <vector>

enum class CmdResult { Ok, Error, Exit };
enum class Console { Ssh, Usb };

using Out = std::function<void(const String&)>;

// Runs one command line. Output goes through `out` using "\n" line endings.
CmdResult runCommand(const String& line, Console console, const Out& out);
String commandBanner();
// Words that may follow `before` on the command line (Tab completion).
std::vector<String> commandCompletions(const String& before, Console console);

#define MAX_COMMAND_LINE 4096
