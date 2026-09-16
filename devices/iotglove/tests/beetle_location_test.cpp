#include <IoTGloveLocation.h>
#include <assert.h>
#include <string.h>

using namespace iotglove;
constexpr BeaconMapEntry map[] = {{"A1", "livingRoom"}, {"B1", "hallway"}};

int main() {
  assert(mappedRoom("HAS3:A1", map, 2));
  assert(!mappedRoom("HAS1:A1", map, 2));
  assert(!mappedRoom("HAS3:A12", map, 2));
  assert(!mappedRoom("HAS3:", map, 2));
  LocationTracker tracker(map, 2);
  assert(!tracker.update(0).valid);
  assert(!tracker.observe("HAS3:unknown", -20, 100));
  assert(!tracker.observe("HAS3:A1", 20, 100));
  assert(tracker.observe("HAS3:A1", -70, 100));
  assert(strcmp(tracker.update(100).room, "livingRoom") == 0);
  assert(tracker.observe("HAS3:B1", -50, 200));
  assert(tracker.observe("HAS3:B1", -50, 201));
  assert(strcmp(tracker.update(201).room, "livingRoom") == 0);
  assert(strcmp(tracker.update(950).room, "livingRoom") == 0);
  assert(strcmp(tracker.update(951).room, "hallway") == 0);
  assert(tracker.update(951).ageMs == 750);
  assert(!tracker.update(5201).valid);
  assert(tracker.observe("HAS3:A1", -100, 5300));
  assert(!tracker.update(5300).valid);  // Weak beacon is not a valid room.
  tracker.clear();
  assert(!tracker.update(5300).valid);
  assert(tracker.observe("HAS3:A1", -60, UINT32_MAX - 50));
  assert(tracker.update(49).valid && tracker.update(49).ageMs == 100);
  assert(!tracker.update(4949).valid);

  // A single RSSI spike cannot switch a established three-sample/EMA candidate.
  tracker.clear();
  tracker.observe("HAS3:A1", -60, 100);
  tracker.update(100);
  tracker.observe("HAS3:B1", -80, 100);
  tracker.observe("HAS3:B1", -80, 200);
  tracker.observe("HAS3:B1", -20, 300);
  assert(strcmp(tracker.update(1500).room, "livingRoom") == 0);

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
