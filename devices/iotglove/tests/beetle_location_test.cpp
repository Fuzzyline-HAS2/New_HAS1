#include <IoTGloveLocation.h>
#include "iotglove_beetle/beacon_map.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <initializer_list>

using namespace iotglove;

void testInstalledBeaconMap() {
  constexpr size_t count = sizeof(kBeaconMap) / sizeof(kBeaconMap[0]);
  const char* bambooIds[] = {"HAS3:BI1", "HAS3:BI2", "HAS3:BR1", "HAS3:BR2",
                            "HAS3:BD1", "HAS3:BD2", "HAS3:BE", "HAS3:BT"};
  for (const char* id : bambooIds)
    assert(strcmp(mappedRoom(id, kBeaconMap, count), "bamboo") == 0);
  const char* ids[] = {"HAS3:LA", "HAS3:TI1", "HAS3:SI1", "HAS3:UI1", "HAS3:HI1"};
  const char* rooms[] = {"living", "toilet", "sleeping", "underground", "hallway"};
  for (size_t i = 0; i < 5; ++i)
    assert(strcmp(mappedRoom(ids[i], kBeaconMap, count), rooms[i]) == 0);
  assert(mappedRoom("HAS3:Babc_123-XYZ", kBeaconMap, count));
  assert(mappedRoom("HAS3:B12345678901234567", kBeaconMap, count));  // 18 characters.
  const char* invalid[] = {nullptr, "", "BI1", "HAS1:BI1", "HAS3:", "HAS3:B",
                          "HAS3:bI1", "HAS3:XI1", "HAS3:BI 1", "HAS3:BI1:",
                          "HAS3:B123456789012345678", "HAS3:bamboo",
                          "HAS3:living"};
  for (const char* id : invalid) assert(!mappedRoom(id, kBeaconMap, count));

}

constexpr size_t roomCount = sizeof(kBeaconMap) / sizeof(kBeaconMap[0]);

void pair(LocationTracker& tracker, const char* id, int rssi, uint32_t now) {
  assert(tracker.observe(id, rssi, now));
  assert(tracker.observe(id, rssi, now));
}

void assertRoom(const Location& location, const char* room, int rssi) {
  assert(location.valid);
  assert(strcmp(location.room, room) == 0);
  assert(location.rssi == rssi);
}

void testSelection() {
  LocationTracker tracker(kBeaconMap, roomCount);
  assert(!tracker.observe("HAS3:BI1", 1, 0));
  assert(!tracker.observe("HAS3:BI1", -128, 0));
  assert(tracker.observe("HAS3:BI1", -100, 0));
  assert(!tracker.update(0).valid);  // One sample cannot acquire a room.
  assert(tracker.observe("HAS3:BI1", -100, 0));
  assert(!tracker.update(199).valid);  // Evaluation runs no faster than 200 ms.
  assert(!tracker.update(200).valid);
  assert(!tracker.update(1200).valid);
  assertRoom(tracker.update(1400), "bamboo", -100);  // No -92 cutoff.

  tracker.clear();
  pair(tracker, "HAS3:BI1", -40, 0);
  pair(tracker, "HAS3:BI2", -90, 0);
  pair(tracker, "HAS3:BR1", -100, 0);
  pair(tracker, "HAS3:LA", -60, 0);
  assert(!tracker.update(0).valid);
  assertRoom(tracker.update(1200), "living", -60);  // B top two mean = -65.

  // All samples in the window contribute to median (including even rounding).
  tracker.clear();
  for (int rssi : {-100, -81, -60, -20}) assert(tracker.observe("HAS3:BI1", rssi, 0));
  tracker.update(0);
  assertRoom(tracker.update(1200), "bamboo", -70);
  pair(tracker, "HAS3:BI1", -40, 1600);
  assertRoom(tracker.update(1600), "bamboo", -55);
  assertRoom(tracker.update(1800), "bamboo", -47);  // .5 EMA each evaluation.
  assertRoom(tracker.update(1999), "bamboo", -47);

  // A 4 dB advantage is insufficient; exactly 5 dB held 1200 ms switches.
  for (int other : {-56, -55}) {
    tracker.clear();
    pair(tracker, "HAS3:BI1", -60, 0);
    tracker.update(0);
    tracker.update(1200);
    pair(tracker, "HAS3:BI1", -60, 1400);
    pair(tracker, "HAS3:LA", other, 1400);
    assertRoom(tracker.update(1400), "bamboo", -60);
    assertRoom(tracker.update(2400), "bamboo", -60);
    const Location result = tracker.update(2600);
    assertRoom(result, other == -55 ? "living" : "bamboo", other == -55 ? -55 : -60);
  }
}

