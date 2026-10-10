#include "control.h"
#include "config.h"
#include "ir.h"
#include "settings.h"
#include "state.h"

struct Sensor {
  const char* name;
  uint8_t pin;
  DhtSensor* dht;
  Reading Status::*reading;
  float Settings::*offset;
  bool optional;  // shown when it answers, "off" otherwise; its failures are not logged
};

// Indexed by SensorId.
static Sensor s_sensors[] = {
    {"cabinet sensor", PIN_DHT_CABINET, nullptr, &Status::cabinet, &Settings::cabinetOffset, false},
    {"room sensor", PIN_DHT_ROOM, nullptr, &Status::room, &Settings::roomOffset, true},
    {"box sensor", PIN_DHT_BOX, nullptr, &Status::box, &Settings::boxOffset, true},
};
static Sensor& sensor(SensorId id) { return s_sensors[(uint8_t)id]; }
static void sensorTask(void*);
static std::mutex s_readMutex;  // one DHT reading at a time
static uint32_t s_acRetryAfterMs = 0;

// With the fan on the relay's normally closed contact, the coil is energised to stop it.
static void writeRelay(bool fanOn) {
  bool energise = fanOn != FAN_ON_RELAY_NC;
  digitalWrite(PIN_RELAY, (energise != RELAY_ACTIVE_LOW) ? HIGH : LOW);
}

void controlBegin() {
  writeRelay(false);  // set the level before enabling the output to avoid a glitch
  pinMode(PIN_RELAY, OUTPUT);
  xTaskCreatePinnedToCore(sensorTask, "sensor", SENSOR_TASK_STACK, nullptr, 2, nullptr, SENSOR_CORE);
}

bool readingFresh(const Reading& r) {
  Lock lk(g_lock);
  return r.lastReadMs != 0 && millis() - r.lastReadMs < SENSOR_STALE_MS;
}

bool controlSensorFresh() { return readingFresh(g_status.cabinet); }

// Occasional single failures are normal for DHT sensors; this many in a row means
// something is wrong.
static const uint32_t FAILING_AFTER = 3;

const char* sensorName(SensorId id) {
  switch (id) {
    case SensorId::Cabinet: return "cabinet";
    case SensorId::Room: return "room";
    case SensorId::Box: return "box";
  }
  return "?";
}

SensorInfo controlSensorInfo(SensorId id) {
  SensorInfo info;
  Lock lk(g_lock);
  const Sensor& s = sensor(id);
  info.reading = g_status.*s.reading;
  info.fresh = readingFresh(info.reading);
  if (info.reading.consecutiveErrors >= FAILING_AFTER) {
    info.state = SensorState::Failing;
    info.problem = dhtErrorText(info.reading.lastError);
  } else {
    info.state = info.reading.lastReadMs ? SensorState::Ok : SensorState::Starting;
  }
  return info;
}

String sensorReadings() {
  String o;
  for (SensorId id : {SensorId::Cabinet, SensorId::Room, SensorId::Box}) {
    SensorInfo info = controlSensorInfo(id);
    if (o.length()) o += " · ";
    o += sensorName(id);
    o += info.fresh ? String(" ") + String(info.reading.temperature, 1) + " °C" : String(": off");
  }
  return o;
}

String sensorSummary(const SensorInfo& info) {
  String model = info.reading.model ? "DHT" + String(info.reading.model) + ", " : String();
  switch (info.state) {
    case SensorState::Starting: return "detecting";
    case SensorState::Ok: return model + "ok";
    case SensorState::Failing: return model + info.problem;
  }
  return "";
}

static void readSensor(Sensor& s, float offset) {  // sensor task, core 0
  if (!s.dht) {
    s.dht = new DhtSensor(s.pin);
    s.dht->begin();
  }
  DhtResult res;
  {
    std::lock_guard<std::mutex> rl(s_readMutex);
    res = s.dht->read();
  }
  Lock lk(g_lock);
  Reading& r = g_status.*s.reading;
  r.attempts++;
  if (res.error != DhtError::None) {
    r.readErrors++;
    r.consecutiveErrors++;
    r.lastError = res.error;
    if (r.consecutiveErrors == FAILING_AFTER && !r.problemLogged && !s.optional) {
      r.problemLogged = true;
      logEvent("%s: %s", s.name, dhtErrorText(res.error));
    }
    return;
  }
  if (r.problemLogged) {
    r.problemLogged = false;
    logEvent("%s: readings back", s.name);
  }
  if (res.model != r.model) {
    logEvent("%s: DHT%u detected on pin %u", s.name, res.model, s.pin);
    r.model = res.model;
  }
  r.consecutiveErrors = 0;
  r.lastError = DhtError::None;
  r.temperature = res.temperature + offset;
  r.humidity = res.humidity;
  r.lastReadMs = millis() | 1;
  if (&r == &g_status.cabinet) g_status.readSeq++;
}

