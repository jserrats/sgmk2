#include <AccelStepper.h>
#include <MultiStepper.h>
#include <ArduinoOTA.h>
#include <NonBlockingRtttl.h>

#include <WiFi.h>
#include "AsyncUDP.h"
#include "secrets.h"

#define motorInterfaceType 1

#define BUZZER 14
#define ENDSTOP 34 // other to ground
#define FLYWHEELS 32
#define LASER 33

#define TILTMIN 0
#define TILTMAX 480

int flywheelState = LOW;
int laserState = LOW;

// Creates an instance   step  -- dir
AccelStepper stepperPusher(motorInterfaceType, 23, 22);
AccelStepper stepperTilt(motorInterfaceType, 21, 19);
AccelStepper stepperPan(motorInterfaceType, 18, 5);

const char *bootup = "bootup:d=16,o=6,b=95:32d,32d#,32e,32f,32f#,32g,g";

AsyncUDP udp;

void setup(void)
{
  Serial.begin(115200);
  Serial.println("\n[+] SGMK2 [+]");
  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(ENDSTOP, INPUT_PULLDOWN);
  pinMode(FLYWHEELS, OUTPUT);
  pinMode(LASER, OUTPUT);
  pinMode(BUZZER, OUTPUT);

  rtttl::begin(BUZZER, bootup);
  while (!rtttl::done())
  {
    rtttl::play();
  }
  
  // stepper initialization
  Serial.println("Setting up steppers...");
  stepperPusher.setMaxSpeed(1000);
  stepperPusher.setAcceleration(200);
  stepperPusher.setSpeed(200);
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
      if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
      else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
      else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
      else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
      else if (error == OTA_END_ERROR) Serial.println("End Failed"); });

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

      if (order == 0) // shoot
      {
        stepperPusher.move(-400);
        Serial.print("order 0: ");
        Serial.println(parameter);
      }
      // tilt absolute
      else if (order == 1 && (parameter >= TILTMIN) && (parameter <= TILTMAX)) 
      {
        stepperTilt.moveTo(parameter);
        Serial.print("order 1: ");
        Serial.println(parameter);
      }
      // tilt relative
      else if (order == 2)
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
        Serial.print("order 1: ");
        Serial.println(parameter);
      }
      else if (order == 3) // pan
      {
        stepperPan.move(parameter);
        Serial.print("order 2: ");
        Serial.println(parameter);
      }
      else if (order == 4) // flywheels
      {
        if (parameter == 255)
        {
          // analogWrite(FLYWHEELS, 120);
          digitalWrite(FLYWHEELS, HIGH);

        }
        else
        {
          digitalWrite(FLYWHEELS, LOW);
        }
      }
      else if (order == 5) // laser
      {
        if (parameter == 255)
        {
          digitalWrite(LASER, HIGH);
        }
        else
        {
          digitalWrite(LASER, LOW);
        }
      } });
    digitalWrite(LED_BUILTIN, HIGH);
  }

  // tilt stepper calibration
  stepperTilt.move(-600);
  Serial.println("Calibrating Tilt offset...");
  while (digitalRead(ENDSTOP) == 0)
  {
    stepperTilt.run();
  }
  stepperTilt.stop();
  stepperTilt.setCurrentPosition(0);
  stepperTilt.moveTo(250);
  Serial.println("Tilt offset calibrated");

  Serial.println("\nAll systems operational");
}
void loop()
{
  // Change direction once the motor reaches target position
  //  if (stepperPusher.distanceToGo() == 0)
  //    stepperPusher.moveTo(stepperPusher.currentPosition()+200);
  //  // Move the motor one step
  //  Serial.println(stepperPusher.currentPosition());
  //  stepperPusher.run();
  stepperPusher.run();
  stepperTilt.run();
  stepperPan.run();

  ArduinoOTA.handle();
}