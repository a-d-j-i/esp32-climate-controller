#include "auth_keys.h"
#include "commands.h"
#include "config.h"
#include "control.h"
#include "hw_task.h"
#include "ir.h"
#include "network.h"
#include "settings.h"
#include "ssh_shell.h"
#include "state.h"
#include <LittleFS.h>
#include <esp_timer.h>
#include <vector>

using Args = std::vector<String>;

static const char* HELP =
    "Tab completes commands, settings and code names; Tab twice lists the choices.\n"
    "Status and settings\n"
    "  status [json]                  readings, outputs and mode\n"
    "  get [name]                     parameters and their values\n"
    "  set <name> <value> [...]       change parameters (pairs are applied together)\n"
    "  mode auto|manual               automatic rules or manual control\n"
    "  log [count]                    recent events\n"
    "  watch [seconds|off]            one-line status every few seconds (saved)\n"
    "  sensor test [cabinet|room|box] one reading, step by step, to find wiring problems\n"
    "Air conditioner\n"
    "  ac learn [seconds]             record its on and off codes: press the remote's power\n"
    "                                 button, then again\n"
    "  ac codes                       which codes turn it on and off\n"
    "  ac use on|off <code>           use another stored code for on or off\n"
    "  ac on|off                      switch it now (switches to manual; on also stops the fan)\n"
    "  ac force on|off [minutes]      as if the range was crossed: hold until the temperature\n"
    "                                 reaches the other end of the range (automatic mode)\n"
    "  ac auto                        back to the automatic rules (ends a force, mode auto)\n"
    "Fan\n"
    "  fan on|off                     switch the relay now (switches to manual; refused while\n"
    "                                 the air conditioner is on)\n"
    "  fan force on|off [minutes]     like ac force\n"
    "  fan auto                       back to the automatic rules (ends a force, mode auto)\n"
    "Other infrared codes (extra buttons, other devices)\n"
    "  ir list                        stored codes and what they do (ir show: full decode)\n"
    "  ir learn <name> [seconds]      record one button\n"
    "  ir send <name>                 transmit a stored code\n"
    "  ir show <name>                 decoded message and timings\n"
    "  ir rename <name> <new name>    rename a code\n"
    "  ir import <name> [-f kHz] <timings>  store timings recorded with another tool\n"
    "  ir delete <name>               remove a code\n"
    "Access\n"
    "  wifi                           Wi-Fi status\n"
    "  wifi set \"<name>\" \"<password>\" store Wi-Fi credentials (USB console only)\n"
    "  wifi clear                     forget Wi-Fi credentials (USB console only)\n"
    "  key list                       public keys allowed to log in\n"
    "  key add <public key line>      authorize a key (contents of ~/.ssh/id_ed25519.pub)\n"
    "  key delete <number>            remove a key\n"
    "  passwd <new password>          change the Secure Shell password\n"
    "  hostkey                        this device's Secure Shell host key fingerprint\n"
    "  reboot                         restart the device\n"
    "  exit                           close the session\n";

static String fmt(const char* f, ...) __attribute__((format(printf, 1, 2)));
static String fmt(const char* f, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, f);
  vsnprintf(buf, sizeof buf, f, ap);
  va_end(ap);
  return buf;
}

// Splits on whitespace; double quotes group words ("My Network").
static Args tokenize(const String& line) {
  Args out;
  String cur;
  bool quoted = false, any = false;
  for (size_t i = 0; i < line.length(); i++) {
    char c = line[i];
    if (c == '"') {
      quoted = !quoted;
      any = true;
    } else if (!quoted && (c == ' ' || c == '\t')) {
      if (any) out.push_back(cur);
      cur = "";
      any = false;
    } else {
      cur += c;
      any = true;
    }
  }
  if (any) out.push_back(cur);
  return out;
}

static String duration(uint64_t s) {
  if (s < 120) return fmt("%llu s", s);
  if (s < 7200) return fmt("%llu min", s / 60);
  if (s < 172800) return fmt("%llu h %llu min", s / 3600, (s / 60) % 60);
  return fmt("%llu days %llu h", s / 86400, (s / 3600) % 24);
}

static String ago(uint32_t sinceMs) { return duration((millis() - sinceMs) / 1000) + " ago"; }

static CmdResult usage(const Out& out, const char* text) {
  out(String("usage: ") + text + "\n");
  return CmdResult::Error;
}

static String sensorLine(const char* label, const SensorInfo& i) {
  String o = fmt("%-18s", label) + sensorSummary(i);
  if (i.reading.lastReadMs) o += ", last reading " + ago(i.reading.lastReadMs);
  if (i.reading.readErrors)
    o += fmt(", %lu of %lu reads failed", (unsigned long)i.reading.readErrors, (unsigned long)i.reading.attempts);
  return o + "\n";
}

