#include "ir.h"
#include "config.h"
#include "control.h"
#include "settings.h"
#include "state.h"
#include <IRac.h>
#include <IRrecv.h>
#include <IRremoteESP8266.h>
#include <IRsend.h>
#include <IRutils.h>
#include <LittleFS.h>
#include <algorithm>
#include <atomic>

static IRsend s_irsend(PIN_IR_SEND);
static IRrecv s_irrecv(PIN_IR_RECV, IR_CAPTURE_BUFFER, IR_CAPTURE_TIMEOUT_MS, true);
static decode_results s_results;
static std::atomic<uint32_t> s_lastSendMs{0};

// Handshake between a learning shell task and the main loop, which owns the receiver.
static std::mutex s_learnMutex;
static bool s_learnActive = false;
static bool s_learnDone = false;
static RawCode s_learnCode;

// File: header, timings, then (version 2) the description text.
static const uint32_t FILE_MAGIC_V1 = 0x31435249;  // "IRC1": timings only
static const uint32_t FILE_MAGIC_V2 = 0x32435249;  // "IRC2": with power flag and description
struct __attribute__((packed)) FileHeader {
  uint32_t magic;
  uint16_t freqKhz;
  uint16_t count;
};
struct __attribute__((packed)) FileExtra {
  int8_t power;
  uint8_t descLen;
};

static String pathFor(const String& name) { return "/ir/" + name + ".bin"; }

static void enrich(RawCode& code);

void irBegin() {
  if (!LittleFS.begin(true)) logEvent("LittleFS mount failed, infrared codes cannot be stored");
  if (!LittleFS.exists("/ir")) LittleFS.mkdir("/ir");
  // Older firmware used the fixed names ac_on and ac_off; assign them once.
  for (bool on : {true, false}) {
    const char* old = on ? "ac_on" : "ac_off";
    if (!settingsAcCodeSaved(on) && irStoreExists(old)) {
      settingsSetAcCode(on, old);
      logEvent("air conditioner %s code: '%s'", on ? "on" : "off", old);
    }
  }
  s_irsend.begin();
  s_irrecv.setUnknownThreshold(12);  // ignore short noise bursts
  s_irrecv.enableIRIn();
}

bool irValidName(const String& name) {
  if (name.length() == 0 || name.length() > IR_MAX_NAME) return false;
  for (size_t i = 0; i < name.length(); i++) {
    char c = name[i];
    if (!isalnum((unsigned char)c) && c != '_' && c != '-') return false;
  }
  return true;
}

bool irStoreSave(const String& name, const RawCode& code) {
  File f = LittleFS.open(pathFor(name), "w");
  if (!f) return false;
  FileHeader h{FILE_MAGIC_V2, code.freqKhz, (uint16_t)code.timings.size()};
  String desc = code.description.substring(0, 255);
  FileExtra x{code.power, (uint8_t)desc.length()};
  bool ok = f.write((const uint8_t*)&h, sizeof h) == sizeof h;
  ok = ok && f.write((const uint8_t*)&x, sizeof x) == sizeof x;
  size_t bytes = code.timings.size() * sizeof(uint16_t);
  ok = ok && f.write((const uint8_t*)code.timings.data(), bytes) == bytes;
  ok = ok && f.write((const uint8_t*)desc.c_str(), desc.length()) == desc.length();
  f.close();
  return ok;
}

bool irStoreLoad(const String& name, RawCode& code) {
  if (!irValidName(name) || !LittleFS.exists(pathFor(name))) return false;  // open() would log an error
  File f = LittleFS.open(pathFor(name), "r");
  if (!f) return false;
  FileHeader h;
  FileExtra x{-1, 0};
  bool ok = f.read((uint8_t*)&h, sizeof h) == sizeof h && (h.magic == FILE_MAGIC_V1 || h.magic == FILE_MAGIC_V2) &&
            h.count > 0 && h.count <= IR_CAPTURE_BUFFER;
  if (ok && h.magic == FILE_MAGIC_V2) ok = f.read((uint8_t*)&x, sizeof x) == sizeof x;
  if (ok) {
    code.freqKhz = h.freqKhz;
    code.power = x.power;
    code.timings.resize(h.count);
    size_t bytes = h.count * sizeof(uint16_t);
    ok = f.read((uint8_t*)code.timings.data(), bytes) == bytes;
  }
  code.description = h.magic == FILE_MAGIC_V1 ? "recorded by older firmware, record again to see what it does" : "";
  if (ok && x.descLen) {
    char buf[256];
    ok = f.read((uint8_t*)buf, x.descLen) == x.descLen;
    buf[ok ? x.descLen : 0] = 0;
    code.description = buf;
  }
  f.close();
  if (ok) enrich(code);
  return ok;
}

