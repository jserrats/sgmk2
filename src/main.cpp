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
#define STEPS_PER_SHOT 200

// Velocity mode: the controller streams a target speed and the ramp lives here,
// so motion stays smooth between commands instead of re-planning a move each time.
#define PAN_ACCEL 2000.0    // steps/s^2
#define TILT_ACCEL 1500.0   // steps/s^2
#define VEL_TIMEOUT_MS 500  // no velocity command for this long -> ramp to a stop
#define TILT_SLOW_BAND 40   // steps before a tilt limit where we start braking
#define MAX_MOTION_DT 0.05  // s; ignore longer gaps so a stall can't jump the ramp
#define HOMING_SPEED 300    // steps/s while seeking the tilt endstop

EspMQTTClient client(
    ssid, password,
    "mqtt.jserrats.xyz", // MQTT Broker server ip
    "sgmk2",             // Client name that uniquely identify your device
    1883                 // The MQTT port, default to 1883. this line can be omitted
);

const char *bootup = "bootup:d=16,o=6,b=95:32d,32d#,32e,32f,32f#,32g,g";
const char *ready = "ready:d=16,o=6,b=95:g,f#";
const char *autoaim = "ready:d=16,o=6,b=95:f#,g";

AccelStepper stepperPusher(motorInterfaceType, 19, 18);
AccelStepper stepperTilt(motorInterfaceType, 5, 17);
AccelStepper stepperPan(motorInterfaceType, 16, 4);

bool shooting = false;

// Brake tilt as it approaches its travel limits instead of stopping dead on them.
float tiltLimitVelocity(float desired)
{
  if (desired == 0.0)
  {
    return 0.0;
  }

  long position = stepperTilt.currentPosition();
  long room = (desired < 0) ? (position - TILTMIN) : (TILTMAX - position);
  if (room <= 0)
  {
    return 0.0;
  }
  if (room < TILT_SLOW_BAND)
  {
    return desired * ((float)room / TILT_SLOW_BAND);
  }
  return desired;
}

// An axis is either in AccelStepper's position mode (move/moveTo, used by the D-pad
// nudges and tilt/absolute) or in velocity mode, where we ramp the speed ourselves
// and step with runSpeed().
struct VelocityAxis
{
  AccelStepper &stepper;
  float accel;                   // steps/s^2 for the ramp
  float (*limitVelocity)(float); // NULL when travel is unbounded
  float targetVel;               // steps/s, last commanded
  float currentVel;              // steps/s, ramped
  uint32_t lastCommandMs;
  bool velocityMode;
};

VelocityAxis panAxis = {stepperPan, PAN_ACCEL, NULL, 0.0, 0.0, 0, false};
VelocityAxis tiltAxis = {stepperTilt, TILT_ACCEL, tiltLimitVelocity, 0.0, 0.0, 0, false};

uint32_t lastMotionUpdateUs = 0;

void commandVelocity(VelocityAxis &axis, float stepsPerSecond)
{
  axis.targetVel = stepsPerSecond;
  axis.lastCommandMs = millis();
  axis.velocityMode = true;
}

// Hand an axis back to AccelStepper's planner from a known-stopped state.
// setCurrentPosition() keeps the position but clears the internal ramp counter and
// speed, so the next move() plans a clean profile.
void commandPosition(VelocityAxis &axis)
{
  axis.velocityMode = false;
  axis.targetVel = 0.0;
  axis.currentVel = 0.0;
  axis.stepper.setCurrentPosition(axis.stepper.currentPosition());
}

void updateAxis(VelocityAxis &axis, float dt)
{
  if (!axis.velocityMode)
  {
    axis.stepper.run();
    return;
  }

  // Failsafe: pan travel is unbounded, so a dropped command or a closed browser
  // must not leave the turret running.
  float target = axis.targetVel;
  if (millis() - axis.lastCommandMs > VEL_TIMEOUT_MS)
  {
    target = 0.0;
  }
  if (axis.limitVelocity != NULL)
  {
    target = axis.limitVelocity(target);
  }

  float maxDelta = axis.accel * dt;
  float error = target - axis.currentVel;
  axis.currentVel += constrain(error, -maxDelta, maxDelta);

  // setSpeed() clamps to setMaxSpeed() and leaves the step timer alone, so calling
  // it every iteration with a changing value is safe.
  axis.stepper.setSpeed(axis.currentVel);
  axis.stepper.runSpeed();
}

void setRateOfFire(int rof)
{
  int stepsPerSecond = rof * STEPS_PER_SHOT / 60;
  if (stepsPerSecond > 950)
  {
    stepsPerSecond = 950;
  }
  stepperPusher.setSpeed(-1 * stepsPerSecond);
}

