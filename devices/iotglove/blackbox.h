#pragma once
#include <HAS1BlackBox.h>
#include "blackbox_policy.h"
namespace iotglove {
// Single producer per gate. These functions never do flash/socket I/O.
void blackBoxAdcSample(uint32_t now,uint32_t adcMv);
void blackBoxBatteryVoltage(uint32_t now);
void blackBoxOutput(uint32_t now,bool motor,int lit,int brightness);
void blackBoxBegin(int firmware,int partition);
void blackBoxLoop(uint32_t now,int phase,int role,int deviceState,bool known,bool ota,
                  bool motor,int lit,int batteryAdcMv,int batteryMv,bool peerOnline);
void blackBoxHttp(int error,int status,uint32_t duration,bool valid);
void blackBoxOperation(int phase);
void blackBoxBeforeRestart(int reason,int attempts);
}