static const char* stateName(SensorState s) {
  switch (s) {
    case SensorState::Starting: return "starting";
    case SensorState::Ok: return "ok";
    case SensorState::Failing: return "failing";
  }
  return "?";
}

static String jsonNumber(float v) { return isnan(v) ? "null" : String(v, 1); }

static const char* powerText(int8_t power) { return power == 1 ? "turns on" : power == 0 ? "turns off" : ""; }

// "(air conditioner on)" when the code is assigned to switch the air conditioner.
static String assignedText(const String& name) {
  bool on = settingsAcCode(true) == name, off = settingsAcCode(false) == name;
  if (on && off) return "(air conditioner on and off)";
  return on ? "(air conditioner on)" : off ? "(air conditioner off)" : "";
}

static CmdResult cmdStatus(const Args& a, const Out& out) {
  Settings s;
  Status st;
  SensorInfo in = controlSensorInfo(SensorId::Cabinet);
  SensorInfo roomInfo = controlSensorInfo(SensorId::Room);
  SensorInfo boxInfo = controlSensorInfo(SensorId::Box);
  bool fresh = in.fresh;
  bool roomFresh = roomInfo.fresh;
  bool boxFresh = boxInfo.fresh;
  {
    Lock lk(g_lock);
    s = g_settings;
    st = g_status;
  }
  const char* ac = !st.acKnown ? "unknown" : st.acOn ? "on" : "off";
  const char* mode = s.mode == Mode::Auto ? "auto" : "manual";
  if (a.size() > 1 && a[1] == "json") {
    auto quoted = [](const String& v) {
      String q = "\"";
      for (size_t i = 0; i < v.length(); i++) {
        if (v[i] == '"' || v[i] == '\\') q += '\\';
        q += v[i];
      }
      return q + "\"";
    };
    String o = "{\"cabinet_temperature\":" + jsonNumber(fresh ? st.cabinet.temperature : NAN) +
               ",\"cabinet_humidity\":" + jsonNumber(fresh ? st.cabinet.humidity : NAN) + ",\"cabinet_sensor_ok\":" + (fresh ? "true" : "false") +
               ",\"cabinet_sensor_state\":\"" + stateName(in.state) + "\",\"cabinet_sensor_problem\":" + quoted(in.problem);
    o += ",\"room_temperature\":" + jsonNumber(roomFresh ? st.room.temperature : NAN) +
         ",\"room_humidity\":" + jsonNumber(roomFresh ? st.room.humidity : NAN) +
         ",\"room_sensor_ok\":" + (roomFresh ? "true" : "false") +
         ",\"room_sensor_state\":\"" + stateName(roomInfo.state) + "\",\"room_sensor_problem\":" + quoted(roomInfo.problem);
    o += ",\"box_temperature\":" + jsonNumber(boxFresh ? st.box.temperature : NAN) +
         ",\"box_humidity\":" + jsonNumber(boxFresh ? st.box.humidity : NAN) +
         ",\"box_sensor_ok\":" + (boxFresh ? "true" : "false") +
         ",\"box_sensor_state\":\"" + stateName(boxInfo.state) + "\",\"box_sensor_problem\":" + quoted(boxInfo.problem);
    o += String(",\"ac\":\"") + ac + "\",\"fan\":\"" + (st.fanOn ? "on" : "off") + "\",\"mode\":\"" + mode + "\"}\n";
    out(o);
    return CmdResult::Ok;
  }
  String o;
  auto climate = [&](const char* label, const Reading& r, bool isFresh) {
    if (r.lastReadMs)
      o += fmt("%-18s%.1f °C, humidity %.0f %%", label, r.temperature, r.humidity) +
           (isFresh ? String("\n") : "  (old: last reading " + ago(r.lastReadMs) + ")\n");
  };
  climate("Cabinet", st.cabinet, fresh);
  if (roomFresh) climate("Room", st.room, true);
  if (boxFresh) climate("Box", st.box, true);
  o += sensorLine("Cabinet sensor", in);
  o += sensorLine("Room sensor", roomInfo);
  o += sensorLine("Box sensor", boxInfo);
  if (!fresh) o += "                  automatic control paused: no recent cabinet reading\n";
  auto forceText = [&](const Force& f, float onAbove, float offBelow) {
    if (!f.active) return String();
    String t = fmt(", forced until %s %.1f °C", f.on ? "falling to" : "rising to", f.on ? offBelow : onAbove);
    if (f.untilMs) t += fmt(" or %ld min", (long)((int32_t)(f.untilMs - millis()) / 60000) + 1);
    return t;
  };
  o += String("Air conditioner   ") + ac + forceText(st.acForce, s.acOnAbove, s.acOffBelow);
  {
    String onCode = settingsAcCode(true), offCode = settingsAcCode(false);
    o += "\n                  codes: on " + (onCode.length() ? "'" + onCode + "'" : String("not recorded")) +
         ", off " + (offCode.length() ? "'" + offCode + "'" : String("not recorded")) +
         (onCode.length() && offCode.length() ? "" : " (run: ac learn)");
  }
  if (st.acKnown) o += " (as last sent or seen, " + ago(st.acChangedMs) + ")";
  o += String("\nFan               ") + (st.fanOn ? "on (" + duration((millis() - st.fanOnSinceMs) / 1000) + ")" : String("off")) +
       forceText(st.fanForce, s.fanOnAbove, s.fanOffBelow) + "\n";
  o += String("Mode              ") + mode + "\n";
  o += fmt("Rules             fan on at %.1f °C or more, off at %.1f °C or less\n", s.fanOnAbove, s.fanOffBelow);
  if (s.fanMinOnMin) o += fmt("                  once started, the fan runs at least %lu min\n", (unsigned long)s.fanMinOnMin);
  if (s.fanUseRoom)
    o += fmt("                  (only when the room is at least %.1f °C cooler to start, more than %.1f °C\n"
             "                  to keep running, or unknown)\n", s.fanRoomMargin, s.fanRoomStop);
  o += fmt("                  air conditioner on at %.1f °C or more, off at %.1f °C or less\n",
           s.acOnAbove, s.acOffBelow);
  o += "                  the fan is off while the air conditioner is on\n";
  if (st.lastIrMs) o += "Last infrared     " + st.lastIr + " (" + ago(st.lastIrMs) + ")\n";
  o += "Wi-Fi             " + networkSummary() + "\n";
  o += String("Last restart      ") + resetReasonText() + "\n";
  o += "Uptime            " + duration(esp_timer_get_time() / 1000000) +
       fmt(", free memory %lu kB (lowest %lu kB)\n", (unsigned long)(ESP.getFreeHeap() / 1024),
           (unsigned long)(ESP.getMinFreeHeap() / 1024));
  out(o);
  return CmdResult::Ok;
}

