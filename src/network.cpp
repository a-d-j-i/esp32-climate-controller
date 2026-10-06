#include "network.h"
#include "config.h"
#include "settings.h"
#include "state.h"
#include <ESPmDNS.h>
#include <WiFi.h>
#include <esp_wifi.h>

static bool s_servicesStarted = false;
static bool s_wasConnected = false;

// The radio's current peaks while transmitting are what a weak USB supply can't
// deliver; less transmit power means smaller peaks. Steps are 0.25 dBm.
void networkApplyTxPower() {
  float dbm;
  {
    Lock lk(g_lock);
    dbm = g_settings.wifiPowerDbm;
  }
  int8_t quarter = (int8_t)constrain((int)(dbm * 4 + 0.5f), 8, 78);
  if (esp_wifi_set_max_tx_power(quarter) != ESP_OK) logEvent("Wi-Fi: could not set transmit power");
}

void networkReconnect() {
  String ssid, pass;
  WiFi.disconnect();
  if (!settingsLoadWifi(ssid, pass)) {
    logEvent("Wi-Fi not configured: on the USB console run  wifi set \"<name>\" \"<password>\"");
    return;
  }
  WiFi.begin(ssid.c_str(), pass.c_str());
  networkApplyTxPower();
  logEvent("Wi-Fi connecting to '%s'", ssid.c_str());
}

void networkBegin() {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);  // keeps Secure Shell responsive
  networkReconnect();
}

void networkLoop() {
  bool connected = WiFi.status() == WL_CONNECTED;
  if (connected != s_wasConnected) {
    s_wasConnected = connected;
    if (connected) logEvent("Wi-Fi connected, address %s", WiFi.localIP().toString().c_str());
    else logEvent("Wi-Fi disconnected");
  }
  if (connected && !s_servicesStarted) {
    s_servicesStarted = true;
    configTzTime(TZ_INFO, NTP_SERVER);
#if ENABLE_MDNS
    // Name lookup only (HOSTNAME.local); the Secure Shell host key check is what
    // proves the device's identity.
    if (MDNS.begin(HOSTNAME)) MDNS.addService("ssh", "tcp", atoi(SSH_PORT));
#endif
  }
}

String networkSummary() {
  String ssid, pass;
  if (!settingsLoadWifi(ssid, pass)) return "not configured";
  if (WiFi.status() != WL_CONNECTED) return "'" + ssid + "', not connected";
  int8_t quarter = 0;
  esp_wifi_get_max_tx_power(&quarter);
  return "'" + ssid + "', address " + WiFi.localIP().toString() + ", signal " + String(WiFi.RSSI()) + " dBm" +
         ", transmit " + String(quarter / 4.0f, 1) + " dBm" +
         (ENABLE_MDNS ? ", " HOSTNAME ".local" : "");
}
