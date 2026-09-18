#include "iotglove_beetle.h"
#include <BLEDevice.h>
#include <host/ble_gap.h>
#include <host/ble_hs.h>
#include <host/ble_hs_adv.h>

// ESP32-C3 core 3.3.11 uses NimBLE. Use the native, streaming GAP callback to
// avoid BLEScan's unbounded map of every nearby address, including non-HAS3.
namespace beetle {
namespace {
struct Observation { char name[24]; int16_t rssi; uint32_t at; };
QueueHandle_t observations = nullptr;
iotglove::LocationTracker tracker(kBeaconMap, sizeof(kBeaconMap) / sizeof(kBeaconMap[0]));
std::atomic<bool> scanning{false};
bool active = false;
uint32_t scanStarted = 0;
uint32_t scanAttempted = 0;
uint8_t ownAddressType = BLE_OWN_ADDR_PUBLIC;

int gapEvent(ble_gap_event* event, void*) {
  if (event->type == BLE_GAP_EVENT_DISC_COMPLETE) {
    scanning.store(false);
  } else if (event->type == BLE_GAP_EVENT_DISC) {
    ble_hs_adv_fields fields = {};
    if (ble_hs_adv_parse_fields(&fields, event->disc.data, event->disc.length_data) != 0 ||
        !fields.name || fields.name_len < 6 || fields.name_len >= sizeof(Observation::name))
      return 0;
    Observation observation = {};
    memcpy(observation.name, fields.name, fields.name_len);
    if (!iotglove::mappedRoom(observation.name, kBeaconMap,
                             sizeof(kBeaconMap) / sizeof(kBeaconMap[0]))) return 0;
    observation.rssi = event->disc.rssi;
    observation.at = millis();
    // This callback never waits. Old queued observations expire by their receive time.
    xQueueSend(observations, &observation, 0);
  }
  return 0;
}
}

void bleInit() {
  observations = xQueueCreate(24, sizeof(Observation));
  if (!observations) abort();
  BLEDevice::init("OriginGloveBeetle");
  ESP_ERROR_CHECK(ble_hs_id_infer_auto(0, &ownAddressType) == 0 ? ESP_OK : ESP_FAIL);
}

void blePoll(uint32_t now, bool enabled) {
  if (enabled != active) {
    active = enabled;
    tracker.clear();
    xQueueReset(observations);
    if (!active && scanning.load()) {
      ble_gap_disc_cancel();
      scanning.store(false);
    }
  }
  Observation observation;
  for (size_t budget = 0; budget < 24 && xQueueReceive(observations, &observation, 0); ++budget) {
    if (active && now - observation.at < iotglove::LocationTracker::kTtlMs)
      tracker.observe(observation.name, observation.rssi, observation.at);
  }
  if (active && !scanning.load() && now - scanAttempted >= 100) {
    scanAttempted = now;
    ble_gap_disc_params params = {};
    params.itvl = 160;    // 100 ms (0.625 ms units).
    params.window = 128;  // 80 ms; active scan includes name in scan responses.
    params.passive = 0;
    params.filter_duplicates = 0;
    scanning.store(true);
    scanStarted = now;
    if (ble_gap_disc(ownAddressType, 1000, &params, gapEvent, nullptr) != 0)
      scanning.store(false);
  }
}

bool bleHealthy(uint32_t now) { return !scanning.load() || now - scanStarted < 8000; }
iotglove::Location currentLocation(uint32_t now) {
  return active ? tracker.update(now) : iotglove::Location{};
}
}
