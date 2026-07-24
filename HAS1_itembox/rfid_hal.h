#pragma once

void RfidInit();
void RfidHalUpdate();
bool RfidTagPresent();
bool RfidReadTag(uint8_t data[32]);
