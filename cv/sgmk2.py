import os

from paho.mqtt import client as mqtt_client

broker = os.environ.get("MQTT_BROKER", "mqtt.jserrats.xyz")
port = int(os.environ.get("MQTT_PORT", "1883"))
# Fixed, so only one controller may run at a time: a second one with the same id
# kicks the first off the broker.
client_id = os.environ.get("MQTT_CLIENT_ID", "sgmk2-controller")


class SGMk2:
    _rof_values = [60, 200, 400]

    def __init__(self):
        self.client = self.connect_mqtt()
        self.laser = False
        self.rof = 0
        self.autoaim = False

    def alert(self):
        self.client.publish("sgmk2/alert", "1")

    def get_rof_value(self):
        return self._rof_values[self.rof]

    def connect_mqtt(self):
        def on_connect(client, userdata, flags, rc):
            if rc == 0:
                client.publish("sgmk2-cv/status", "online", retain=True)

        client = mqtt_client.Client(client_id)
        client.on_connect = on_connect
        client.on_message = self.on_message
        client.will_set("sgmk2-cv/status", "offline", retain=True)
        client.connect(broker, port)
        client.subscribe("sgmk2/status")
        client.loop_start()

        return client

    def on_message(self, client, userdata, msg):
        # The turret republishes a retained "online" on every boot, at which point it
        # is back to firmware defaults and our cached state is stale. "offline" tells
        # us nothing about its state, so don't reset on it.
        if msg.payload.decode() == "online":
            self.rof = 0
            self.autoaim = False
            self.laser = False

    def flywheel(self, value):
        if value:
            self.client.publish("sgmk2/flywheel", "on")
        else:
            self.client.publish("sgmk2/flywheel", "off")

    def shooting(self, value):
        if value:
            self.client.publish("sgmk2/shoot", "on")
        else:
            self.client.publish("sgmk2/shoot", "off")

    def pan_rel(self, value):
        self.client.publish("sgmk2/pan/relative", value)

    def tilt_rel(self, value):
        self.client.publish("sgmk2/tilt/relative", value)

    # Velocity commands are how the joystick and autoaim drive the turret: the
    # firmware ramps between them, so these can be sent at a modest rate. Round
    # rather than truncate, and always send an integer string -- the firmware parses
    # these and a value like "0.7" would otherwise be lost.
    def pan_vel(self, steps_per_second):
        self.client.publish("sgmk2/pan/velocity", str(int(round(steps_per_second))))

    def tilt_vel(self, steps_per_second):
        self.client.publish("sgmk2/tilt/velocity", str(int(round(steps_per_second))))

    def _set_rof(self, value):
        self.client.publish("sgmk2/rof", value)

    def switch_rof(self):
        self.rof = (self.rof + 1) % 3
        self._set_rof(self._rof_values[self.rof])

    def safe_stop(self):
        """Leave the turret inert: trigger released, flywheels down, axes stopped.

        The velocity topics would time out on their own, but sgmk2/shoot has no
        firmware timeout -- without this, a controller that dies with the trigger
        held leaves the turret firing.
        """
        infos = [
            self.client.publish("sgmk2/shoot", "off", qos=1),
            self.client.publish("sgmk2/flywheel", "off", qos=1),
            self.client.publish("sgmk2/pan/velocity", "0", qos=1),
            self.client.publish("sgmk2/tilt/velocity", "0", qos=1),
        ]
        for info in infos:
            try:
                info.wait_for_publish(timeout=1)
            except (RuntimeError, ValueError):
                pass  # not connected; nothing more we can do

    def toggle_laser(self):
        self.laser = not self.laser
        self.client.publish("sgmk2/laser", "on" if self.laser else "off")
