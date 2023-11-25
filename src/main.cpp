#include <AccelStepper.h>
#include <MultiStepper.h>
#include <ArduinoOTA.h>
#include <NonBlockingRtttl.h>

#include <WiFi.h>
#include "AsyncUDP.h"
#include "secrets.h"

#define motorInterfaceType 1

#define BUZZER_PIN 14
#define ENDSTOP_PIN 34 // other to ground
#define FLYWHEELS_PIN 32
#define LASER_PIN 33

#define TILTMIN 0
#define TILTMAX 480
#define TILT_CALIBRATION false

#define SHOOT_ORDER 0
#define TILT_ABS_ORDER 1
#define TILT_REL_ORDER 2
#define PAN_ORDER 3
#define FLYWHEELS_ORDER 4
#define LASER_ORDER 5
#define ROF_ORDER 6

// step  -- dir
AccelStepper stepperPusher(motorInterfaceType, 19, 18);
AccelStepper stepperTilt(motorInterfaceType, 5, 17);
AccelStepper stepperPan(motorInterfaceType, 16, 4);

const char *bootup = "bootup:d=16,o=6,b=95:32d,32d#,32e,32f,32f#,32g,g";
bool shooting = false;
AsyncUDP udp;

void setRateOfFire(int rof)
{
  int stepsPerSecond = rof * 200 / 60;
  if (stepsPerSecond > 950)
  {
    stepsPerSecond = 950;
  }
  stepperPusher.setSpeed(-1 * stepsPerSecond);
}

void setup(void)
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
  // stepper initialization
  Serial.println("Setting up steppers...");
  stepperPusher.setMaxSpeed(1500);
  setRateOfFire(60);
  stepperTilt.setMaxSpeed(1000);
  stepperTilt.setAcceleration(200);
  stepperTilt.setSpeed(200);
  stepperPan.setMaxSpeed(1000);
  stepperPan.setAcceleration(200);
  stepperPan.setSpeed(200);

  // WiFi stuff
  Serial.println("Initializing WiFi...");
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("sgmk2.lan");
  WiFi.begin(ssid, password);
  if (WiFi.waitForConnectResult() != WL_CONNECTED)
  {
    Serial.println("WiFi Failed");
    while (1)
    {
      delay(1000);
    }
  }
  Serial.println("WiFi successfully connected");

  ArduinoOTA
      .onStart([]()
               {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH)
      type = "sketch";
    else // U_SPIFFS
      type = "filesystem";

    // NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
    Serial.println("Start updating " + type); })
      .onEnd([]()
             { Serial.println("\nEnd"); })
      .onProgress([](unsigned int progress, unsigned int total)
                  { Serial.printf("Progress: %u%%\r", (progress / (total / 100))); })
      .onError([](ota_error_t error)
               {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR)
      Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR)
      Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR)
      Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR)
      Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR)
      Serial.println("End Failed"); });

  ArduinoOTA.begin();

  if (udp.listen(1234))
  {
    Serial.print("UDP server on IP: ");
    Serial.println(WiFi.localIP());
    udp.onPacket([](AsyncUDPPacket packet)
                 {
      uint8_t order = *(packet.data());
      uint8_t parameter0 = *(packet.data() + 1);
      uint8_t parameter1 = *(packet.data() + 2);
      int16_t parameter = ((parameter0 << 8) | parameter1 )- 32768;

      if (order == SHOOT_ORDER) // shoot
      {
        //stepperPusher.move(-200);
        if (parameter == 255)
        {
          shooting = true;
        }
        else
        {
          shooting = false;
        }
        Serial.print("SHOOTING: ");
        Serial.println(shooting);
      }
      // tilt absolute
      else if (order == TILT_ABS_ORDER && (parameter >= TILTMIN) && (parameter <= TILTMAX)) 
      {
        stepperTilt.moveTo(parameter);
        Serial.print("TILT ABS: ");
        Serial.println(parameter);
      }
      // tilt relative
      else if (order == TILT_REL_ORDER)
      {
        if(stepperTilt.currentPosition() + parameter < TILTMIN){
          stepperTilt.moveTo(TILTMIN);
        }
        else if (stepperTilt.currentPosition() + parameter > TILTMAX) {
          stepperTilt.moveTo(TILTMAX);
        }
        else {
          stepperTilt.move(parameter);
        }
        Serial.print("TILT REL: ");
        Serial.println(parameter);
      }
      else if (order == PAN_ORDER) // pan
      {
        stepperPan.move(parameter);
        Serial.print("PAN REL: ");
        Serial.println(parameter);
      }
      else if (order == FLYWHEELS_ORDER) // FLYWHEELS_PIN
      {
        if (parameter == 255)
        {
          // analogWrite(FLYWHEELS_PIN, 120);
          digitalWrite(FLYWHEELS_PIN, HIGH);

        }
        else
        {
          digitalWrite(FLYWHEELS_PIN, LOW);
        }
      }
      else if (order == LASER_ORDER) // LASER_PIN 
      {
        if (parameter == 255)
        {
          digitalWrite(LASER_PIN , HIGH);
        }
        else
        {
          digitalWrite(LASER_PIN , LOW);
        }
      } 
      else if (order == ROF_ORDER)
      {
        setRateOfFire(parameter);
        Serial.print("ROF:");
        Serial.println(parameter);
      } });
    digitalWrite(LED_BUILTIN, HIGH);
  }

  // tilt stepper calibration
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
    stepperTilt.moveTo(250);
    Serial.println("Tilt offset calibrated");
  }

  Serial.println("\nAll systems operational");
}

int stepsShot = 0;

void loop()
{
  if (shooting || stepsShot % 100 != 0)
  {
    if (stepperPusher.runSpeed())
    {
      stepsShot++;
    };
    Serial.println("step");
  }

  stepperTilt.run();
  stepperPan.run();

  ArduinoOTA.handle();
}