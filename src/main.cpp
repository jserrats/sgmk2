#include <AccelStepper.h>
#include <MultiStepper.h>
#include <ArduinoOTA.h>
#include <NonBlockingRtttl.h>

// #include <WiFi.h>
// #include "AsyncUDP.h"
#include "secrets.h"

#include "EspMQTTClient.h"

#define motorInterfaceType 1

#define BUZZER_PIN 14
#define ENDSTOP_PIN 34 // other to ground
#define FLYWHEELS_PIN 32
#define LASER_PIN 33

#define TILTMIN -120
#define TILTMAX 230
#define TILT_CALIBRATION true

#define SHOOT_ORDER 0
#define TILT_ABS_ORDER 1
#define TILT_REL_ORDER 2
#define PAN_ORDER 3
#define FLYWHEELS_ORDER 4
#define LASER_ORDER 5
#define ROF_ORDER 6

EspMQTTClient client(
    ssid, password,
    "mqtt.jserrats.xyz", // MQTT Broker server ip
    "sgmk2",             // Client name that uniquely identify your device
    1883                 // The MQTT port, default to 1883. this line can be omitted
);

const char *bootup = "bootup:d=16,o=6,b=95:32d,32d#,32e,32f,32f#,32g,g";
const char *ready = "ready:d=16,o=6,b=95:g,f#";

// AccelStepper stepperPusher(motorInterfaceType, 19, 18);
AccelStepper stepperTilt(motorInterfaceType, 5, 17);
AccelStepper stepperPan(motorInterfaceType, 16, 4);

void setup()
{
  Serial.begin(115200);

  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(ENDSTOP_PIN, INPUT_PULLDOWN);
  pinMode(FLYWHEELS_PIN, OUTPUT);
  pinMode(LASER_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  rtttl::begin(BUZZER_PIN, bootup);
  while (!rtttl::done())
  {
    rtttl::play();
  }

  stepperPan.setMaxSpeed(1000);
  stepperPan.setAcceleration(200);
  stepperPan.setSpeed(200);
  stepperTilt.setMaxSpeed(1000);
  stepperTilt.setAcceleration(200);
  stepperTilt.setSpeed(200);

  client.enableDebuggingMessages(); // Enable debugging messages sent to serial output
  client.enableOTA();
  client.enableLastWillMessage("sgmk2/status", "offline", true);

  if (TILT_CALIBRATION == true)
  {
    stepperTilt.move(-600);
    Serial.println("Calibrating Tilt offset...");
    while (digitalRead(ENDSTOP_PIN) == 0)
    {
      stepperTilt.run();
    }
    stepperTilt.stop();
    stepperTilt.setCurrentPosition(0);
    Serial.println("Tilt offset calibrated");
  }
  rtttl::begin(BUZZER_PIN, ready);

  while (!rtttl::done())
  {
    rtttl::play();
  }
}

void onConnectionEstablished()
{
  client.subscribe("sgmk2/pan", [](const String &payload)
                   { int16_t parameter = payload.toInt();
                   stepperPan.move(parameter); });
  client.subscribe("sgmk2/tilt/absolute", [](const String &payload)
                   { int16_t parameter = payload.toInt();
                   if((parameter >= TILTMIN) && (parameter <= TILTMAX)){
                      stepperTilt.moveTo(parameter);
                   } });
  client.subscribe("sgmk2/flywheel", [](const String &payload)
                   { 
                    if(payload == "on"){
                      digitalWrite(FLYWHEELS_PIN, HIGH);
                    }else{
                      digitalWrite(FLYWHEELS_PIN, LOW);
                    } });
  client.publish("sgmk2/status", "online", true); // You can activate the retain flag by setting the third parameter to true

  digitalWrite(LED_BUILTIN, HIGH);
}

void loop()
{
  stepperTilt.run();
  stepperPan.run();
  client.loop();
}