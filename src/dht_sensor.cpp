#include "dht_sensor.h"
#include <driver/gpio.h>
#include <esp_timer.h>

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

void DhtSensor::begin() {
  // Open drain with pull-up: writing 0 pulls the line low, writing 1 releases it and
  // lets the sensor drive it, with no mode switching during the timed part.
  gpio_config_t c = {};
  c.pin_bit_mask = 1ULL << pin_;
  c.mode = GPIO_MODE_INPUT_OUTPUT_OD;
  c.pull_up_en = GPIO_PULLUP_ENABLE;
  gpio_config(&c);
  gpio_set_level((gpio_num_t)pin_, 1);
}

// Waits while the line stays at `level`; returns the microseconds waited, or -1.
static inline int32_t IRAM_ATTR waitWhile(gpio_num_t pin, int level, int32_t timeoutUs) {
  int64_t start = esp_timer_get_time();
  while (gpio_get_level(pin) == level) {
    int32_t waited = esp_timer_get_time() - start;
    if (waited > timeoutUs) return -1;
  }
  return esp_timer_get_time() - start;
}

// Decodes a frame as one model; false if the values are impossible for it.
static bool decode(uint8_t type, const uint8_t* b, float& t, float& h) {
  if (type == 11) {
    h = b[0] + b[1] * 0.1f;
    t = b[2] + (b[3] & 0x7f) * 0.1f;
    if (b[3] & 0x80) t = -t;
    // A DHT11 sends decimals as one digit and never reports below 5 %; DHT22 data read
    // this way breaks one of the two.
    return b[1] <= 9 && (b[3] & 0x7f) <= 9 && h >= 5 && h <= 100 && t >= -20 && t <= 60;
  }
  h = ((b[0] << 8) | b[1]) * 0.1f;
  t = (((b[2] & 0x7f) << 8) | b[3]) * 0.1f;
  if (b[2] & 0x80) t = -t;
  return h >= 0 && h <= 100 && t >= -40 && t <= 80;
}

DhtResult DhtSensor::read() {
  DhtResult r;
  gpio_num_t pin = (gpio_num_t)pin_;
  gpio_set_level(pin, 1);
  delayMicroseconds(50);
  r.idleHigh = gpio_get_level(pin) == 1;
  if (!r.idleHigh) {
    r.error = DhtError::StuckLow;
    return r;
  }

  // Start signal: hold the line low 18.5 ms. A DHT11 needs at least 18 ms, a DHT22
  // accepts 0.8 to 20 ms, so this suits both without knowing the model.
  int64_t startUs = esp_timer_get_time();
  gpio_set_level(pin, 0);
  vTaskDelay(pdMS_TO_TICKS(17));
  while (esp_timer_get_time() - startUs < 18500) {
  }

  uint8_t* b = r.raw;
  DhtError err = DhtError::None;
  portENTER_CRITICAL(&s_mux);
  gpio_set_level(pin, 1);
  // The sensor answers by pulling low about 80 µs, then high about 80 µs.
  r.answerUs = waitWhile(pin, 1, 200);
  if (r.answerUs < 0) err = DhtError::NoResponse;
  else if (waitWhile(pin, 0, 150) < 0 || waitWhile(pin, 1, 150) < 0) err = DhtError::Incomplete;
  // 40 bits: about 50 µs low, then high for about 27 µs (0) or 70 µs (1).
  for (int i = 0; i < 40 && err == DhtError::None; i++) {
    int32_t high;
    if (waitWhile(pin, 0, 150) < 0 || (high = waitWhile(pin, 1, 150)) < 0) {
      err = DhtError::Incomplete;
      break;
    }
    b[i / 8] = (b[i / 8] << 1) | (high > 45 ? 1 : 0);
    r.bits = i + 1;
  }
  portEXIT_CRITICAL(&s_mux);

  if (err != DhtError::None) {
    r.error = err;
    return r;
  }
  if (((b[0] + b[1] + b[2] + b[3]) & 0xff) != b[4]) {
    r.error = DhtError::Checksum;
    return r;
  }
  float t, h;
  for (uint8_t model : {22, 11}) {
    if (decode(model, b, t, h)) {
      r.temperature = t;
      r.humidity = h;
      r.model = model;
      return r;
    }
  }
  r.error = DhtError::OutOfRange;
  return r;
}

const char* dhtErrorText(DhtError e) {
  switch (e) {
    case DhtError::None: return "ok";
    case DhtError::NoResponse: return "not answering: check wiring and power";
    case DhtError::StuckLow: return "data line held low: check for a short";
    case DhtError::Incomplete: return "incomplete reading: loose wire or interference";
    case DhtError::Checksum: return "corrupted reading: interference or cable too long";
    case DhtError::OutOfRange: return "readings out of range: faulty sensor?";
  }
  return "?";
}
