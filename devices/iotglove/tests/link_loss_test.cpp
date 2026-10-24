#include "link_loss.h"
#include "network_recovery.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
using namespace iotglove;

int main() {
  // RTC RAM starts with arbitrary content after a cold boot: no magic, no record.
  LinkLossRecord record;
  memset(&record, 0xA5, sizeof(record));
  assert(!linkLossPresent(record) && !linkLossPending(record));

  LinkLossContext context;
  context.stateKnown = true; context.phase = 5; context.role = 3; context.deviceState = 4;
  context.motor = true; context.lit = 4; context.motorAgeMs = 120;
  context.heapFree = 177000; context.batteryAdcMv = 1980; context.batteryMv = 0;
  linkLossCapture(record, 5000, 4200, 200, -71, 0, context);
  assert(linkLossPresent(record) && linkLossPending(record));
  assert(record.cause == static_cast<uint8_t>(LinkLossCause::None));
  assert(record.reason == 200 && record.rssi == -71);
  assert(record.lostAtMs == 4200 && record.noticedAtMs == 5000 && record.restartAtMs == 0);
  assert(record.stateKnown && record.phase == 5 && record.role == 3 && record.deviceState == 4);
  assert(record.motor && record.lit == 4 && record.motorAgeMs == 120);
  assert(record.heapFree == 177000 && record.batteryAdcMv == 1980 && record.batteryMv == 0);

  // A restart completes the pending record without touching the loss moment.
  LinkLossContext later = context;
  later.motor = false; later.motorAgeMs = 61000;
  linkLossComplete(record, LinkLossCause::WifiDown, 65000, 4, 1, later);
  assert(!linkLossPending(record) && linkLossPresent(record));
  assert(record.cause == static_cast<uint8_t>(LinkLossCause::WifiDown));
  assert(record.restartAtMs == 65000 && record.connectFailures == 4 && record.restartAttempt == 1);
  assert(record.lostAtMs == 4200 && record.reason == 200 && record.rssi == -71 && record.motor);
  assert(!strcmp(linkLossCauseName(static_cast<LinkLossCause>(record.cause)), "wifi_down"));

  // A worker stall with the STA still associated starts its own record.
  linkLossClear(record);
  assert(!linkLossPresent(record));
  LinkLossContext stall;
  stall.stateKnown = false; stall.motorAgeMs = kLinkLossMotorNever;
  linkLossComplete(record, LinkLossCause::WorkerStall, 90000, 0, 2, stall);
  assert(linkLossPresent(record) && !linkLossPending(record));
  assert(record.lostAtMs == 0 && record.reason == 0 && record.rssi == 0);
  assert(record.noticedAtMs == 90000 && record.restartAtMs == 90000 && record.restartAttempt == 2);
  assert(!record.stateKnown && record.motorAgeMs == kLinkLossMotorNever);
  assert(!strcmp(linkLossCauseName(static_cast<LinkLossCause>(record.cause)), "worker_stall"));
  assert(!strcmp(linkLossCauseName(LinkLossCause::None), "none"));

  // Outage policy: a link that was up gets 30s, a boot that never associated 180s.
  NetworkDiagnostics d;
  d.wifiConnected = false; d.wifiWasUp = true; d.wifiDownSinceMs = 10000;
  assert(!wifiLinkDownTooLong(d, 39999));
  assert(wifiLinkDownTooLong(d, 40000));
  d.wifiConnected = true;
  assert(!wifiLinkDownTooLong(d, 40000));
  d.wifiConnected = false; d.wifiWasUp = false;
  assert(!wifiLinkDownTooLong(d, 189999));
  assert(wifiLinkDownTooLong(d, 190000));
  d.phase = NetworkPhase::Ota;  // OTA keeps the worker-watchdog exemption.
  assert(!wifiLinkDownTooLong(d, 190000));
  d.phase = NetworkPhase::Connecting; d.wifiWasUp = true;
  d.wifiDownSinceMs = UINT32_MAX - 1000;  // Rollover of millis(): 30s later is 28999.
  assert(!wifiLinkDownTooLong(d, 28998));
  assert(wifiLinkDownTooLong(d, 28999));
  puts("PASS: link loss record capture/complete and STA outage restart policy");
}
