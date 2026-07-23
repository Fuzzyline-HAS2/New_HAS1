#pragma once

void RfidInit();
void RfidHalUpdate();
bool RfidTagPresent();
bool RfidReadTag(uint8_t data[32]);
String CheckingPlayers(uint8_t rfidData[32]);