void setup()
{
  Serial.begin(115200);
  Serial.println("\n[+] SGMK2 [+]");

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
  stepperPusher.setMaxSpeed(1500);
  setRateOfFire(60);
  stepperPan.setMaxSpeed(1000);
  stepperPan.setAcceleration(PAN_ACCEL);
  stepperPan.setSpeed(0);
  stepperTilt.setMaxSpeed(1000);
  stepperTilt.setAcceleration(TILT_ACCEL);
  stepperTilt.setSpeed(0);

  client.enableDebuggingMessages(); // Enable debugging messages sent to serial output
  client.enableOTA();
  client.enableLastWillMessage("sgmk2/status", "offline", true);

  if (TILT_CALIBRATION == true)
  {
    // Home slowly: the raised acceleration would otherwise drive the carriage into
    // the physical endstop about three times faster than before.
    stepperTilt.setMaxSpeed(HOMING_SPEED);
    stepperTilt.move(-600);
    Serial.println("Calibrating Tilt offset...");
    while (digitalRead(ENDSTOP_PIN) == 0)
    {
      stepperTilt.run();
    }
    stepperTilt.stop();
    stepperTilt.setCurrentPosition(0);
    stepperTilt.setMaxSpeed(1000);
    Serial.println("Tilt offset calibrated");
  }
  rtttl::begin(BUZZER_PIN, ready);

  while (!rtttl::done())
  {
    rtttl::play();
  }

  lastMotionUpdateUs = micros();
}

void onConnectionEstablished()
{
  client.subscribe("sgmk2/pan/relative", [](const String &payload)
                   { int16_t parameter = payload.toInt();
                   commandPosition(panAxis);
                   stepperPan.move(parameter); });
  client.subscribe("sgmk2/tilt/absolute", [](const String &payload)
                   { int16_t parameter = payload.toInt();
                   if((parameter >= TILTMIN) && (parameter <= TILTMAX)){
                      commandPosition(tiltAxis);
                      stepperTilt.moveTo(parameter);
                   } });
  client.subscribe("sgmk2/tilt/relative", [](const String &payload)
                   { int16_t parameter = payload.toInt();
                   commandPosition(tiltAxis);
                   if(stepperTilt.currentPosition() + parameter < TILTMIN){
                      stepperTilt.moveTo(TILTMIN);
                    }
                    else if (stepperTilt.currentPosition() + parameter > TILTMAX) {
                      stepperTilt.moveTo(TILTMAX);
                    }
                    else {
                      stepperTilt.move(parameter);
                   } });
  client.subscribe("sgmk2/pan/velocity", [](const String &payload)
                   { commandVelocity(panAxis, payload.toFloat()); });
  client.subscribe("sgmk2/tilt/velocity", [](const String &payload)
                   { commandVelocity(tiltAxis, payload.toFloat()); });
  client.subscribe("sgmk2/flywheel", [](const String &payload)
                   { 
                    if(payload == "on"){
                      digitalWrite(FLYWHEELS_PIN, HIGH);
                    }else{
                      digitalWrite(FLYWHEELS_PIN, LOW);
                      shooting = false;
                    } });
  client.subscribe("sgmk2/shoot", [](const String &payload)
                   { 
                    if(payload == "on" && digitalRead(FLYWHEELS_PIN)){
                     shooting = true;
                    }else{
                     shooting = false;
                   } });
  client.subscribe("sgmk2/laser", [](const String &payload)
                   { 
                    if(payload == "on"){
                     digitalWrite(LASER_PIN, HIGH);
                    }else{
                     digitalWrite(LASER_PIN, LOW);
                   } });
  client.subscribe("sgmk2/rof", [](const String &payload)
                   {
                     int16_t parameter = payload.toInt();
                     setRateOfFire(parameter); });
  client.subscribe("sgmk2/alert", [](const String &payload)
                   { rtttl::begin(BUZZER_PIN, ready); });
                   
  client.publish("sgmk2/status", "online", true); // You can activate the retain flag by setting the third parameter to true
  client.publish("sgmk2/tilt/absolute", "0");
  digitalWrite(LED_BUILTIN, HIGH);
}

int stepsShot = 0;

void loop()
{
  if (shooting || stepsShot % STEPS_PER_SHOT != 0)
  {
    if (stepperPusher.runSpeed())
    {
      stepsShot++;
    };
  }
  rtttl::play();

  uint32_t nowUs = micros();
  float dt = (nowUs - lastMotionUpdateUs) * 1e-6;
  lastMotionUpdateUs = nowUs;
  if (dt > MAX_MOTION_DT)
  {
    dt = MAX_MOTION_DT;
  }
  updateAxis(tiltAxis, dt);
  updateAxis(panAxis, dt);

  client.loop();
}