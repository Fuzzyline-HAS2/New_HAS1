#ifndef _LIBRARY_AND_PIN_
#define _LIBRARY_AND_PIN_

#include <Arduino.h>

#include <SPI.h>
#include <Adafruit_PN532.h>
#include <HardwareSerial.h>
#include <SimpleTimer.h>

#define PN532_SCK   4
#define PN532_MISO  5
#define PN532_MOSI  6
#define PN532_SS1   7
#define PN532_SS2   9
#define PN532_SS3   2

#define HWSERIAL_RX 8
#define HWSERIAL_TX 21

#endif