DhtResult controlSensorTest(SensorId id) {
  Sensor& s = sensor(id);
  std::lock_guard<std::mutex> rl(s_readMutex);
  if (!s.dht) {
    s.dht = new DhtSensor(s.pin);
    s.dht->begin();
  }
  vTaskDelay(pdMS_TO_TICKS(2100));  // the sensor needs 2 s between readings
  return s.dht->read();
}

uint8_t controlSensorPin(SensorId id) { return sensor(id).pin; }

// The DHT protocol is bit-banged with interrupts disabled for about 5 ms. Only the
// interrupts of the current core are disabled, so reading here on core 0 cannot
// disturb the infrared receiver interrupts on core 1. All sensors are read every time,
// so the room and box sensors can be plugged in or out at any time; a missing one
// costs a start signal and a 200 µs wait.
static void sensorTask(void*) {
  for (;;) {
    for (Sensor& s : s_sensors) {
      float offset;
      {
        Lock lk(g_lock);
        offset = g_settings.*s.offset;
      }
      readSensor(s, offset);
    }
    vTaskDelay(pdMS_TO_TICKS(SENSOR_INTERVAL_MS));
  }
}

// Ends a force when the temperature reaches its end point or the time limit passes.
static void checkForce(Force& f, const char* name, float t, float onAbove, float offBelow, uint32_t now) {
  if (!f.active) return;
  if (f.on ? t > offBelow : t < onAbove) f.armed = true;
  bool reached = f.armed && (f.on ? t <= offBelow : t >= onAbove);
  bool expired = f.untilMs && (int32_t)(now - f.untilMs) >= 0;
  if (reached || expired) {
    f.active = false;
    logEvent("%s: forced %s ended (%s), automatic rules resume", name, f.on ? "on" : "off",
             reached ? "temperature reached its end point" : "time limit");
  }
}

static void runAuto() {
  Lock lk(g_lock);
  const Settings& s = g_settings;
  Status& st = g_status;
  if (s.mode != Mode::Auto || !controlSensorFresh()) return;
  float t = st.cabinet.temperature;
  uint32_t now = millis();
  const char* reason = "auto";  // the log line adds the temperatures
  checkForce(st.acForce, "air conditioner", t, s.acOnAbove, s.acOffBelow, now);
  checkForce(st.fanForce, "fan", t, s.fanOnAbove, s.fanOffBelow, now);

  bool fanCanHelp = true;
  if (s.fanUseRoom && readingFresh(st.room)) {
    // Start only with a clear margin; keep running while the room is still clearly
    // cooler, so a cabinet that levels off just above the room lets the fan stop.
    float diff = t - st.room.temperature;
    fanCanHelp = st.fanOn ? diff > s.fanRoomStop : diff >= s.fanRoomMargin;
  }
  // Hysteresis: once on, stay on until the lower threshold. With an unknown state
  // (after boot) send whichever code the temperature calls for. A force was applied
  // when it was set and holds the state until it ends.
  if (!st.acForce.active) {
    bool wantAc = st.acKnown && st.acOn ? t > s.acOffBelow : t >= s.acOnAbove;
    if (!st.acKnown || wantAc != st.acOn) {
      bool cycleOk = !st.acKnown || now - st.acChangedMs >= s.acMinCycleS * 1000UL;
      if (cycleOk && (int32_t)(now - s_acRetryAfterMs) >= 0 && !setAc(wantAc, reason))
        s_acRetryAfterMs = now + 60000;  // missing code: do not retry every reading
    }
  }

  bool acRunning = st.acKnown && st.acOn;
  bool fanCalledFor = st.fanOn ? t > s.fanOffBelow : t >= s.fanOnAbove;
  // A minimum run time, so a cabinet that heats quickly doesn't cycle the fan every few
  // minutes. It only postpones the temperature stop: a room that is no longer cooler
  // and the air conditioner still stop the fan at once. There is no minimum off time,
  // which would let the cabinet overheat while the fan waits.
  if (st.fanOn && s.fanMinOnMin && now - st.fanOnSinceMs < s.fanMinOnMin * 60000UL) fanCalledFor = true;
  bool wantFan = st.fanForce.active ? st.fanForce.on : fanCanHelp && fanCalledFor;
  wantFan = wantFan && !acRunning;
  if (wantFan != st.fanOn) setFan(wantFan, reason);
}

