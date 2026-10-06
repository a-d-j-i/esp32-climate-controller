#pragma once
#include <Arduino.h>
#include "state.h"

// Starts the sensor task on core 0. controlTick() runs the automatic rules in the
// main loop whenever a new cabinet reading has arrived.
void controlBegin();
void controlTick();
bool controlSensorFresh();  // cabinet sensor
bool readingFresh(const Reading& r);

enum class SensorId : uint8_t { Cabinet, Room, Box };
// "cabinet", "room", "box"
const char* sensorName(SensorId id);

enum class SensorState { Starting, Ok, Failing };
struct SensorInfo {
  SensorState state;
  Reading reading;  // includes the detected model
  bool fresh;       // a valid reading within SENSOR_STALE_MS
  String problem;   // with Failing
};
SensorInfo controlSensorInfo(SensorId id);
// One reading with the details of what happened on the line (`sensor test`).
DhtResult controlSensorTest(SensorId id);
uint8_t controlSensorPin(SensorId id);
// e.g. "DHT22, ok", "DHT22, not answering: check wiring and power", "detecting".
String sensorSummary(const SensorInfo& info);

// Sends the code assigned with `ac learn` / `ac use` and records the new state. Turning the air
// conditioner on also turns the fan off.
bool setAc(bool on, const char* reason, String* err = nullptr);
// Refuses to turn the fan on while the air conditioner is on.
bool setFan(bool on, const char* reason, String* err = nullptr);
// Sets the output now and holds it, in automatic mode, until the temperature reaches
// the end point of its range (see Force in state.h) or `minutes` pass (0 = no limit).
bool forceAc(bool on, uint32_t minutes, String* err);
bool forceFan(bool on, uint32_t minutes, String* err);
void releaseForce(bool ac);
void controlClearForces();
// Called when the original remote was seen turning the air conditioner on or off.
void onRemoteAc(bool on, const String& detail);