bool irStoreExists(const String& name) { return irValidName(name) && LittleFS.exists(pathFor(name)); }

bool irStoreDelete(const String& name) { return irStoreExists(name) && LittleFS.remove(pathFor(name)); }

bool irStoreRename(const String& from, const String& to) {
  if (!irStoreExists(from) || !irValidName(to)) return false;
  if (irStoreExists(to)) LittleFS.remove(pathFor(to));
  return LittleFS.rename(pathFor(from), pathFor(to));
}

std::vector<String> irStoreList() {
  std::vector<String> names;
  File dir = LittleFS.open("/ir");
  if (!dir || !dir.isDirectory()) return names;
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    String n = f.name();
    int slash = n.lastIndexOf('/');
    if (slash >= 0) n = n.substring(slash + 1);
    if (n.endsWith(".bin")) names.push_back(n.substring(0, n.length() - 4));
  }
  std::sort(names.begin(), names.end(), [](const String& a, const String& b) { return a < b; });
  return names;
}

void irSendRaw(const RawCode& code) {
  Lock lk(g_lock);
  s_irsend.sendRaw(code.timings.data(), code.timings.size(), code.freqKhz);
  s_lastSendMs = millis();
}

bool irSendCode(const String& name, String& err) {
  RawCode code;
  if (!irStoreLoad(name, code)) {
    err = "no infrared code named '" + name + "' (record it with: ir learn " + name + ")";
    return false;
  }
  irSendRaw(code);
  return true;
}

bool irLearn(uint32_t timeoutMs, RawCode& code) {
  {
    std::lock_guard<std::mutex> lk(s_learnMutex);
    s_learnActive = true;
    s_learnDone = false;
  }
  bool got = false;
  uint32_t start = millis();
  while (!got && millis() - start < timeoutMs) {
    vTaskDelay(pdMS_TO_TICKS(50));
    std::lock_guard<std::mutex> lk(s_learnMutex);
    if (s_learnDone) {
      code = s_learnCode;
      got = true;
    }
  }
  std::lock_guard<std::mutex> lk(s_learnMutex);
  s_learnActive = false;
  return got;
}

// Protocol name, then everything the library decodes for air conditioners it knows
// (power, mode, temperature, fan, swing, ...), otherwise the raw value. The value
// form "NAME 0x... (N bits)" is also what lets enrich() decode a stored code again.
static String describe(const decode_results& r) {
  String d = typeToString(r.decode_type, r.repeat);
  if (r.decode_type != decode_type_t::UNKNOWN && !hasACState(r.decode_type))
    d += " 0x" + uint64ToString(r.value, 16) + " (" + String(r.bits) + " bits)";
  if (IRac::isProtocolSupported(r.decode_type)) {
    String ac = IRAcUtils::resultAcToString(&r);
    if (ac.length()) d += ": " + ac;
  }
  if (r.overflow) d += " [warning: capture buffer overflowed]";
  return d;
}

// One field of a decoded description, e.g. "Temp" in "..., Temp: 17C, ..." gives
// "17C"; for "Mode: 0 (Cool)" the name in brackets is returned.
static String decodedField(const String& d, const char* name) {
  String key = String(", ") + name + ": ";
  int i = d.indexOf(key);
  if (i < 0) {
    key = String(": ") + name + ": ";  // first field, right after the protocol name
    i = d.indexOf(key);
  }
  if (i < 0) return "";
  int start = i + key.length(), end = d.indexOf(", ", start);
  String v = end < 0 ? d.substring(start) : d.substring(start, end);
  int open = v.indexOf(" ("), close = v.lastIndexOf(')');
  return open >= 0 && close > open ? v.substring(open + 2, close) : v;
}