static CmdResult cmdSet(const Args& a, const Out& out) {
  if (a.size() < 3 || a.size() % 2 == 0) return usage(out, "set <name> <value> [<name> <value> ...]");
  std::vector<std::pair<String, String>> pairs;
  for (size_t i = 1; i + 1 < a.size(); i += 2) pairs.push_back({a[i], a[i + 1]});
  String err;
  bool reboot = false;
  if (!settingsSet(pairs, err, reboot)) {
    out("error: " + err + "\n");
    return CmdResult::Error;
  }
  for (const auto& kv : pairs)
    if (kv.first == "wifi_power") networkApplyTxPower();
  out(reboot ? "saved; reboot to apply\n" : "saved\n");
  return CmdResult::Ok;
}

static CmdResult cmdMode(const Args& a, const Out& out) {
  if (a.size() != 2 || (a[1] != "auto" && a[1] != "manual")) return usage(out, "mode auto|manual");
  if (a[1] == "manual") controlClearForces();
  {
    Lock lk(g_lock);
    g_settings.mode = a[1] == "auto" ? Mode::Auto : Mode::Manual;
    settingsSaveMode();
  }
  logEvent("mode %s (command)", a[1].c_str());
  out("mode " + a[1] + "\n");
  return CmdResult::Ok;
}

static void enterManual(const Out& out) {
  Lock lk(g_lock);
  if (g_settings.mode == Mode::Manual) return;
  controlClearForces();
  g_settings.mode = Mode::Manual;
  settingsSaveMode();
  logEvent("mode manual (manual command)");
  out("switched to manual mode (mode auto to resume)\n");
}

// ac|fan on|off           manual control (switches to manual mode)
// ac|fan force on|off [m]  hold the state in automatic mode until the range end point
// ac|fan auto              back to the rules: end a force, leave manual mode
static CmdResult cmdAcCodes(const Out& out) {
  for (bool on : {true, false}) {
    String name = settingsAcCode(on);
    RawCode c;
    String line = fmt("  %-4s ", on ? "on:" : "off:");
    if (!name.length()) line += "none (run: ac learn)";
    else if (!irStoreLoad(name, c)) line += name + "  (missing: run ac learn)";
    else line += fmt("%-14s ", name.c_str()) + irSummary(c.description);
    out(line + "\n");
  }
  return CmdResult::Ok;
}

