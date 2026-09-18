#include "application.h"

// SecureOTA CI increments the sketch version before compile/sign/release.
#define FIRMWARE_VER 5
#define PARTITION_VER 1

void setup() { gloveBegin(FIRMWARE_VER, PARTITION_VER); }
void loop() { gloveLoop(); }
