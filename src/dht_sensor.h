#pragma once
#include <Arduino.h>

// DHT11 / DHT22 reader. It detects the model from each reading (the two encode
// their data differently and no frame is valid for both) and reports why a reading
// failed, so the console can tell a missing sensor from a wiring fault.
enum class DhtError : uint8_t {
  None,
  NoResponse,   // nothing answered the start signal: missing sensor, wiring, power
  StuckLow,     // data line already low before starting: short circuit
  Incomplete,   // answer stopped partway: loose wire, interference
  Checksum,     // all bits arrived but the checksum is wrong: noise, long cable
  OutOfRange,   // values impossible for either model: faulty sensor
};

struct DhtResult {
  DhtError error = DhtError::None;
  float temperature = NAN;
  float humidity = NAN;
  uint8_t model = 0;  // 11 or 22 when the reading succeeded
  // What happened on the line, for `sensor test`:
  bool idleHigh = false;      // line was high (pulled up) before the start signal
  int32_t answerUs = -1;      // time until the sensor pulled the line low, -1 = never
  uint8_t bits = 0;           // data bits received (40 for a full reading)
  uint8_t raw[5] = {0};       // the bytes received
};

class DhtSensor {
 public:
  explicit DhtSensor(uint8_t pin) : pin_(pin) {}
  void begin();
  // Blocks for about 24 ms, about 5 ms of it with this core's interrupts disabled.
  // Call at most every 2 seconds.
  DhtResult read();

 private:
  uint8_t pin_;
};

const char* dhtErrorText(DhtError e);
