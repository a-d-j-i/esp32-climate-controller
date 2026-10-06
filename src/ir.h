#pragma once
#include <Arduino.h>
#include <vector>

struct RawCode {
  uint16_t freqKhz = 38;
  std::vector<uint16_t> timings;  // microseconds, alternating mark and space
  String description;             // what the infrared library decoded, if anything
  int8_t power = -1;              // decoded air conditioner power: 1 on, 0 off, -1 unknown
};

void irBegin();
// Main loop: handles codes arriving at the receiver (learning, or the original remote).
void irPoll();

// Blocks the calling task (not the main loop) until a code arrives or the timeout passes.
bool irLearn(uint32_t timeoutMs, RawCode& code);
void irSendRaw(const RawCode& code);
bool irSendCode(const String& name, String& err);
// "MIDEA · On · Cool · 17C · fan Auto" from a decoded description (power, mode,
// temperature, fan); other codes keep their text, shortened to 70 characters.
String irSummary(const String& description);

// Codes are stored as files in LittleFS under /ir/<name>.bin.
bool irValidName(const String& name);
bool irStoreSave(const String& name, const RawCode& code);
bool irStoreLoad(const String& name, RawCode& code);
bool irStoreExists(const String& name);
bool irStoreDelete(const String& name);
bool irStoreRename(const String& from, const String& to);
std::vector<String> irStoreList();