String irSummary(const String& d) {
  if (!d.length()) return "not decoded";
  int sp = d.indexOf(' '), colon = d.indexOf(':');
  int cut = sp < 0 ? colon : colon < 0 ? sp : min(sp, colon);
  String protocol = cut > 0 ? d.substring(0, cut) : d;
  String power = decodedField(d, "Power"), mode = decodedField(d, "Mode"), temp = decodedField(d, "Temp"),
         fan = decodedField(d, "Fan");
  if (!power.length() && !mode.length() && !temp.length()) return d.length() > 70 ? d.substring(0, 67) + "..." : d;
  String o = protocol;
  if (power.length()) o += " · " + power;
  if (mode.length()) o += " · " + mode;
  if (temp.length()) o += " · " + temp;
  if (fan.length()) o += " · fan " + fan;
  return o;
}

// Codes saved before the full decode was shown carry only "NAME 0x... (N bits)".
// For protocols that send a single value that is enough to decode them again.
static void enrich(RawCode& code) {
  const String& d = code.description;
  int sp = d.indexOf(" 0x"), paren = d.indexOf(" (", sp), bitsEnd = d.indexOf(" bits)", paren);
  if (sp <= 0 || paren < 0 || bitsEnd < 0 || d.indexOf(": ") >= 0) return;
  decode_results r;
  memset(&r, 0, sizeof r);
  r.decode_type = strToDecodeType(d.substring(0, sp).c_str());
  if (r.decode_type == decode_type_t::UNKNOWN || hasACState(r.decode_type) || !IRac::isProtocolSupported(r.decode_type)) return;
  r.value = strtoull(d.substring(sp + 3, paren).c_str(), nullptr, 16);
  r.bits = d.substring(paren + 2, bitsEnd).toInt();
  code.description = describe(r);
  stdAc::state_t state;
  if (code.power < 0 && IRAcUtils::decodeToState(&r, &state)) code.power = state.power ? 1 : 0;
}

static bool rawMatches(const std::vector<uint16_t>& a, const std::vector<uint16_t>& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); i++) {
    uint32_t x = a[i], y = b[i];
    uint32_t tolerance = std::max<uint32_t>(200, std::max(x, y) / 4);
    if ((x > y ? x - y : y - x) > tolerance) return false;
  }
  return true;
}

// Works out whether a received code turns the air conditioner on (1), off (0) or
// neither (-1): from the decoded protocol when the library understands it, otherwise
// by comparing with the codes assigned to the air conditioner.
// Power flag carried in the message itself, for remotes the library can decode.
static int decodedPower(const decode_results& r) {
  stdAc::state_t state;
  if (IRac::isProtocolSupported(r.decode_type) && IRAcUtils::decodeToState(&r, &state))
    return state.power ? 1 : 0;
  return -1;
}

static int remotePower(const decode_results& r, const RawCode& code) {
  if (code.power >= 0) return code.power;
  RawCode stored;
  if (irStoreLoad(settingsAcCode(true), stored) && rawMatches(stored.timings, code.timings)) return 1;
  if (irStoreLoad(settingsAcCode(false), stored) && rawMatches(stored.timings, code.timings)) return 0;
  return -1;
}

void irPoll() {
  static uint32_t lastRxMs = 0;
  if (!s_irrecv.decode(&s_results)) return;
  uint32_t now = millis();
  if (now - s_lastSendMs < IR_ECHO_IGNORE_MS) {
    s_irrecv.resume();
    return;
  }
  // Remotes repeat their message, and repeats can decode differently: one press is a
  // burst of messages, and only its first message counts.
  bool repeat = lastRxMs != 0 && now - lastRxMs < IR_BURST_GAP_MS;
  lastRxMs = now;
  if (repeat) {
    s_irrecv.resume();
    return;
  }
  RawCode code;
  uint16_t* raw = resultToRawArray(&s_results);
  code.timings.assign(raw, raw + getCorrectedRawLength(&s_results));
  delete[] raw;
  String desc = describe(s_results);
  code.description = desc;
  code.power = decodedPower(s_results);
  {
    std::lock_guard<std::mutex> lk(s_learnMutex);
    if (s_learnActive) {
      if (!s_learnDone) {
        s_learnCode = code;
        s_learnDone = true;
      }
      s_irrecv.resume();
      return;
    }
  }
  int power = remotePower(s_results, code);
  s_irrecv.resume();
  {
    Lock lk(g_lock);
    g_status.lastIr = desc;
    g_status.lastIrMs = millis();
  }
  if (power >= 0) onRemoteAc(power == 1, irSummary(desc));
}