static CmdResult cmdAcUse(const Args& a, const Out& out) {
  if (a.size() != 4 || (a[2] != "on" && a[2] != "off")) return usage(out, "ac use on|off <code>");
  bool on = a[2] == "on";
  RawCode c;
  if (!irStoreLoad(a[3], c)) {
    out("error: no code named '" + a[3] + "' (see: ir list)\n");
    return CmdResult::Error;
  }
  settingsSetAcCode(on, a[3]);
  out("air conditioner " + a[2] + ": '" + a[3] + "'\n");
  if (c.power >= 0 && (c.power == 1) != on)
    out(String("warning: this code ") + powerText(c.power) + " the air conditioner\n");
  logEvent("air conditioner %s code: '%s'", on ? "on" : "off", a[3].c_str());
  return CmdResult::Ok;
}

// Records ac_on and ac_off from a remote whose power button toggles: every press
// carries the decoded power state, so keep the first "on" and the first "off" seen.
static CmdResult cmdAcLearn(const Args& a, const Out& out) {
  long seconds = a.size() == 3 ? a[2].toInt() : 30;
  if (a.size() > 3 || seconds < 5 || seconds > 120) return usage(out, "ac learn [seconds 5-120]");
  out(fmt("Point the remote at the receiver and press its power button, then again, until both\n"
          "on and off are recorded (%ld s). The air conditioner will follow along.\n"
          "The 'on' message carries the mode, temperature and fan shown on the remote.\n", seconds));
  RawCode on, off;
  bool haveOn = false, haveOff = false;
  int unknown = 0;
  uint32_t deadline = millis() + seconds * 1000UL;
  while (!(haveOn && haveOff)) {
    int32_t left = (int32_t)(deadline - millis());
    RawCode code;
    if (left <= 0 || !irLearn(left, code)) break;
    if (code.power == 1 && !haveOn) {
      on = code;
      haveOn = true;
      out("  on:  " + code.description + "\n");
    } else if (code.power == 0 && !haveOff) {
      off = code;
      haveOff = true;
      out("  off: " + code.description + "\n");
    } else if (code.power < 0) {
      unknown++;
      out("  not an on/off message: " + code.description + "\n");
    }
    if (!(haveOn && haveOff)) out(haveOn ? "  now press it so it turns off...\n" : haveOff ? "  now press it so it turns on...\n" : "");
  }
  if (!haveOn && !haveOff) {
    out(unknown ? "This remote's messages can't be decoded, so on and off can't be told apart.\n"
                  "Record them by name instead: ir learn ac_on, then ir learn ac_off.\n"
                : "nothing received\n");
    return CmdResult::Error;
  }
  if ((haveOn && !irStoreSave("ac_on", on)) || (haveOff && !irStoreSave("ac_off", off))) {
    out("error: could not write to flash\n");
    return CmdResult::Error;
  }
  if (haveOn) settingsSetAcCode(true, "ac_on");
  if (haveOff) settingsSetAcCode(false, "ac_off");
  out(String("saved and in use:") + (haveOn ? " ac_on (on)" : "") + (haveOff ? " ac_off (off)" : "") + "\n");
  logEvent("infrared codes recorded:%s%s", haveOn ? " ac_on" : "", haveOff ? " ac_off" : "");
  if (!(haveOn && haveOff)) {
    out(String("still missing the ") + (haveOn ? "off" : "on") + " code: run ac learn again\n");
    return CmdResult::Error;
  }
  return CmdResult::Ok;
}

static CmdResult cmdOutput(const Args& a, const Out& out, bool ac) {
  const char* name = ac ? "air conditioner" : "fan";
  const char* use = ac ? "ac learn [seconds] | ac codes | ac use on|off <code> | ac on|off | ac force on|off [minutes] | ac auto"
                       : "fan on|off | fan force on|off [minutes] | fan auto";
  String err;
  bool ok;
  if (a.size() == 2 && (a[1] == "on" || a[1] == "off")) {
    bool on = a[1] == "on";
    enterManual(out);
    hwRun([&] { ok = ac ? setAc(on, "command", &err) : setFan(on, "command", &err); });
    if (!ok) {
      out("error: " + err + "\n");
      return CmdResult::Error;
    }
    out(String(name) + " " + a[1] + "\n");
    return CmdResult::Ok;
  }
  if ((a.size() == 3 || a.size() == 4) && a[1] == "force" && (a[2] == "on" || a[2] == "off")) {
    bool on = a[2] == "on";
    long minutes = a.size() == 4 ? a[3].toInt() : 0;
    if (a.size() == 4 && (minutes < 1 || minutes > 1440)) return usage(out, use);
    hwRun([&] { ok = ac ? forceAc(on, minutes, &err) : forceFan(on, minutes, &err); });
    if (!ok) {
      out("error: " + err + "\n");
      return CmdResult::Error;
    }
    float end;
    {
      Lock lk(g_lock);
      end = ac ? (on ? g_settings.acOffBelow : g_settings.acOnAbove)
               : (on ? g_settings.fanOffBelow : g_settings.fanOnAbove);
    }
    String o = fmt("%s forced %s until the temperature %s %.1f °C", name, on ? "on" : "off",
                   on ? "falls to" : "rises to", end);
    if (minutes) o += fmt(" or %ld min pass", minutes);
    out(o + " (automatic mode)\n");
    return CmdResult::Ok;
  }
  if (a.size() == 2 && a[1] == "auto") {
    // Hand the output back to the rules: end its force, and leave manual mode, which
    // applies to the whole controller.
    releaseForce(ac);
    bool wasManual;
    {
      Lock lk(g_lock);
      wasManual = g_settings.mode == Mode::Manual;
      if (wasManual) {
        g_settings.mode = Mode::Auto;
        settingsSaveMode();
      }
    }
    if (wasManual) logEvent("mode auto (%s auto)", ac ? "ac" : "fan");
    out(String(name) + ": automatic rules" + (wasManual ? " (mode auto: the " + String(ac ? "fan" : "air conditioner") +
        " follows the rules too)\n" : "\n"));
    return CmdResult::Ok;
  }
  return usage(out, use);
}

