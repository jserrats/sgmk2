import os
import signal
import threading
import time

from flask import Flask, render_template, Response
import cv2
from flask_socketio import SocketIO

import input_shaping
from sgmk2 import SGMk2

app = Flask(__name__)
# No sessions outlive the process, so a random key per start is fine.
app.config["SECRET_KEY"] = os.environ.get("SECRET_KEY") or os.urandom(32)
socketio = SocketIO(app)

CAMERA_URL = os.environ.get("CAMERA_URL", "http://sgmk2-cam.iot:8080/")
PORT = int(os.environ.get("PORT", "5000"))

# The cascade ships with the opencv-python wheel.
face_cascade = cv2.CascadeClassifier(
    cv2.data.haarcascades + "haarcascade_frontalface_default.xml"
)
font = cv2.FONT_HERSHEY_SIMPLEX

sgmk2 = SGMk2()

gamepad_buttons_cache = [0 for i in range(17)]

# Motion command rate. The firmware ramps between commands and stops on its own
# after 500 ms of silence, so we only need to publish on change plus a keepalive
# comfortably inside that window.
SEND_HZ = 20
KEEPALIVE_S = 0.2

# Autoaim proportional gains, in steps/s per pixel of error, plus the deadband that
# stops it hunting around a target it has already centred.
AIM_KP_PAN = 1.2
AIM_KP_TILT = 0.5
AIM_DEADBAND_PX = 12

# Shared between the socket handlers, the frame loop and the sender thread.
_state_lock = threading.Lock()
manual_axes = (0.0, 0.0)  # raw stick, as reported by the browser
autoaim_command = (0.0, 0.0)  # steps/s, from the frame loop


def set_manual_axes(x, y):
    global manual_axes
    with _state_lock:
        manual_axes = (x, y)


def set_autoaim_command(pan_sps, tilt_sps):
    global autoaim_command
    with _state_lock:
        autoaim_command = (pan_sps, tilt_sps)


def clamp(value, limit):
    return max(-limit, min(limit, value))


def motion_sender():
    """Sole publisher of the velocity topics.

    Runs at a fixed rate in its own thread so that command latency never depends on
    how long a frame of face detection took, and so the joystick and autoaim can't
    fight over an axis -- exactly one of them wins here, every tick.
    """
    publish = (sgmk2.pan_vel, sgmk2.tilt_vel)
    last_sent = [None, None]
    last_sent_at = [0.0, 0.0]
    period = 1.0 / SEND_HZ
    next_tick = time.monotonic()

    while True:
        with _state_lock:
            stick_x, stick_y = manual_axes
            aim = autoaim_command

        if input_shaping.is_active(stick_x, stick_y):
            command = input_shaping.shape(stick_x, stick_y)  # manual always wins
        elif sgmk2.autoaim:
            command = aim
        else:
            command = (0.0, 0.0)

        now = time.monotonic()
        for axis in (0, 1):
            value = int(round(command[axis]))
            stale = now - last_sent_at[axis] >= KEEPALIVE_S
            # A zero only needs sending once: if it is lost, the firmware's own
            # timeout stops the axis anyway.
            if value != last_sent[axis] or (value != 0 and stale):
                publish[axis](value)
                last_sent[axis] = value
                last_sent_at[axis] = now

        next_tick += period
        time.sleep(max(0.0, next_tick - time.monotonic()))