// Main loop, core 1: apply the rules once per new reading.
void controlTick() {
  static uint32_t lastSeq = 0;
  uint32_t seq;
  {
    Lock lk(g_lock);
    seq = g_status.readSeq;
  }
  if (seq == lastSeq) return;
  lastSeq = seq;
  runAuto();
}

static void noteAcOn() {
  g_status.fanForce.active = false;
  if (g_status.fanOn) setFan(false, "air conditioner on");
}

void controlClearForces() {
  Lock lk(g_lock);
  if (g_status.acForce.active || g_status.fanForce.active) logEvent("forced outputs released");
  g_status.acForce.active = false;
  g_status.fanForce.active = false;
}

static void startForce(Force& f, bool on, uint32_t minutes) {
  f.active = true;
  f.on = on;
  f.armed = false;
  f.untilMs = minutes ? (millis() + minutes * 60000UL) | 1 : 0;
}

// Forcing implies automatic mode: the rules take over when the force ends.
static void ensureAuto() {
  if (g_settings.mode == Mode::Auto) return;
  g_settings.mode = Mode::Auto;
  settingsSaveMode();
  logEvent("mode auto (force command)");
}

bool forceAc(bool on, uint32_t minutes, String* err) {
  Lock lk(g_lock);
  if (!setAc(on, "forced", err)) return false;
  ensureAuto();
  startForce(g_status.acForce, on, minutes);
  return true;
}

bool forceFan(bool on, uint32_t minutes, String* err) {
  Lock lk(g_lock);
  if (!setFan(on, "forced", err)) return false;
  ensureAuto();
  startForce(g_status.fanForce, on, minutes);
  return true;
}

void releaseForce(bool ac) {
  Lock lk(g_lock);
  Force& f = ac ? g_status.acForce : g_status.fanForce;
  if (f.active) logEvent("%s: force released (command)", ac ? "air conditioner" : "fan");
  f.active = false;
}

bool setAc(bool on, const char* reason, String* errOut) {
  Lock lk(g_lock);
  String err;
  String name = settingsAcCode(on);
  if (name.length() == 0) err = String("no code assigned for air conditioner ") + (on ? "on" : "off") + " (run: ac learn)";
  if (name.length() == 0 || !irSendCode(name, err)) {
    logEvent("air conditioner %s failed: %s", on ? "on" : "off", err.c_str());
    if (errOut) *errOut = err;
    return false;
  }
  g_status.acOn = on;
  g_status.acKnown = true;
  g_status.acChangedMs = millis();
  logEvent("air conditioner %s (%s) · %s", on ? "on" : "off", reason, sensorReadings().c_str());
  if (on) noteAcOn();
  return true;
}

bool setFan(bool on, const char* reason, String* errOut) {
  Lock lk(g_lock);
  if (on && g_status.acKnown && g_status.acOn) {
    if (errOut) *errOut = "the air conditioner is on; turn it off first (ac off)";
    return false;
  }
  writeRelay(on);
  if (on && !g_status.fanOn) g_status.fanOnSinceMs = millis();
  g_status.fanOn = on;
  logEvent("fan %s (%s) · %s", on ? "on" : "off", reason, sensorReadings().c_str());
  return true;
}

void onRemoteAc(bool on, const String& detail) {
  Lock lk(g_lock);
  g_status.acOn = on;
  g_status.acKnown = true;
  g_status.acChangedMs = millis();
  logEvent("original remote: air conditioner %s · %s (%s)", on ? "on" : "off", sensorReadings().c_str(), detail.c_str());
  if (on) noteAcOn();
  if (g_settings.remoteOverride && g_settings.mode == Mode::Auto) {
    controlClearForces();
    g_settings.mode = Mode::Manual;
    settingsSaveMode();
    logEvent("switched to manual mode because the remote was used (mode auto to resume)");
  }
}
