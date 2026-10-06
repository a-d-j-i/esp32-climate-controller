// Climate controller: reads a DHT11/DHT22, switches the air conditioner with stored
// infrared codes and a fan through a relay, configured over Secure Shell or USB.
#include <Arduino.h>
#include "console_session.h"
#include "control.h"
#include "hw_task.h"
#include "ir.h"
#include "network.h"
#include "settings.h"
#include "ssh_shell.h"
#include "state.h"
#include "usb_console.h"

void setup() {
  Serial.setRxBufferSize(2048);  // room for a pasted public key line (RSA: up to ~750 characters)
  Serial.begin(115200);
  delay(200);
  hwBegin();
  usbConsoleBegin();  // first, so it shows every event from boot on
  settingsLoad();
  controlBegin();
  irBegin();
  networkBegin();
  sshBegin();
  enableLoopWDT();  // reboot if the control loop ever hangs
  logEvent("started, last restart: %s", resetReasonText());
}

// Core 1: infrared, relay and control rules, plus hardware requests from the
// consoles. Wi-Fi, the consoles and the sensor run in their own tasks on core 0.
void loop() {
  networkLoop();
  irPoll();
  hwPoll();
  controlTick();
  watchTick();
  delay(5);
}
