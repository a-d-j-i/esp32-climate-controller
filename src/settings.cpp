#include "settings.h"
#include "state.h"
#include <Preferences.h>
#include <mbedtls/sha256.h>

static Preferences s_prefs;

enum class PType { Float, UInt, Bool };

struct ParamDef {
  const char* name;  // also the storage key, at most 15 characters
  PType type;
  void* ptr;
  float minV, maxV;
  const char* unit;
  bool reboot;
  const char* help;
};

static const ParamDef PARAMS[] = {
    {"ac_on_above", PType::Float, &g_settings.acOnAbove, 10, 40, "°C", false,
     "auto: air conditioner on at or above"},
    {"ac_off_below", PType::Float, &g_settings.acOffBelow, 10, 40, "°C", false,
     "auto: air conditioner off at or below"},
    {"fan_on_above", PType::Float, &g_settings.fanOnAbove, 10, 40, "°C", false,
     "auto: fan on at or above"},
    {"fan_off_below", PType::Float, &g_settings.fanOffBelow, 10, 40, "°C", false,
     "auto: fan off at or below"},
    {"ac_min_cycle", PType::UInt, &g_settings.acMinCycleS, 0, 3600, "s", false,
     "minimum time between automatic air conditioner switches"},
    {"wifi_power", PType::Float, &g_settings.wifiPowerDbm, 2, 19.5, "dBm", false,
     "Wi-Fi transmit power (2-19.5); lower it on a weak supply, raise it for range"},
    {"watch", PType::UInt, &g_settings.watchS, 0, 3600, "s", false,
     "print a one-line status on the consoles every this many seconds (0 = off)"},
    {"remote_override", PType::Bool, &g_settings.remoteOverride, 0, 1, "", false,
     "switch to manual mode when the original remote is used"},
    {"password_login", PType::Bool, &g_settings.passwordLogin, 0, 1, "", false,
     "allow Secure Shell login with the password (keys always work)"},
    {"cabinet_offset", PType::Float, &g_settings.cabinetOffset, -10, 10, "°C", false,
     "calibration added to the cabinet temperature"},
    {"room_offset", PType::Float, &g_settings.roomOffset, -10, 10, "°C", false,
     "calibration added to the room temperature"},
    {"fan_use_room", PType::Bool, &g_settings.fanUseRoom, 0, 1, "", false,
     "auto: start the fan only when the room is cooler (off: the room is only shown)"},
    {"fan_room_margin", PType::Float, &g_settings.fanRoomMargin, 0, 10, "°C", false,
     "auto: the room must be this much cooler than the cabinet to start the fan"},
    {"box_offset", PType::Float, &g_settings.boxOffset, -10, 10, "°C", false,
     "calibration added to the box temperature"},
};

static const ParamDef* findParam(const String& name) {
  for (const auto& p : PARAMS)
    if (name == p.name) return &p;
  return nullptr;
}

static String formatValue(const ParamDef& p) {
  switch (p.type) {
    case PType::Float: return String(*(float*)p.ptr, 1);
    case PType::UInt: return String(*(uint32_t*)p.ptr);
    case PType::Bool: return *(bool*)p.ptr ? "on" : "off";
  }
  return "";
}

static bool parseValue(const ParamDef& p, String text, String& err) {
  text.toLowerCase();
  const char* s = text.c_str();
  char* end = nullptr;
  switch (p.type) {
    case PType::Bool:
      if (text == "on" || text == "1" || text == "true" || text == "yes") *(bool*)p.ptr = true;
      else if (text == "off" || text == "0" || text == "false" || text == "no") *(bool*)p.ptr = false;
      else { err = String(p.name) + ": expected on or off"; return false; }
      return true;
    case PType::Float: {
      float v = strtof(s, &end);
      if (end == s || *end || v < p.minV || v > p.maxV) {
        err = String(p.name) + ": expected a number from " + String(p.minV, 0) + " to " + String(p.maxV, 0);
        return false;
      }
      *(float*)p.ptr = v;
      return true;
    }
    case PType::UInt: {
      unsigned long v = strtoul(s, &end, 10);
      if (end == s || *end || v < p.minV || v > p.maxV) {
        err = String(p.name) + ": expected a whole number from " + String(p.minV, 0) + " to " + String(p.maxV, 0);
        return false;
      }
      *(uint32_t*)p.ptr = v;
      return true;
    }
  }
  return false;
}

