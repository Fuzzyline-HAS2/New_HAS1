#ifndef _LIBRARY_AND_PIN_
#define _LIBRARY_AND_PIN_

#include <Arduino.h>

#include <Adafruit_NeoPixel.h>
#include <HardwareSerial.h>
#include <SoftwareSerial.h>
#include "DFRobotDFPlayerMini.h"
#include "HAS2_Wifi.h"
#include <SimpleTimer.h>
#include <SecureOTA.h>
#include "secrets.h"

#define DFPLAYER_RX_PIN 39
#define DFPLAYER_TX_PIN 33
#define SUB_BEETLE_RX_PIN 19
#define SUB_BEETLE_TX_PIN 23
#define MAIN_BEETLE_RX_PIN 2
#define MAIN_BEETLE_TX_PIN 15

#define MAIN_LINE_NEOPIXEL_PIN       13
#define MAIN_ROUND_NEOPIXEL_PIN      14
#define SUB_LINE_NEOPIXEL_PIN       5
#define SUB_ROUND_NEOPIXEL_PIN      18
#define ONBOARD_NEOPIXEL_PIN    27

#define RELAY_PIN  21
#endif
