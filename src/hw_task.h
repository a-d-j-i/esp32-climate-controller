#pragma once
#include <functional>

// Core 1 (the Arduino loop) owns the timing-sensitive hardware: infrared receive and
// send, the relay and the control rules. Consoles on core 0 hand hardware work to it
// with hwRun() instead of touching the pins themselves.
void hwBegin();  // call from setup(), which runs in the loop task
void hwPoll();   // call from loop()
// Runs fn in the loop task and waits until it has finished.
void hwRun(const std::function<void()>& fn);
