#pragma once
#include <Arduino.h>
#include <utility>
#include <vector>

// All persistent data lives in the flash non-volatile storage, namespace "climate".
void settingsLoad();
void settingsSaveMode();

// Applies name/value pairs all together or not at all, so a threshold range can be
// moved in one step. Sets needsReboot when a changed parameter only applies at boot.
bool settingsSet(const std::vector<std::pair<String, String>>& pairs, String& err,
                 bool& needsReboot);
String settingsTable(const String& only = "");  // "" = all; empty result if no such name
// Parameter names, with whether each is on/off (for completion).
std::vector<std::pair<String, bool>> settingsNames();

bool settingsHasPassword();
bool settingsCheckPassword(const char* password);
void settingsSetPassword(const String& password);

String settingsLoadHostKey();
void settingsSaveHostKey(const char* base64);

bool settingsLoadWifi(String& ssid, String& password);
void settingsSaveWifi(const String& ssid, const String& password);
void settingsClearWifi();

// Names of the stored infrared codes that turn the air conditioner on and off
// ("" = none assigned). Set by `ac learn` or `ac use`.
String settingsAcCode(bool on);
void settingsSetAcCode(bool on, const String& name);
bool settingsAcCodeSaved(bool on);