static bool checkName(const String& name, const Out& out) {
  if (irValidName(name)) return true;
  out(fmt("error: names use letters, digits, '_' or '-', at most %d characters\n", IR_MAX_NAME));
  return false;
}

static CmdResult cmdIr(const Args& a, const Out& out) {
  String sub = a.size() > 1 ? a[1] : "";
  if (sub == "list") {
    auto names = irStoreList();
    if (names.empty()) out("no codes stored (record one with: ir learn <name>)\n");
    for (const auto& n : names) {
      RawCode c;
      if (!irStoreLoad(n, c)) {
        out(fmt("  %-14s (unreadable)\n", n.c_str()));
        continue;
      }
      String what = irSummary(c.description);
      out(fmt("  %-14s %-10s %s", n.c_str(), powerText(c.power), what.c_str()));
      String used = assignedText(n);
      out(used.length() ? "\n  " + String("               ") + used + "\n" : String("\n"));
    }
    out(fmt("storage: %u of %u kB used\n", (unsigned)(LittleFS.usedBytes() / 1024), (unsigned)(LittleFS.totalBytes() / 1024)));
    return CmdResult::Ok;
  }
  if (sub == "learn") {
    if (a.size() < 3 || a.size() > 4) return usage(out, "ir learn <name> [seconds]");
    if (!checkName(a[2], out)) return CmdResult::Error;
    long seconds = a.size() == 4 ? a[3].toInt() : 10;
    if (seconds < 1 || seconds > 60) return usage(out, "ir learn <name> [seconds 1-60]");
    out(fmt("Point the remote at the receiver and press the button (%ld s)...\n", seconds));
    RawCode code;
    if (!irLearn(seconds * 1000, code)) {
      out("nothing received\n");
      return CmdResult::Error;
    }
    bool replaced = irStoreExists(a[2]);
    if (!irStoreSave(a[2], code)) {
      out("error: could not write to flash\n");
      return CmdResult::Error;
    }
    out(fmt("received %u timings: ", (unsigned)code.timings.size()) + code.description + "\n");
    out((replaced ? "replaced '" : "saved '") + a[2] + "'\n");
    if (code.power >= 0 && settingsAcCode(code.power == 1) != a[2])
      out(String("this code ") + powerText(code.power) + " the air conditioner; to use it for that: ac use " +
          (code.power == 1 ? "on " : "off ") + a[2] + "\n");
    logEvent("infrared code '%s' recorded", a[2].c_str());
    return CmdResult::Ok;
  }
  if (sub == "send") {
    if (a.size() != 3) return usage(out, "ir send <name>");
    String err;
    bool ok;
    hwRun([&] { ok = irSendCode(a[2], err); });
    if (!ok) {
      out("error: " + err + "\n");
      return CmdResult::Error;
    }
    out("sent '" + a[2] + "'\n");
    return CmdResult::Ok;
  }
  if (sub == "show") {
    if (a.size() != 3) return usage(out, "ir show <name>");
    RawCode c;
    if (!irStoreLoad(a[2], c)) {
      out("error: no code named '" + a[2] + "'\n");
      return CmdResult::Error;
    }
    String o;
    if (c.description.length()) o += c.description + "\n";
    if (c.power >= 0) o += String(powerText(c.power)) + " the air conditioner\n";
    o += fmt("%u kHz, %u timings (microseconds, mark first):\n", c.freqKhz, (unsigned)c.timings.size());
    for (size_t i = 0; i < c.timings.size(); i++) {
      o += String(c.timings[i]);
      o += i + 1 == c.timings.size() ? "\n" : (i % 16 == 15 ? ",\n" : ",");
    }
    out(o);
    return CmdResult::Ok;
  }
  if (sub == "import") {
    if (a.size() < 4) return usage(out, "ir import <name> [-f kHz] <t1,t2,...>");
    if (!checkName(a[2], out)) return CmdResult::Error;
    RawCode code;
    size_t i = 3;
    if (a[i] == "-f" && i + 1 < a.size()) {
      long f = a[i + 1].toInt();
      if (f < 20 || f > 60) return usage(out, "ir import <name> [-f kHz 20-60] <timings>");
      code.freqKhz = f;
      i += 2;
    }
    // Accept separators and braces as printed by common tools; a minus sign marks a
    // space in some dumps (ESPHome) and is dropped.
    String text;
    for (; i < a.size(); i++) text += a[i] + " ";
    for (size_t k = 0; k < text.length(); k++)
      if (strchr(",{};[]", text[k])) text[k] = ' ';
    for (const auto& tok : tokenize(text)) {
      char* end;
      long v = strtol(tok.c_str(), &end, 10);
      if (*end || v == 0 || labs(v) > 65535) {
        out("error: '" + tok + "' is not a timing in microseconds\n");
        return CmdResult::Error;
      }
      code.timings.push_back(labs(v));
    }
    code.description = "imported";
    if (code.timings.size() < 4 || code.timings.size() > IR_CAPTURE_BUFFER) {
      out(fmt("error: expected 4 to %d timings\n", IR_CAPTURE_BUFFER));
      return CmdResult::Error;
    }
    if (!irStoreSave(a[2], code)) {
      out("error: could not write to flash\n");
      return CmdResult::Error;
    }
    out(fmt("imported %u timings as '%s'\n", (unsigned)code.timings.size(), a[2].c_str()));
    return CmdResult::Ok;
  }
  if (sub == "rename") {
    if (a.size() != 4) return usage(out, "ir rename <name> <new name>");
    if (!checkName(a[3], out)) return CmdResult::Error;
    bool replaced = irStoreExists(a[3]);
    if (!irStoreRename(a[2], a[3])) {
      out("error: no code named '" + a[2] + "'\n");
      return CmdResult::Error;
    }
    for (bool on : {true, false})
      if (settingsAcCode(on) == a[2]) settingsSetAcCode(on, a[3]);
    out("renamed '" + a[2] + "' to '" + a[3] + "'" + (replaced ? " (replacing the old one)\n" : "\n"));
    return CmdResult::Ok;
  }
  if (sub == "delete") {
    if (a.size() != 3) return usage(out, "ir delete <name>");
    String used = assignedText(a[2]);
    if (used.length()) {
      out("error: '" + a[2] + "' is in use " + used + "; record or assign another first (ac learn, ac use)\n");
      return CmdResult::Error;
    }
    if (!irStoreDelete(a[2])) {
      out("error: no code named '" + a[2] + "'\n");
      return CmdResult::Error;
    }
    out("deleted '" + a[2] + "'\n");
    return CmdResult::Ok;
  }
  return usage(out, "ir list|learn|rename|send|show|import|delete ...");
}

