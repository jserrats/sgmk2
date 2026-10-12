# SGMK2

Sentry Gun Mk2: a motorized foam-dart turret, controlled over MQTT.

| Directory | What |
| --- | --- |
| `firmware/` | PlatformIO/Arduino firmware for the turret's ESP32 (`pio run -d firmware`) |
| `cv/` | Web controller: camera feed, face-detection autoaim, gamepad over MQTT (see `cv/README.md`) |
| `chart/` | Helm chart that deploys `cv/` to Kubernetes, e.g. from ArgoCD (see `chart/README.md`) |

CI builds the `cv/` image to `ghcr.io/jserrats/sgmk2/cv` and lints the chart.

## Notes to myself

- To upload via OTA you need to be in the same LAN!
- esphome file for the camera is in esphome project
- `firmware/include/secrets.h` (gitignored) holds the WiFi credentials; the build fails without it
- In VS Code, open `firmware/` as the folder for the PlatformIO extension to pick it up
