#pragma once

// Pin assignment (ESP32 DevKit V1, 30 pins). Everything on 3.3 V uses top-row pins,
// next to the 3V3 pin; everything on 5 V uses bottom-row pins: the relay on D27,
// the infrared LED at the far end. D23 (SPI MOSI) holds the box sensor; D21/D22 stay free
// for I2C and D18/D19 for SPI (the ESP32 can route MOSI to another pin). Avoid the strapping pins 0, 2, 5, 12
// and 15. GPIO16/17 are free on WROOM modules (on WROVER modules they belong to the PSRAM).
#define PIN_DHT_CABINET 17   // TX2
#define PIN_DHT_ROOM 16      // RX2, optional sensor in the room (shown whenever it answers)
#define PIN_DHT_BOX 23       // D23, optional sensor inside the controller's box (shown whenever it answers)
#define PIN_IR_RECV 4        // D4, 38 kHz infrared receiver (VS1838B, TSOP38238, ...)
#define PIN_RELAY 27         // D27, fan relay (through transistor Q2)
#define PIN_IR_SEND 32       // D32, infrared LED through a transistor
// The relay module switches on when its input is LOW (PNP input transistor on 5 V), but
// the ESP32's 3.3 V high can't switch it fully off, so an NPN transistor on the board
// drives the module input (10 kΩ pull-up to 5 V): D27 HIGH = relay on.
#define RELAY_ACTIVE_LOW false
// The fan is wired through the relay's normally closed (NC) contact, so it runs whenever
// the relay is unpowered: if the board fails or loses power, the fan stays on. The
// firmware energises the relay to switch the fan off. Set false for the NO contact.
#define FAN_ON_RELAY_NC true

#define HOSTNAME "climate"
// Multicast DNS (HOSTNAME.local) answers anyone on the network; off by default so the
// Secure Shell server is the only thing listening. Use the address shown by `wifi`.
#define ENABLE_MDNS 0
// POSIX time zone string, e.g. "CET-1CEST,M3.5.0,M10.5.0/3" for central Europe.
#define TZ_INFO "UTC0"
#define NTP_SERVER "pool.ntp.org"

#define SENSOR_INTERVAL_MS 3000
#define SENSOR_STALE_MS 60000      // automatic control pauses without a reading this recent

#define IR_CAPTURE_BUFFER 1024     // air conditioner frames are long
#define IR_CAPTURE_TIMEOUT_MS 50   // gap that ends a frame; 50 suits air conditioner remotes
#define IR_ECHO_IGNORE_MS 1000     // ignore our own transmission picked up by the receiver
#define IR_BURST_GAP_MS 600        // messages closer than this are one button press (remotes repeat)
#define IR_MAX_NAME 24

// Core 0 runs Wi-Fi, the Secure Shell and USB consoles and the DHT read (which
// disables interrupts on its own core for about 5 ms). Core 1, the Arduino loop,
// runs the infrared receiver and transmitter, relay and control rules.
#define CONSOLE_CORE 0
#define SENSOR_CORE 0
#define SENSOR_TASK_STACK 4096

#define SSH_USER "admin"
#define SSH_PORT "22"
#define SSH_LOGIN_TIMEOUT_S 30         // whole handshake: key exchange, login, channel
#define SSH_FAIL_DELAY_MAX_S 30        // delay grows by 1 s per consecutive failed password
#define PASSWORD_MIN_LENGTH 12
#define RSA_MIN_BITS 2048              // libssh would accept 1024-bit RSA keys
#define SSH_IDLE_TIMEOUT_MS (15UL * 60 * 1000)
#define SSH_MAX_AUTH_TRIES 3
#define SSH_TASK_STACK 51200

#define EVENT_LOG_SIZE 40
