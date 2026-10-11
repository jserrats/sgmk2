const haveEvents = "ongamepadconnected" in window;
const controllers = {};

var socket = io();

// Only emit when something actually changed. The server resamples at a fixed rate
// and the firmware ramps between commands, so blasting the same values 50x/s only
// added queueing latency and jittered the turret's step timing.
const AXIS_EPSILON = 0.01;

let lastAxes = [0, 0];
let lastButtons = [];

if (navigator.getGamepads) {
  // Add an event listener for gamepad connection/disconnection events
  window.addEventListener("gamepadconnected", function (event) {
    console.log(
      "Gamepad connected at index %d: %s. %d buttons, %d axis.",
      event.gamepad.index,
      event.gamepad.id,
      event.gamepad.buttons.length,
      event.gamepad.axes.length
    );
  });
  window.addEventListener("gamepaddisconnected", function (event) {
    console.log(
      "Gamepad disconnected from index %d: %s",
      event.gamepad.index,
      event.gamepad.id
    );
    releaseStick();
  });
}

function releaseStick() {
  lastAxes = [0, 0];
  socket.emit("axis", { data: lastAxes });
}

function polling() {
  const gamepad = navigator.getGamepads()[0];

  if (gamepad) {
    const buttons = Array.from(gamepad.buttons, (button) => button.value);
    if (
      buttons.length !== lastButtons.length ||
      buttons.some((value, i) => value !== lastButtons[i])
    ) {
      socket.emit("buttons", { data: buttons });
      lastButtons = buttons;
    }

    // Compare against the last value we *sent*, not the last sampled, so a slow
    // push that moves less than the epsilon per frame still gets through.
    const axes = [gamepad.axes[0], gamepad.axes[1]];
    if (
      Math.abs(axes[0] - lastAxes[0]) > AXIS_EPSILON ||
      Math.abs(axes[1] - lastAxes[1]) > AXIS_EPSILON
    ) {
      socket.emit("axis", { data: axes });
      lastAxes = axes;
    }
  }

  requestAnimationFrame(polling);
}

// requestAnimationFrame stops firing while the tab is hidden, which would freeze the
// stick at whatever it was last holding. Zero it on the way out.
document.addEventListener("visibilitychange", function () {
  if (document.hidden) {
    releaseStick();
  }
});

requestAnimationFrame(polling);
