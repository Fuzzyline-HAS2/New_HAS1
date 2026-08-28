#ifndef _LIBRARY_AND_PIN_
#define _LIBRARY_AND_PIN_

#include <Arduino.h>

#include <Adafruit_NeoPixel.h>
#include <HardwareSerial.h>
#include "DFRobotDFPlayerMini.h"
#include <HAS2_Wifi.h>
#include <SimpleTimer.h>
#include <SecureOTA.h>
#include "secrets.h"

#define DFPLAYER_RX_PIN 39
#define DFPLAYER_TX_PIN 33

#define HWSERIAL_RX 18
#define HWSERIAL_TX 23

#define RELAY_PIN    14

#define SW_PIN      4

#define EN_PIN      12 

#define STEP_PIN    13                       
#define DIR_PIN     15

#define LINE_NEOPIXEL_PIN       25
#define ROUND_NEOPIXEL_PIN      26
#define ONBOARD_NEOPIXEL_PIN    27

#endif
