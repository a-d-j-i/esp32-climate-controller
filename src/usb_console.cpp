#include "usb_console.h"
#include "config.h"
#include "console_session.h"
#include <Arduino.h>

static void usbTask(void*) {
  ConsoleSession session(Console::Usb, true, [](const String& s) { Serial.print(toCrlf(s)); });
  session.start();
  char buf[64];
  for (;;) {
    int n = 0;
    while (Serial.available() && n < (int)sizeof buf) buf[n++] = (char)Serial.read();
    if (n) session.feed(buf, n);
    session.poll();
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

void usbConsoleBegin() { xTaskCreatePinnedToCore(usbTask, "usb", 12288, nullptr, 1, nullptr, CONSOLE_CORE); }