static bool validate(const Settings& s, String& err) {
  if (s.acOffBelow >= s.acOnAbove) err = "ac_off_below must be lower than ac_on_above";
  else if (s.fanOffBelow >= s.fanOnAbove) err = "fan_off_below must be lower than fan_on_above";
  else return true;
  return false;
}

static void persist(const ParamDef& p) {
  switch (p.type) {
    case PType::Float: s_prefs.putFloat(p.name, *(float*)p.ptr); break;
    case PType::UInt: s_prefs.putUInt(p.name, *(uint32_t*)p.ptr); break;
    case PType::Bool: s_prefs.putBool(p.name, *(bool*)p.ptr); break;
  }
}

void settingsLoad() {
  Lock lk(g_lock);
  s_prefs.begin("climate", false);
  for (const auto& p : PARAMS) {
    if (!s_prefs.isKey(p.name)) continue;  // keep the default; reading a missing key logs an error
    switch (p.type) {
      case PType::Float: *(float*)p.ptr = s_prefs.getFloat(p.name, *(float*)p.ptr); break;
      case PType::UInt: *(uint32_t*)p.ptr = s_prefs.getUInt(p.name, *(uint32_t*)p.ptr); break;
      case PType::Bool: *(bool*)p.ptr = s_prefs.getBool(p.name, *(bool*)p.ptr); break;
    }
  }
  g_settings.mode = s_prefs.isKey("mode") && s_prefs.getUChar("mode", 0) == 1 ? Mode::Manual : Mode::Auto;
  // Settings saved by older firmware under earlier names.
  const char* renamed[][2] = {{"out_temp_offset", "room_offset"},
                              {"temp_offset", "cabinet_offset"}, {"fan_out_margin", "fan_room_margin"}};
  bool migrated = false;
  for (const auto& r : renamed) {
    if (!s_prefs.isKey(r[0])) continue;
    if (!s_prefs.isKey(r[1])) {
      const ParamDef* p = findParam(r[1]);
      if (p && p->type == PType::Bool) *(bool*)p->ptr = s_prefs.getBool(r[0]);
      if (p && p->type == PType::Float) *(float*)p->ptr = s_prefs.getFloat(r[0]);
      if (p) persist(*p);
    }
    s_prefs.remove(r[0]);
    migrated = true;
  }
  if (migrated) logEvent("settings: moved to the cabinet / room names");
  // No longer used: the sensor model is detected, and the room and box sensors are
  // shown whenever they answer.
  for (const char* key : {"sensor_type", "out_sensor_type", "outside_sensor", "room_sensor", "box_sensor"})
    if (s_prefs.isKey(key)) s_prefs.remove(key);
  String err;
  if (!validate(g_settings, err)) {
    g_settings = Settings();
    logEvent("stored settings invalid (%s), using defaults", err.c_str());
  }
}

void settingsSaveMode() {
  Lock lk(g_lock);
  s_prefs.putUChar("mode", g_settings.mode == Mode::Manual ? 1 : 0);
}

bool settingsSet(const std::vector<std::pair<String, String>>& pairs, String& err,
                 bool& needsReboot) {
  Lock lk(g_lock);
  Settings backup = g_settings;
  std::vector<const ParamDef*> touched;
  needsReboot = false;
  for (const auto& kv : pairs) {
    const ParamDef* p = findParam(kv.first);
    if (!p) err = "unknown parameter '" + kv.first + "' (see: get)";
    if (!p || !parseValue(*p, kv.second, err)) {
      g_settings = backup;
      return false;
    }
    touched.push_back(p);
    needsReboot |= p->reboot;
  }
  if (!validate(g_settings, err)) {
    g_settings = backup;
    return false;
  }
  for (const auto* p : touched) persist(*p);
  return true;
}