static CmdResult cmdLog(const Args& a, const Out& out) {
  auto lines = eventLogSnapshot();
  size_t n = a.size() > 1 ? (size_t)max(1L, a[1].toInt()) : 20;
  size_t start = lines.size() > n ? lines.size() - n : 0;
  String o;
  for (size_t i = start; i < lines.size(); i++) o += lines[i] + "\n";
  out(o.length() ? o : "no events\n");
  return CmdResult::Ok;
}

static const char* pinLabel(uint8_t gpio) {
  switch (gpio) {
    case 16: return "RX2";
    case 17: return "TX2";
    case 23: return "D23";
    default: return "";
  }
}

static CmdResult cmdSensorTest(const Args& a, const Out& out) {
  SensorId id = SensorId::Cabinet;
  if (a.size() == 3 && a[2] == "room") id = SensorId::Room;
  else if (a.size() == 3 && a[2] == "box") id = SensorId::Box;
  else if (a.size() > 3 || (a.size() == 3 && a[2] != "cabinet")) return usage(out, "sensor test [cabinet|room|box]");
  uint8_t gpio = controlSensorPin(id);
  String label = pinLabel(gpio);
  out(fmt("%s sensor on GPIO %u%s%s%s:\n", sensorName(id), gpio, label.length() ? " (" : "", label.c_str(), label.length() ? ")" : ""));
  DhtResult r = controlSensorTest(id);
  if (!r.idleHigh) {
    out("  line before the start: LOW. Something holds it low: a short to GND, or data and GND swapped.\n");
    return CmdResult::Error;
  }
  out("  line before the start: high (pulled up, ok)\n");
  if (r.answerUs < 0) {
    out("  no answer within 200 µs of the start signal. Check that the sensor has 3V3 and GND,\n"
        "  that its data wire is on this pin, and that the + and - wires aren't swapped.\n");
    return CmdResult::Error;
  }
  out(fmt("  sensor answered after %ld µs (ok: 20-40 µs is typical)\n", (long)r.answerUs));
  out(fmt("  received %u of 40 bits: %02X %02X %02X %02X %02X", r.bits, r.raw[0], r.raw[1], r.raw[2], r.raw[3], r.raw[4]));
  if (r.bits < 40) {
    out("\n  the answer stopped early: loose wire, long cable without pull-up, or interference\n");
    return CmdResult::Error;
  }
  bool sumOk = ((r.raw[0] + r.raw[1] + r.raw[2] + r.raw[3]) & 0xff) == r.raw[4];
  out(sumOk ? "  (checksum ok)\n" : "  (checksum WRONG: noise on the line)\n");
  if (r.error != DhtError::None) {
    out(String("  result: ") + dhtErrorText(r.error) + "\n");
    return CmdResult::Error;
  }
  out(fmt("  result: DHT%u, %.1f °C, %.1f %%\n", r.model, r.temperature, r.humidity));
  return CmdResult::Ok;
}

