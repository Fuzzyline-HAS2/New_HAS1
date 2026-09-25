#ifndef REVIVAL_CARD_UPLOAD_H
#define REVIVAL_CARD_UPLOAD_H
#include <stdint.h>
#include "card_ndef.h"

// Output adapter is supplied by telnet.ino; a single line, no trailing newline.
void CardUploadOutput(const char *line);
void CardUploadInit();
void CardUploadConnected();
void CardUploadDisconnected();
void CardUploadInput(uint8_t byte);
// Returns true only while server device_state is exactly "card-upload".
bool CardUploadSyncMode(const char *state);
bool CardUploadBlocksGameplay();
bool CardUploadBlocksOpen();
void CardUploadLoop();
#endif
