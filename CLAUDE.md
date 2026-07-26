# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

SGMK2 ("Sentry Gun Mk2") is a PlatformIO/Arduino firmware for an ESP32 that drives a motorized foam-dart sentry turret: three stepper motors (pan, tilt, dart pusher), flywheel motor, laser pointer, and a piezo buzzer. It is controlled entirely over MQTT — there is no local UI. Vision/targeting lives outside this repo: the camera runs ESPHome (config kept in a separate `esphome` project), and whatever does the aiming publishes MQTT commands to this device.

## Build / upload

`pio` is not on `PATH`; use `~/.platformio/penv/bin/pio`.

```sh
~/.platformio/penv/bin/pio run                 # build
~/.platformio/penv/bin/pio run -t upload       # upload (OTA, see below)
~/.platformio/penv/bin/pio device monitor      # serial monitor @ 115200
```

Upload is **OTA only** (`upload_protocol = espota`, `upload_port = sgmk2.iot`) — you must be on the same LAN as the turret, and it must be powered and connected. To flash over USB instead, temporarily override `upload_protocol`/`upload_port` (e.g. `pio run -t upload --upload-port /dev/cu.usbserial-*`) rather than committing a change to `platformio.ini`.

There are no tests (`test/` holds only the PlatformIO placeholder README).

## secrets.h

`src/main.cpp` includes `secrets.h`, which is gitignored and **not present in a fresh clone** — the build fails until you create it (in `include/` or `src/`):

```cpp
const char *ssid = "...";
const char *password = "...";
```

## Architecture

Single translation unit, `src/main.cpp`, in classic Arduino shape:

- **`setup()`** — pin config, plays the `bootup` RTTTL tune, configures stepper speeds/accelerations, then homes tilt against the endstop (`TILT_CALIBRATION`): drive tilt negative until `ENDSTOP_PIN` reads high, then `setCurrentPosition(0)`. All tilt positions afterwards are steps relative to that home. Plays `ready` when done.
- **`onConnectionEstablished()`** — the EspMQTTClient callback where every subscription is registered. This is the whole command surface; add new commands here.
- **`loop()`** — must stay non-blocking. It pumps `stepperTilt.run()`, `stepperPan.run()`, `rtttl::play()`, and `client.loop()` every iteration. Anything that busy-waits here stalls MQTT, OTA, and motion at once. (`setup()` deliberately busy-waits on `rtttl::done()`, which is fine only because nothing else is running yet.)

Key invariants:

- **Tilt is clamped to `TILTMIN`..`TILTMAX`** in both the absolute and relative handlers; pan is unbounded (continuous rotation). Keep any new tilt path clamped.
- **Firing is step-counted, not time-based.** `STEPS_PER_SHOT` steps of `stepperPusher` = one dart. `loop()` keeps stepping past a `shooting = false` until `stepsShot % STEPS_PER_SHOT == 0`, so a stop request always completes the dart in flight instead of jamming mid-cycle.
- **Rate of fire is a speed, not a delay.** `setRateOfFire(rpm)` converts rounds/min → steps/sec via `STEPS_PER_SHOT` and caps at 950 steps/s (motor limit).
- **Flywheels gate shooting.** `sgmk2/shoot` is ignored unless `FLYWHEELS_PIN` is already high, and turning flywheels off force-clears `shooting`.

## MQTT interface

Broker `mqtt.jserrats.xyz:1883`, client id `sgmk2`. Retained LWT on `sgmk2/status` (`online`/`offline`).

| Topic | Payload | Effect |
| --- | --- | --- |
| `sgmk2/pan/relative` | int steps | relative pan, unbounded |
| `sgmk2/tilt/absolute` | int steps | absolute tilt, ignored outside `TILTMIN`..`TILTMAX` |
| `sgmk2/tilt/relative` | int steps | relative tilt, clamped to the limits |
| `sgmk2/flywheel` | `on` / anything | spin up / spin down (also stops shooting) |
| `sgmk2/shoot` | `on` / anything | start / stop firing (needs flywheels on) |
| `sgmk2/laser` | `on` / anything | laser pointer |
| `sgmk2/rof` | int rounds/min | rate of fire |
| `sgmk2/alert` | any | play the alert tune on the buzzer |

Payload convention: only the exact string `on` enables; every other payload is treated as off.

## Hardware pin map

Changing these means rewiring, so treat them as fixed unless the user says the hardware changed.

| | Pin(s) |
| --- | --- |
| Buzzer | 14 |
| Tilt endstop | 34 (`INPUT_PULLDOWN`, other side to ground) |
| Flywheels | 32 |
| Laser | 33 |
| Pusher stepper (step, dir) | 19, 18 |
| Tilt stepper (step, dir) | 5, 17 |
| Pan stepper (step, dir) | 16, 4 |

Note: the PlatformIO env is named `esp32-c3-devkitm-1` but `board = nodemcu-32s` (a plain ESP32) — the board setting is what's real; the env name is stale.