void testLifetime() {
  LocationTracker tracker(kBeaconMap, roomCount);
  // The sample window includes its 1500 ms boundary.
  pair(tracker, "HAS3:BI1", -50, 0);
  tracker.update(0);
  assertRoom(tracker.update(1500), "bamboo", -50);
  assertRoom(tracker.update(4800), "bamboo", -50);  // Stable survives absent scores.
  assert(!tracker.update(5000).valid);  // Observation at timestamp zero counts.
  pair(tracker, "HAS3:LA", -70, 5200);
  assertRoom(tracker.update(5200), "living", -70);  // Unknown recovery is immediate.

  tracker.clear();
  tracker.update(0);
  tracker.update(5000);  // No observations also enters unknown after five seconds.
  pair(tracker, "HAS3:BI1", -60, 5200);
  assertRoom(tracker.update(5200), "bamboo", -60);

  tracker.clear();
  pair(tracker, "HAS3:BI1", -40, 0);
  tracker.update(0);
  assert(!tracker.update(1501).valid);  // Expired before initial hold completes.

  tracker.clear();
  pair(tracker, "HAS3:BI1", -40, 0);
  tracker.update(0);
  tracker.update(1200);
  // Sparse observations from another room keep stable B globally fresh, but
  // never reach two samples in the window and cannot acquire a room score.
  for (uint32_t now : {2000u, 4000u, 6000u}) {
    assert(tracker.observe("HAS3:LA", -20, now));
    const Location location = tracker.update(now);
    assertRoom(location, "bamboo", -40);
    assert(location.ageMs == 0);
  }
  assert(!tracker.update(11000).valid);

  // Expired EMA is removed, and reused slots do not inherit another device.
  tracker.clear();
  pair(tracker, "HAS3:BI1", -80, 0);
  tracker.update(0);
  tracker.update(1200);
  tracker.update(6201);  // Last EMA refresh was 1200; retention is >5000.
  pair(tracker, "HAS3:LA", -30, 6401);
  assertRoom(tracker.update(6401), "living", -30);

  const uint32_t start = UINT32_MAX - 600;
  tracker.clear(start);
  pair(tracker, "HAS3:BI1", -50, start);
  tracker.update(start);
  const Location wrapped = tracker.update(start + 1200);
  assertRoom(wrapped, "bamboo", -50);
  assert(wrapped.ageMs == 1200);
  assert(!tracker.update(start + 5000).valid);
}

void testCapacity() {
  LocationTracker tracker(kBeaconMap, roomCount);
  char name[24];
  for (size_t i = 0; i < LocationTracker::kCapacity; ++i) {
    snprintf(name, sizeof(name), "HAS3:B%zu", i);
    assert(tracker.observe(name, -70, 0));
  }
  assert(!tracker.observe("HAS3:LA", -40, 0));
  tracker.update(5001);  // Reclaim all 24 expired device slots.
  pair(tracker, "HAS3:LA", -40, 5201);
  assertRoom(tracker.update(5201), "living", -40);

  // The 97th/98th samples evict the oldest samples, including across wrap.
  const uint32_t start = UINT32_MAX - 100;
  tracker.clear(start);
  pair(tracker, "HAS3:BI1", -20, start);
  for (size_t i = 0; i < LocationTracker::kMaxSamples; ++i)
    assert(tracker.observe("HAS3:LA", -80, start + 200));
  tracker.update(start + 200);
  assertRoom(tracker.update(start + 1400), "living", -80);
}

int main() {
  testInstalledBeaconMap();
  testSelection();
  testLifetime();
  testCapacity();
  ResetRequestGate bootHigh;
  assert(!bootHigh.update(true, 0));
  assert(!bootHigh.update(true, 10000));
  assert(!bootHigh.update(false, 10001));
  assert(!bootHigh.update(false, 10051));
  assert(!bootHigh.update(true, 10052));
  assert(!bootHigh.update(false, 10060));  // Short HIGH bounce rejected.
  assert(!bootHigh.update(true, 10070));
  assert(!bootHigh.update(true, 10089));
  assert(bootHigh.update(true, 10090));
  assert(bootHigh.update(false, 11000));  // Latched until reboot.
  ResetRequestGate wrap;
  assert(!wrap.update(false, UINT32_MAX - 20));
  assert(!wrap.update(false, 29));
  assert(!wrap.update(true, 30));
  assert(wrap.update(true, 50));
}