def track_faces(img, faces, image_center):
    """Draw the detections and steer autoaim toward one of them."""
    center_x, center_y = image_center

    for x, y, w, h in faces:
        on_target = x < center_x < x + w and y < center_y < y + h
        box_color = (0, 0, 255) if on_target else (0, 255, 0)
        cv2.rectangle(img, (x, y), (x + w, y + h), box_color, 2)
        cv2.line(img, image_center, (x + (w // 2), y + (h // 2)), box_color, 5)

    if not len(faces):
        set_autoaim_command(0.0, 0.0)
        return

    # One target per frame. Aiming at every detection at once just made them fight
    # for the axis; the largest face is the closest one.
    x, y, w, h = max(faces, key=lambda face: face[2] * face[3])
    error_x = center_x - (x + (w // 2))
    error_y = center_y - (y + (h // 2))

    set_autoaim_command(
        0.0
        if abs(error_x) < AIM_DEADBAND_PX
        else clamp(AIM_KP_PAN * error_x, input_shaping.PAN_MAX_SPS),
        0.0
        if abs(error_y) < AIM_DEADBAND_PX
        else clamp(AIM_KP_TILT * error_y, input_shaping.TILT_MAX_SPS),
    )


def gen_frames():
    camera = cv2.VideoCapture(CAMERA_URL)
    try:
        while 1:
            success, img = camera.read()
            if not success:
                print("Camera Offline")
                set_autoaim_command(0.0, 0.0)
                break

            # convert to gray scale of each frames
            gray = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY)

            if sgmk2.autoaim:
                color = (0, 0, 255)  # red
            else:
                color = (0, 255, 0)  # green
            # Detects faces of different sizes in the input image
            faces = face_cascade.detectMultiScale(gray, 1.3, 5)
            (image_height, image_width) = img.shape[:2]
            image_center = (image_width // 2, image_height // 2)
            cv2.circle(img, image_center, 7, (0, 0, 255), -1)
            cv2.putText(
                img,
                f"AIMBOT: {sgmk2.autoaim}",
                (10, 450),
                font,
                1,
                color,
                2,
                cv2.LINE_AA,
            )
            cv2.putText(
                img,
                f"ROF: {sgmk2.get_rof_value()}",
                (400, 450),
                font,
                1,
                color,
                2,
                cv2.LINE_AA,
            )

            track_faces(img, faces, image_center)

            # Display an image in a window
            ret, buffer = cv2.imencode(".jpg", img)
            frame = buffer.tobytes()
            yield (
                b"--frame\r\n" b"Content-Type: image/jpeg\r\n\r\n" + frame + b"\r\n"
            )  # concat frame one by one and show result
    finally:
        camera.release()
        set_autoaim_command(0.0, 0.0)


@app.route("/video_feed")
def video_feed():
    return Response(gen_frames(), mimetype="multipart/x-mixed-replace; boundary=frame")


@app.route("/")
def index():
    """Video streaming home page."""
    return render_template("index.html")


@socketio.on("axis")
def axis(json):
    # Just record the stick; motion_sender owns the publishing. Keeping this handler
    # trivial is what stops frame-detection time leaking into control latency.
    x_axis, y_axis = json["data"]
    set_manual_axes(float(x_axis), float(y_axis))


@socketio.on("disconnect")
def disconnect(*args):
    # No operator, no motion. Zeroing the stick alone isn't enough: autoaim would
    # happily keep tracking on its own, so disengage it too rather than waiting on
    # the video generator to be finalised.
    set_manual_axes(0.0, 0.0)
    sgmk2.autoaim = False
    set_autoaim_command(0.0, 0.0)


@socketio.on("buttons")
def buttons(json):
    data = json["data"]
    for button, state in enumerate(data):
        if gamepad_buttons_cache[button] != state:
            control(button, state)
            gamepad_buttons_cache[button] = state
            # print(button, state)


def control(button, state):
    if button == 6:
        sgmk2.flywheel(state == 1)
    if button == 7:
        sgmk2.shooting(state == 1)
    if button == 4 and state == 1:
        sgmk2.toggle_laser()
    if button == 5 and state == 1:
        sgmk2.switch_rof()
    if button == 14 and state == 1:
        sgmk2.pan_rel(1)
    if button == 15 and state == 1:
        sgmk2.pan_rel(-1)
    if button == 12 and state == 1:
        sgmk2.tilt_rel(1)
    if button == 13 and state == 1:
        sgmk2.tilt_rel(-1)
    if button == 0 and state == 1:
        sgmk2.autoaim = not sgmk2.autoaim
        if sgmk2.autoaim:
            sgmk2.alert()


def shutdown(signum, frame):
    # As PID 1 in a container, Python would otherwise ignore SIGTERM and every
    # rollout would wait out the grace period. Stop the turret on the way out:
    # sgmk2/shoot has no firmware timeout, so a trigger held at shutdown would
    # keep firing.
    print("Shutting down, stopping the turret")
    # Zero the inputs first so motion_sender can't publish a stale speed after
    # the stop below.
    set_manual_axes(0.0, 0.0)
    sgmk2.autoaim = False
    set_autoaim_command(0.0, 0.0)
    sgmk2.safe_stop()
    os._exit(0)


if __name__ == "__main__":
    signal.signal(signal.SIGTERM, shutdown)
    signal.signal(signal.SIGINT, shutdown)
    threading.Thread(target=motion_sender, daemon=True).start()
    socketio.run(app, host="0.0.0.0", port=PORT, allow_unsafe_werkzeug=True) # enabled because it simplifies concurrency and will not be exposed over internet
