# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

SGMK2 ("Sentry Gun Mk2") is a motorized foam-dart sentry turret: three stepper motors (pan, tilt, dart pusher), flywheel motor, laser pointer, and a piezo buzzer, on an ESP32 controlled entirely over MQTT. The repo holds three parts:

- `firmware/` — the PlatformIO/Arduino firmware for the ESP32. No local UI.
- `cv/` — the web controller (Flask + socket.io + OpenCV): serves the camera feed, reads a gamepad through the browser, does face-detection autoaim, and publishes the MQTT commands.
- `chart/` — Helm chart deploying `cv/` to Kubernetes (ArgoCD on ganymede, `path: chart`).

The camera itself runs ESPHome; its config is kept in the separate `esphome` project.

## Firmware build / upload

`pio` is not on `PATH`; use `~/.platformio/penv/bin/pio`. The PlatformIO project is `firmware/`: pass `-d firmware` from the repo root (or `cd firmware`).

```sh
~/.platformio/penv/bin/pio run -d firmware                # build
~/.platformio/penv/bin/pio run -d firmware -t upload      # upload (OTA, see below)
~/.platformio/penv/bin/pio device monitor -d firmware     # serial monitor @ 115200
```

Upload is **OTA only** (`upload_protocol = espota`, `upload_port = sgmk2.iot`) — you must be on the same LAN as the turret, and it must be powered and connected. To flash over USB instead, temporarily override `upload_protocol`/`upload_port` (e.g. `pio run -d firmware -t upload --upload-port /dev/cu.usbserial-*`) rather than committing a change to `platformio.ini`.

There are no tests (`firmware/test/` holds only the PlatformIO placeholder README).

## secrets.h

`firmware/src/main.cpp` includes `secrets.h`, which is gitignored and **not present in a fresh clone** — the build fails until you create it as `firmware/include/secrets.h`:

```cpp
const char *ssid = "...";
const char *password = "...";
```

## Firmware architecture

Single translation unit, `firmware/src/main.cpp`, in classic Arduino shape:

- **`setup()`** — pin config, plays the `bootup` RTTTL tune, configures stepper speeds/accelerations, then homes tilt against the endstop (`TILT_CALIBRATION`): drive tilt negative until `ENDSTOP_PIN` reads high, then `setCurrentPosition(0)`. All tilt positions afterwards are steps relative to that home. Plays `ready` when done.
- **`onConnectionEstablished()`** — the EspMQTTClient callback where every subscription is registered. This is the whole command surface; add new commands here.
- **`loop()`** — must stay non-blocking. It pumps `updateAxis()` for tilt and pan, `rtttl::play()`, and `client.loop()` every iteration. Anything that busy-waits here stalls MQTT, OTA, and motion at once. (`setup()` deliberately busy-waits on `rtttl::done()`, which is fine only because nothing else is running yet.)

Key invariants:

- **Each of pan and tilt is in one of two modes.** *Position mode* is AccelStepper's planner (`move`/`moveTo`), used by the D-pad nudges and `tilt/absolute`. *Velocity mode* is how the joystick and autoaim drive: `sgmk2/*/velocity` sets a target speed, `updateAxis()` ramps `currentVel` toward it at `PAN_ACCEL`/`TILT_ACCEL` and steps with `runSpeed()`. Streaming position deltas at joystick rate does **not** work — the target outruns the achievable speed and the axis lags then overshoots. Switch modes via `commandVelocity()` / `commandPosition()`; never call `move()` without `commandPosition()` first, or the stale velocity ramp fights the planner.
- **Velocity mode fails safe.** No `*/velocity` message for `VEL_TIMEOUT_MS` (500 ms) ramps that axis to a stop. Pan travel is unbounded, so a dropped packet or a closed browser must never leave the turret running. Any new streaming command path needs the same treatment.
- **Tilt is clamped to `TILTMIN`..`TILTMAX`** in the absolute and relative handlers, and `tiltLimitVelocity()` brakes velocity mode across `TILT_SLOW_BAND` steps before each limit. Pan is unbounded (continuous rotation). Keep any new tilt path clamped.
- **Homing runs at `HOMING_SPEED`**, not the raised `TILT_ACCEL`/max speed, so the carriage doesn't slam the endstop. There is no encoder: a skipped tilt step silently corrupts the homed origin and therefore the limits, recoverable only by rebooting to re-home. Tune speeds upward from conservative values.
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
| `sgmk2/pan/velocity` | signed steps/s | target pan speed; ramped, stops after `VEL_TIMEOUT_MS` |
| `sgmk2/tilt/velocity` | signed steps/s | target tilt speed; ramped, brakes into the tilt limits |
| `sgmk2/flywheel` | `on` / anything | spin up / spin down (also stops shooting) |
| `sgmk2/shoot` | `on` / anything | start / stop firing (needs flywheels on) |
| `sgmk2/laser` | `on` / anything | laser pointer |
| `sgmk2/rof` | int rounds/min | rate of fire |
| `sgmk2/alert` | any | play the alert tune on the buzzer |

Payload convention: only the exact string `on` enables; every other payload is treated as off.

## CV app (`cv/`)

`sgmk2-cv.py` is the Flask/socket.io app, `sgmk2.py` wraps the MQTT client, `input_shaping.py` turns the raw stick into steps/s (pure functions). The browser sends gamepad state over socket.io (`static/gamepad.js`).

- **`motion_sender()` is the only publisher of `*/velocity`.** It runs on its own thread at `SEND_HZ`, arbitrates stick vs. autoaim (the stick always wins), publishes on change plus a keepalive inside the firmware's `VEL_TIMEOUT_MS`. Socket handlers and the frame loop only record state — keep detection time out of the control path.
- **Autoaim** steers proportionally toward the largest face (`AIM_KP_*`, `AIM_DEADBAND_PX`). A socket disconnect zeroes the stick and disengages autoaim.
- **Config from env**, defaults = the home setup: `MQTT_BROKER`, `MQTT_PORT`, `MQTT_CLIENT_ID`, `CAMERA_URL`, `PORT`, `SECRET_KEY` (random if unset).
- **SIGTERM/SIGINT fail-safe:** `shutdown()` zeroes inputs, then `SGMk2.safe_stop()` publishes `shoot off`, `flywheel off` and zero pan/tilt velocity before exiting. `sgmk2/shoot` has no firmware timeout, so any new way for the app to exit must go through this.
- The face cascade comes from `cv2.data.haarcascades` (shipped in the wheel). The image is `python:3.10-slim` + `opencv-python-headless`, running as uid 1000.

Check it with `python -m py_compile cv/*.py`; run it with `python cv/sgmk2-cv.py` (needs the deps from `cv/requirements.txt`).

## Chart and CI

- `chart/` is the Helm chart `sgmk2-cv`, deployed straight from the repo (not published). Fixed one replica + `Recreate` because the MQTT client id is fixed. Ingress is off by default — the UI has no auth and can fire the turret. Values reference and the ArgoCD Application snippet are in `chart/README.md`; the Application itself lives in the `cloud` repo and pins both `targetRevision` and `image.tag: sha-<commit>`.
- `.github/workflows/cv-image.yml` builds `cv/` and pushes `ghcr.io/jserrats/sgmk2/cv` (`sha-<short>`, `latest` on `main`, semver on `v*` tags); PRs build without pushing.
- `.github/workflows/lint-chart.yml` runs `helm lint --strict` and `helm template` with the default values and `chart/ci/all-features-values.yaml`. Add any new optional block to that file.

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
