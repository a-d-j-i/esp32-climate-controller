#pragma once
#include <Arduino.h>
#include <mutex>
#include <vector>
#include "dht_sensor.h"

enum class Mode : uint8_t { Auto = 0, Manual = 1 };

// Persistent parameters (see settings.cpp for names, ranges and storage).
struct Settings {
  Mode mode = Mode::Auto;
  float acOnAbove = 27.0;
  float acOffBelow = 25.0;
  float fanOnAbove = 26.0;
  float fanOffBelow = 24.5;
  uint32_t acMinCycleS = 180;
  bool remoteOverride = true;
  bool passwordLogin = true;
  float cabinetOffset = 0;
  float roomOffset = 0;
  bool fanUseRoom = true;      // the fan rule checks the room temperature when it is known
  float fanRoomMargin = 1.0;   // the room must be this much cooler to start the fan
  float boxOffset = 0;
  uint32_t watchS = 60;        // status line on the consoles every this many seconds, 0 = off
  float wifiPowerDbm = 15;     // transmit power; lower draws smaller current peaks
};

struct Reading {
  float temperature = NAN;
  float humidity = NAN;
  uint32_t lastReadMs = 0;  // 0 = never read
  uint32_t attempts = 0;
  uint32_t readErrors = 0;
  uint32_t consecutiveErrors = 0;
  DhtError lastError = DhtError::None;
  uint8_t model = 0;        // detected: 11 or 22, 0 = not yet
  bool problemLogged = false;
};

// A forced output acts as if the temperature had just crossed its threshold: forced on
// lasts until the temperature reaches the off threshold, forced off until it reaches
// the on threshold. The end point only counts once the temperature has been on the
// other side of it, so a force also works when the room is already past that point.
struct Force {
  bool active = false;
  bool on = false;
  bool armed = false;     // temperature has been on the far side of the end point
  uint32_t untilMs = 0;   // optional time limit, 0 = none
};

// Runtime state, not persisted.
struct Status {
  Reading cabinet;
  Reading room;
  Reading box;
  uint32_t readSeq = 0;     // increments with every valid cabinet reading
  bool acKnown = false;     // infrared is one-way: the state is what we last sent or saw
  bool acOn = false;
  uint32_t acChangedMs = 0;
  bool fanOn = false;
  uint32_t fanOnSinceMs = 0;
  Force acForce;
  Force fanForce;
  String lastIr;
  uint32_t lastIrMs = 0;
};

// Guards g_settings, g_status, the actuators and the event log. Recursive so that
// helpers can be called with the lock already held.
extern std::recursive_mutex g_lock;
extern Settings g_settings;
extern Status g_status;

using Lock = std::lock_guard<std::recursive_mutex>;

void logEvent(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
std::vector<String> eventLogSnapshot();
String timestamp();
// Why the chip last restarted, e.g. "brownout (supply voltage dropped)".
const char* resetReasonText();