static CmdResult cmdWatch(const Args& a, const Out& out) {
  if (a.size() == 1) {
    uint32_t s;
    {
      Lock lk(g_lock);
      s = g_settings.watchS;
    }
    out(s ? fmt("watch: every %lu s\n", (unsigned long)s) : String("watch: off\n"));
    return CmdResult::Ok;
  }
  if (a.size() != 2) return usage(out, "watch [seconds|off]");
  String value = a[1] == "off" ? "0" : a[1];
  String err;
  bool reboot;
  if (!settingsSet({{"watch", value}}, err, reboot)) {
    out("error: " + err + "\n");
    return CmdResult::Error;
  }
  out(value == "0" ? "watch: off\n" : "watch: every " + value + " s\n");
  return CmdResult::Ok;
}

static CmdResult cmdWifi(const Args& a, Console console, const Out& out) {
  if (a.size() == 1) {
    out(networkSummary() + "\n");
    return CmdResult::Ok;
  }
  if (console != Console::Usb) {
    out("error: Wi-Fi credentials can only be changed on the USB console\n");
    return CmdResult::Error;
  }
  if (a[1] == "set" && (a.size() == 3 || a.size() == 4)) {
    String pass = a.size() == 4 ? a[3] : "";
    settingsSaveWifi(a[2], pass);
    networkReconnect();
    out("saved; connecting to '" + a[2] + "'\n");
    return CmdResult::Ok;
  }
  if (a[1] == "clear" && a.size() == 2) {
    settingsClearWifi();
    networkReconnect();
    out("Wi-Fi credentials removed\n");
    return CmdResult::Ok;
  }
  return usage(out, "wifi [set \"<name>\" \"<password>\" | clear]");
}

static CmdResult cmdKey(const Args& a, const Out& out) {
  String sub = a.size() > 1 ? a[1] : "";
  if (sub == "list" && a.size() == 2) {
    auto keys = authKeysLoad();
    if (keys.empty()) out("no keys (add one with: key add <contents of ~/.ssh/id_ed25519.pub>)\n");
    for (size_t i = 0; i < keys.size(); i++)
      out(fmt("  %u  %s %s  %s\n", (unsigned)(i + 1), keys[i].type.c_str(),
              authKeyFingerprint(keys[i]).c_str(), keys[i].comment.c_str()));
    bool pw;
    {
      Lock lk(g_lock);
      pw = g_settings.passwordLogin;
    }
    if (!pw) out("password login: off\n");
    else if (!settingsHasPassword()) out("password login: no password set, only keys can log in\n");
    else out("password login: on (set password_login off once a key works)\n");
    return CmdResult::Ok;
  }
  if (sub == "add" && a.size() >= 4) {
    String line;
    for (size_t i = 2; i < a.size(); i++) line += (i > 2 ? " " : "") + a[i];
    String err, fp;
    if (!authKeysAdd(line, err, fp)) {
      out("error: " + err + "\n");
      return CmdResult::Error;
    }
    logEvent("Secure Shell key added: %s", fp.c_str());
    out("added " + fp + "\n");
    return CmdResult::Ok;
  }
  if (sub == "delete" && a.size() == 3) {
    if (!authKeysDelete(a[2].toInt())) {
      out("error: no key number " + a[2] + " (see: key list)\n");
      return CmdResult::Error;
    }
    logEvent("Secure Shell key %s deleted", a[2].c_str());
    out("deleted\n");
    return CmdResult::Ok;
  }
  return usage(out, "key list | key add <public key line> | key delete <number>");
}

