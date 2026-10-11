"""Gamepad stick conditioning.

Pure functions with no I/O, so they can be exercised without a turret or a pad.

The turret is a velocity-controlled device: the stick says how fast to move, not
how far. These helpers turn a raw stick vector into steps/s for each axis.
"""

import math

# Radial, applied to the (x, y) magnitude rather than per-axis. 0.15 is what the
# previous per-axis drift filter used, so it is known to cover this pad's drift.
DEADZONE = 0.15
EXPO = 0.6  # 0 = linear, 1 = fully cubic; higher means finer control near centre

# Start conservative and raise these once the mechanics prove they keep up.
# Tilt only has ~350 steps of travel end to end, so 120 steps/s is a ~3 s full sweep.
PAN_MAX_SPS = 400.0
TILT_MAX_SPS = 120.0


def is_active(x, y):
    """True when the stick is deflected past the deadzone."""
    return math.hypot(x, y) >= DEADZONE


def shape(x, y):
    """Raw stick (x, y) in [-1, 1] -> (pan_sps, tilt_sps) in steps/s.

    The deadzone is radial so that pushing straight up doesn't leak into pan, and
    the response is rescaled from the deadzone edge so speed ramps from zero
    instead of jumping the moment the stick crosses the threshold.
    """
    magnitude = math.hypot(x, y)
    if magnitude < DEADZONE:
        return 0.0, 0.0

    # Rescale [DEADZONE, 1] -> [0, 1] so there is no step at the deadzone edge.
    scaled = min(1.0, (magnitude - DEADZONE) / (1.0 - DEADZONE))
    curved = EXPO * scaled**3 + (1.0 - EXPO) * scaled

    # Both axes are inverted: pushing the stick right/up pans/tilts toward the target.
    unit_x, unit_y = x / magnitude, y / magnitude
    return -unit_x * curved * PAN_MAX_SPS, -unit_y * curved * TILT_MAX_SPS
