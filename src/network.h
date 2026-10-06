#pragma once
#include <Arduino.h>

// Wi-Fi credentials come from flash (set with the `wifi` command on the USB console).
void networkBegin();
void networkLoop();
void networkReconnect();
void networkApplyTxPower();  // after changing wifi_power
String networkSummary();
