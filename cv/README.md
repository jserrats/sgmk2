# SGMK2 - Computer Vision

The web controller: a Flask + socket.io app that serves the camera feed with face
detection drawn on it, reads a gamepad through the browser's Gamepad API, and drives
the turret over MQTT. Autoaim steers toward the largest detected face.

## Running

```
pip install -r requirements.txt
python sgmk2-cv.py            # http://127.0.0.1:5000/
```

Or the image CI publishes (`.github/workflows/cv-image.yml`):

```
docker run --rm -p 5000:5000 ghcr.io/jserrats/sgmk2/cv:latest
```

Configuration is from the environment; the defaults are the home setup.

| Variable | Default | Meaning |
| --- | --- | --- |
| `MQTT_BROKER` | `mqtt.jserrats.xyz` | broker host |
| `MQTT_PORT` | `1883` | broker port |
| `MQTT_CLIENT_ID` | `sgmk2-controller` | fixed, so only one controller may run at a time |
| `CAMERA_URL` | `http://sgmk2-cam.iot:8080/` | the ESPHome camera's MJPEG stream |
| `PORT` | `5000` | HTTP port |
| `SECRET_KEY` | random per start | Flask secret; nothing persists sessions, so random is fine |

On SIGTERM/SIGINT the app publishes `sgmk2/shoot off`, `sgmk2/flywheel off` and zero
pan/tilt velocity before exiting. The velocity topics time out in the firmware anyway,
but `sgmk2/shoot` does not, so this is what stops a controller killed with the trigger
held from leaving the turret firing.

The UI has no authentication and can fire the turret. Don't expose it to the internet.

## Controls

The joystick drives the turret with **velocity** commands (`sgmk2/{pan,tilt}/velocity`,
in steps/s), not position deltas. The firmware ramps between them, so this side only
has to say how fast to move and can do it at a modest rate. `motion_sender()` in
`sgmk2-cv.py` is the single publisher of those topics — it runs on its own fixed-rate
thread so that face-detection time never lands in the control path, and it arbitrates
between the stick and autoaim (the stick always wins).

The D-pad still uses the older `sgmk2/{pan,tilt}/relative` step commands.

Tuning knobs, all needing a bench pass on real hardware:

| Where | Constant | Meaning |
| --- | --- | --- |
| `input_shaping.py` | `DEADZONE` | radial stick deadzone; raise if the turret creeps at rest |
| `input_shaping.py` | `EXPO` | 0 = linear, 1 = fully cubic. Higher = finer control near centre |
| `input_shaping.py` | `PAN_MAX_SPS` / `TILT_MAX_SPS` | steps/s at full deflection. Deliberately conservative — raise until motion is fast enough but still silent and step-accurate |
| `sgmk2-cv.py` | `AIM_KP_PAN` / `AIM_KP_TILT` | autoaim gain, steps/s per pixel of error. Too high oscillates |
| `sgmk2-cv.py` | `AIM_DEADBAND_PX` | how close counts as on-target; too small makes autoaim hunt |

There is no encoder on either axis, so a skipped tilt step silently corrupts the
firmware's endstop-homed origin and its travel limits until the turret is rebooted.
Tune the maxima upward from safe values rather than down from aggressive ones.