// Pads to a display width; the degree sign is two bytes but one column.
static String pad(const String& s, size_t width) {
  size_t cols = 0;
  for (size_t i = 0; i < s.length(); i++)
    if (((uint8_t)s[i] & 0xC0) != 0x80) cols++;
  String out = s;
  while (cols++ < width) out += ' ';
  return out;
}

std::vector<std::pair<String, bool>> settingsNames() {
  std::vector<std::pair<String, bool>> names;
  for (const auto& p : PARAMS) names.push_back({p.name, p.type == PType::Bool});
  return names;
}

String settingsTable(const String& only) {
  Lock lk(g_lock);
  String out;
  for (const auto& p : PARAMS) {
    if (only.length() && only != p.name) continue;
    String value = formatValue(p);
    if (*p.unit) value += String(" ") + p.unit;
    out += "  " + pad(p.name, 17) + pad(value, 10) + p.help + "\n";
  }
  return out;
}

// Reading a missing key makes the Arduino core log an error, so check first.
static String getText(const char* key) { return s_prefs.isKey(key) ? s_prefs.getString(key) : String(); }

static String sha256Hex(const String& s) {
  uint8_t digest[32];
  mbedtls_sha256((const unsigned char*)s.c_str(), s.length(), digest, 0);
  char hex[65];
  for (int i = 0; i < 32; i++) sprintf(hex + 2 * i, "%02x", digest[i]);
  return hex;
}

bool settingsHasPassword() {
  Lock lk(g_lock);
  return s_prefs.isKey("pw_hash");
}

// There is no default password: until one is set on the USB console, password login
// is impossible.
bool settingsCheckPassword(const char* password) {
  Lock lk(g_lock);
  if (!s_prefs.isKey("pw_hash")) return false;
  String expected = s_prefs.getString("pw_hash");
  String actual = sha256Hex(getText("pw_salt") + password);
  uint8_t diff = expected.length() != actual.length();
  for (size_t i = 0; i < expected.length() && i < actual.length(); i++) diff |= expected[i] ^ actual[i];
  return diff == 0;
}

void settingsSetPassword(const String& password) {
  Lock lk(g_lock);
  char salt[17];
  snprintf(salt, sizeof salt, "%08lx%08lx", (unsigned long)esp_random(), (unsigned long)esp_random());
  s_prefs.putString("pw_salt", salt);
  s_prefs.putString("pw_hash", sha256Hex(String(salt) + password));
}

String settingsLoadHostKey() {
  Lock lk(g_lock);
  return getText("hostkey");
}

void settingsSaveHostKey(const char* base64) {
  Lock lk(g_lock);
  s_prefs.putString("hostkey", base64);
}

bool settingsLoadWifi(String& ssid, String& password) {
  Lock lk(g_lock);
  ssid = getText("wifi_ssid");
  password = getText("wifi_pass");
  return ssid.length() > 0;
}

void settingsSaveWifi(const String& ssid, const String& password) {
  Lock lk(g_lock);
  s_prefs.putString("wifi_ssid", ssid);
  s_prefs.putString("wifi_pass", password);
}

String settingsAcCode(bool on) {
  Lock lk(g_lock);
  return getText(on ? "ac_on_code" : "ac_off_code");
}

void settingsSetAcCode(bool on, const String& name) {
  Lock lk(g_lock);
  s_prefs.putString(on ? "ac_on_code" : "ac_off_code", name);
}

bool settingsAcCodeSaved(bool on) {
  Lock lk(g_lock);
  return s_prefs.isKey(on ? "ac_on_code" : "ac_off_code");
}

void settingsClearWifi() {
  Lock lk(g_lock);
  if (s_prefs.isKey("wifi_ssid")) s_prefs.remove("wifi_ssid");
  if (s_prefs.isKey("wifi_pass")) s_prefs.remove("wifi_pass");
}
