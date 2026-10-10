# ESP32 climate controller

Keeps a cabinet cool: reads up to three DHT11 or DHT22 sensors (cabinet, room, and the
controller's own box), switches an air conditioner with recorded infrared codes and a
fan through a relay. Everything is configured over Secure Shell on the ESP32 itself
(or over the USB serial console). Settings, infrared codes, the password, keys, the
host key and the Wi-Fi credentials are stored in flash and survive reboots, power loss and
firmware updates.

## Wiring (ESP32 DevKit)

The full wiring guide, with the pinout, the perfboard layout, cabling and parts list, is
`docs/index.html`, published with GitHub Pages (Settings → Pages → deploy from the
`docs` folder). Its board drawing embeds the data from `python3 docs/layout.py --json`.

| Part | ESP32 pin | Notes |
|---|---|---|
| DHT11 / DHT22 data (cabinet) | GPIO 17 (TX2) | 10 kΩ pull-up to 3.3 V (most breakout boards have it) |
| DHT11 / DHT22 data (room, optional) | GPIO 16 (RX2) | Up to 2 m of cable; same pull-up as the cabinet sensor |
| DHT11 / DHT22 data (box, optional) | GPIO 23 (D23) | Inside the controller's box, away from the ESP32 module and the relay; same pull-up. Shown in `status` and the `watch` line only |
| Infrared receiver (VS1838B, TSOP38238) | GPIO 4 (D4) | On the board at an edge; 100 Ω in series with its supply, 10 µF across it |
| Relay module input | GPIO 27 (D27) | Through an NPN transistor: GPIO → 1 kΩ → base, 10 kΩ GPIO to ground; collector to the module input with 10 kΩ to 5 V; emitter to ground. The module needs a full 0 V / 5 V input; `RELAY_ACTIVE_LOW` is `false` because the transistor inverts. The fan is on the relay's NC contact (`FAN_ON_RELAY_NC`), so it runs if the board fails or loses power |
| Infrared LED | GPIO 32 (D32) | Through an NPN transistor (2N2222 / BC547): GPIO → 1 kΩ → base; LED + ~33 Ω from 5 V to collector; emitter to ground |

Everything on 3.3 V connects to top-row pins next to 3V3, everything on 5 V to bottom-row
pins, so the sensors and the relay sit on opposite sides of the board. The relay uses
D27, the infrared LED D32 near the EN end; the supply capacitors sit in a row of holes
between the GND and VIN wires.

Pins and other constants are in `include/config.h`. Mains wiring to the fan needs a
properly rated relay and enclosure.

## Build and first setup

There are no compiled-in credentials. Out of the box the Secure Shell port is closed;
it opens once a password or key has been set on the USB console.

```sh
pio run -t upload
pio device monitor                                # USB console, 115200 baud
```

In the USB console (physical access is trusted, so it has no login):

```
climate> wifi set "My Network" "my-wifi-password"
climate> key add ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAA... me@laptop   # cat ~/.ssh/id_ed25519.pub
climate> passwd <at least 12 characters>                               # optional
climate> wifi                                                          # shows the address
climate> hostkey
```

Compare the host key fingerprint with what `ssh` shows on the first connection.

```sh
ssh admin@192.168.1.50           # interactive shell (use the address from `wifi`)
ssh admin@192.168.1.50 status    # single command, also: 'status json', 'set ...; get'
```

Most routers also resolve the name `climate` given to them by the device. Multicast DNS
(`climate.local`) is off by default; set `ENABLE_MDNS 1` in `include/config.h` to enable it.

### Keys and passwords

- `key add`, `key list`, `key delete` work on both consoles. Up to 8 keys
  (`ssh-ed25519`, `ecdsa-sha2-nistp256/384/521`, `ssh-rsa` of at least 2048 bits,
  signed with SHA-2), stored
  in flash as `/authorized_keys`.
- Once key login works, `set password_login off` disables password login entirely.
- Lost key or forgotten password: `key add` or `passwd` on the USB console.

## Security

The device is meant to be safe on an untrusted Wi-Fi network:

- **One listening port**: TCP 22, Secure Shell, and only after credentials exist. No web
  server, no over-the-air updates (firmware is flashed over USB only), multicast DNS off
  by default. Outgoing only: DHCP and network time (time only affects log timestamps).
- **Strict algorithms**: Ed25519 host key; curve25519 key exchange; ChaCha20-Poly1305,
  AES-GCM or AES-CTR; SHA-2 message authentication. libssh 0.11.5 with strict key
  exchange (protects against the Terrapin attack).
- **Password guessing**: 3 attempts per connection, connections are served one at a
  time, and the delay after a failed password grows by 1 s per consecutive failure up
  to 30 s. Passwords need 12 characters and are stored salted and hashed.
- **Malformed or hostile traffic**: the whole handshake (key exchange, login, channel
  setup) must finish within 30 s; incoming packets are limited to 64 KB (libssh would
  otherwise buffer 256 KB from a client that has not logged in, more than the free
  memory); commands are limited to 4 KB; control characters are removed from logged
  text such as user names.
- **What the network can still do**: anyone on the Wi-Fi can keep the single session
  slot busy or jam the radio, so remote access can be denied. Climate control does not
  depend on the network and keeps running. A fake access point with the same name
  cannot get in or read anything, as long as you check the host key fingerprint.
- `status` shows free memory and its lowest point since boot.

To confirm nothing else is listening after flashing (from a computer on the same network):

```sh
nmap -p- 192.168.1.50                    # expect only 22/tcp open
sudo nmap -sU --top-ports 200 192.168.1.50
```

## How the two cores are used

- **Core 1** (the Arduino loop): infrared receiver and transmitter, relay, automatic
  rules. Console commands that touch hardware (`ac`, `fan`, `ir send`) are queued to
  it, so nothing else drives the pins.
- **Core 0**: Wi-Fi, the Secure Shell and USB consoles, and the DHT read. The DHT
  protocol is read with interrupts disabled for about 5 ms, which only affects the core
  it runs on, so it cannot corrupt an infrared capture on core 1.
- A watchdog reboots the device if the control loop ever hangs.

## Recording the air conditioner codes

Most air conditioner remotes send their whole state (power, mode, temperature, fan) with
every press, and the device decodes it, so it can tell on from off by itself:

```
climate> ac learn              # press the power button, wait a second, press it again
climate> ac codes              # which codes turn the air conditioner on and off
climate> ac force on 1         # test
```

- `ac learn` records both codes and puts them in use. It works with a power button that
  toggles, whichever state the unit starts in. Set the remote's mode, temperature and fan
  first: the "on" message carries them.
- Which codes are used is a saved setting, shown by `ac codes`, `status` and `ir list`.
  `ac use on|off <code>` assigns any other stored code, e.g. one recorded with
  `ir learn cool_22`. Renaming a code keeps it in use; a code in use can't be deleted.
- `ir learn`, `ir send` and the other `ir` commands are for extra buttons or other
  devices. `ir learn` says whether a code turns the air conditioner on or off.
- Codes recorded with another tool can be imported as a list of microsecond timings
  (ESPHome's negative numbers and IRremoteESP8266's `{...}` arrays are accepted):
  `ir import cool_22 4400,4300,550,1600,550,550,...`

## Automatic control

The fan and the air conditioner each have their own temperature range. Set the fan
range below the air conditioner range to make the air conditioner the last resort.

```
climate> get
climate> set fan_on_above 26 fan_off_below 24.5
climate> set ac_on_above 28 ac_off_below 25.5
climate> mode auto
```

- **Fan**: on at `fan_on_above`, off at `fan_off_below`, as long as it can help. With
  a room sensor it starts only when
  the room is at least `fan_room_margin` (default 1 °C) cooler than the cabinet, and stops
  when the room is no more than `fan_room_stop` (default 0 °C) cooler. A cabinet with
  something warm in it levels off a little above the room, so with `fan_room_stop 0` the
  fan can keep running until `fan_off_below`; set it to about 1-2 °C to let it stop. If the room temperature is unknown (no sensor, or no recent
  reading) the fan is allowed. `set fan_use_room off` keeps the room temperature
  shown but leaves it out of this rule, so the fan follows the cabinet thresholds alone.
- `fan_min_on` (minutes, default 0): once the rules start the fan, it runs at least this
  long, so a cabinet that heats quickly doesn't cycle it every few minutes. It only
  postpones the stop at `fan_off_below`: a room that is no longer cooler, or the air
  conditioner coming on, still stops the fan at once. There is no minimum off time: the
  cabinet could overheat while the fan waits.
- **Air conditioner**: on at `ac_on_above`, off at `ac_off_below`.
- **Never both**: turning the air conditioner on (automatically, with `ac on` or with the
  original remote) switches the fan off, and `fan on` is refused until `ac off`.
- Humidity is shown by `status` but not used for decisions.
- `ac_min_cycle` (default 180 s) limits how often the air conditioner is switched automatically.
- Automatic control pauses if the cabinet sensor has not given a valid reading for 60 s.
- Calibrate the two sensors against each other: put them side by side for 30 minutes,
  compare `Cabinet` and `Room` in `status`, and set `room_offset` to the difference
  (e.g. `set room_offset 0.5` when the room sensor reads 0.5 °C low). Each DHT22 can be
  off by 0.5 °C, as much as the default fan margin.
- `ac on|off` and `fan on|off` switch to manual mode; `mode auto` resumes.

### Forcing outputs (for testing)

`ac force on|off [minutes]` and `fan force on|off [minutes]` act as if the temperature
had just crossed the threshold, and stay in automatic mode:

- forced **on** lasts until the temperature falls to the off threshold
  (`ac_off_below` / `fan_off_below`);
- forced **off** lasts until it rises to the on threshold (`ac_on_above` / `fan_on_above`);
- then the normal rules take over. The end point only counts once the temperature has
  been on the other side of it, so forcing the air conditioner on in a room that is
  already below `ac_off_below` keeps it on until the room has warmed above that point
  and cooled back down;
- the optional `minutes` ends the force earlier; `ac auto` / `fan auto` end it now (and
  leave manual mode, if it was on), and
  `mode manual`, a reboot or using the original remote clear all forces;
- a forced fan ignores the room temperature, but the fan is still never on together
  with the air conditioner: `fan force on` is refused while it is on, and turning the
  air conditioner on ends a fan force.

`status` shows active forces and when they end.
- Infrared is one-way, so the air conditioner state is what was last sent or seen. After
  boot the state is unknown and the first automatic decision always sends a code.
- When the original remote is used, the receiver updates the state, and with
  `remote_override on` (default) the controller switches to manual mode so it does not
  fight you.
- All three sensors are read every 3 s; the room and box sensors need no setting and can
  be plugged in or out at any time. A sensor that isn't answering shows as `off` in the
  `watch` line; `status` says why.
- The box sensor reports the temperature inside the controller's own box. It is only
  shown (in `status` and the `watch` line), never used by the rules; `box_offset` calibrates it like `room_offset`.
- DHT11 and DHT22 are detected automatically from their readings, so sensors can be
  swapped without changing anything. `status` shows the model found and, when a sensor
  fails, why: not answering, data line held low, incomplete or corrupted readings, or
  values out of range.

## Consoles

The Secure Shell session and the USB console (`pio device monitor`) behave the same:

- Events (fan on, air conditioner off, setting changes, logins, sensor problems) appear as they happen,
  printed above the `climate>` prompt. The prompt stays on the last line and whatever
  you were typing is redrawn, so live output never mixes with your typing.
- Tab completes commands, setting names, `on`/`off` and stored code names; Tab twice
  lists the choices (`get` Tab Tab shows every setting).
- Shell-style editing: arrows, Home/End, Ctrl-A/E, Ctrl-U/K/W, Delete, Ctrl-L, and Up/Down
  (or Ctrl-P/N) for the last 20 commands. The history is shared by the USB console and
  Secure Shell, kept in memory only (a reboot clears it), and leaves out `passwd` and
  `wifi set`, which contain secrets.
- `watch 60` prints a one-line status every 60 seconds (`watch off` stops it). The
  setting is saved and survives reboots; it is on, every 60 s, by default.
- The history is your terminal's own scrollback: scroll with the mouse wheel, search and
  copy as usual. The device does not keep it, apart from the last 40 events in memory
  for `log`, which a reboot clears. If your terminal jumps to the bottom on new output
  while you read, turn off its "scroll on output" option.
- `platformio.ini` sets `monitor_filters = direct` so the USB monitor passes the
  terminal codes used for the redraw instead of showing them as symbols.
- Single commands (`ssh admin@<address> status`) print only their own output.

`help` lists all commands; `log` shows recent events.

## Notes

- Power: the ESP32 draws short peaks of several hundred mA while transmitting. On a weak
  supply (long cable, hub, a port shared with other devices) heavy network traffic, such
  as a port scan, can make it reset; `status` then shows `Last restart: brownout`. Give it
  its own port or a 1 A charger, fit C1, or lower `wifi_power` (default 15 dBm, range
  2-19.5; `status` shows the current value under Wi-Fi).

- One Secure Shell session at a time; a second connection waits until the first ends.
  A session stays open while it is idle unless `ssh_idle` is set (minutes without
  typing; `watch` output doesn't count). A client that disappears without closing the
  connection (laptop asleep, out of Wi-Fi) is detected by TCP keepalive and its
  session ends within about 90 s, so the slot is never held by a dead connection.
- `pio run -t erase` followed by an upload wipes everything stored in flash: settings,
  codes, keys, Wi-Fi credentials and the host key.

## License

MIT, see `LICENSE`. The libraries fetched at build time keep their own licenses
(IRremoteESP8266 and LibSSH-ESP32: LGPL-2.1).