static CmdResult cmdPasswd(const Args& a, const Out& out) {
  if (a.size() != 2) return usage(out, "passwd <new password>  (no spaces, or use quotes)");
  if (a[1].length() < PASSWORD_MIN_LENGTH) {
    out(fmt("error: use at least %d characters\n", PASSWORD_MIN_LENGTH));
    return CmdResult::Error;
  }
  settingsSetPassword(a[1]);
  logEvent("Secure Shell password changed");
  out("password changed\n");
  return CmdResult::Ok;
}

std::vector<String> commandCompletions(const String& before, Console console) {
  Args w = tokenize(before);
  size_t n = w.size();
  auto settingNames = [] {
    std::vector<String> v;
    for (const auto& s : settingsNames()) v.push_back(s.first);
    return v;
  };
  if (n == 0)
    return {"status", "get", "set", "mode", "log", "watch", "sensor", "ac", "fan", "ir", "wifi", "key",
            "passwd", "hostkey", "reboot", "exit", "help"};
  const String& c = w[0];
  if (c == "status" && n == 1) return {"json"};
  if (c == "get" && n == 1) return settingNames();
  if (c == "set") {
    if (n % 2 == 1) return settingNames();
    for (const auto& s : settingsNames())
      if (s.first == w[n - 1] && s.second) return {"on", "off"};
    return {};
  }
  if (c == "mode" && n == 1) return {"auto", "manual"};
  if (c == "watch" && n == 1) return {"off"};
  if (c == "sensor") {
    if (n == 1) return {"test"};
    if (n == 2 && w[1] == "test") return {"cabinet", "room", "box"};
  }
  if (c == "ac") {
    if (n == 1) return {"learn", "codes", "use", "on", "off", "force", "auto"};
    if (w[1] == "use" && n == 2) return {"on", "off"};
    if (w[1] == "use" && n == 3) return irStoreList();
    if (w[1] == "force" && n == 2) return {"on", "off"};
  }
  if (c == "fan") {
    if (n == 1) return {"on", "off", "force", "auto"};
    if (w[1] == "force" && n == 2) return {"on", "off"};
  }
  if (c == "ir") {
    if (n == 1) return {"list", "learn", "send", "show", "rename", "import", "delete"};
    if (n == 2 && (w[1] == "send" || w[1] == "show" || w[1] == "rename" || w[1] == "delete")) return irStoreList();
  }
  if (c == "wifi" && n == 1 && console == Console::Usb) return {"set", "clear"};
  if (c == "key" && n == 1) return {"list", "add", "delete"};
  return {};
}

String commandBanner() {
  return String("Climate controller (") + HOSTNAME + "). Type 'help' for commands.\n";
}

CmdResult runCommand(const String& line, Console console, const Out& out) {
  Args a = tokenize(line);
  if (a.empty()) return CmdResult::Ok;
  String c = a[0];
  c.toLowerCase();
  if (c == "help" || c == "?") {
    out(HELP);
    return CmdResult::Ok;
  }
  if (c == "status") return cmdStatus(a, out);
  if (c == "get") {
    if (a.size() > 2) return usage(out, "get [name]");
    String table = settingsTable(a.size() == 2 ? a[1] : "");
    if (!table.length()) {
      out("error: unknown parameter '" + a[1] + "' (see: get)\n");
      return CmdResult::Error;
    }
    out(table);
    return CmdResult::Ok;
  }
  if (c == "set") return cmdSet(a, out);
  if (c == "mode") return cmdMode(a, out);
  if (c == "ac" && a.size() >= 2 && a[1] == "learn") return cmdAcLearn(a, out);
  if (c == "ac" && a.size() == 2 && a[1] == "codes") return cmdAcCodes(out);
  if (c == "ac" && a.size() >= 2 && a[1] == "use") return cmdAcUse(a, out);
  if (c == "ac") return cmdOutput(a, out, true);
  if (c == "fan") return cmdOutput(a, out, false);
  if (c == "ir") return cmdIr(a, out);
  if (c == "log") return cmdLog(a, out);
  if (c == "watch") return cmdWatch(a, out);
  if (c == "sensor" && a.size() >= 2 && a[1] == "test") return cmdSensorTest(a, out);
  if (c == "wifi") return cmdWifi(a, console, out);
  if (c == "passwd") return cmdPasswd(a, out);
  if (c == "key") return cmdKey(a, out);
  if (c == "hostkey") {
    out(sshHostKeyFingerprint() + "\n");
    return CmdResult::Ok;
  }
  if (c == "reboot") {
    out("rebooting\n");
    logEvent("reboot (command)");
    delay(500);
    ESP.restart();
  }
  if (c == "exit" || c == "quit" || c == "logout") return CmdResult::Exit;
  out("unknown command '" + a[0] + "' (type help)\n");
  return CmdResult::Error;
}
