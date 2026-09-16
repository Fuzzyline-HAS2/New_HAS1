#pragma once
#include <IoTGloveLocation.h>

// Exact beacon device_name -> canonical Origin room. Room names/aliases below
// are verified against fuzzyline-core bc6907f config/audio-layouts.json.
// The server Origin layout contains NO device-prefix map. Add the real installed
// altar/revival device IDs here after surveying their advertised HAS3:<name>.
// Do not infer room from the first character of an unverified device name.
constexpr iotglove::BeaconMapEntry kBeaconMap[] = {
    {"bambooForest", "bambooForest"},
    {"bambooForestB", "bambooForest"},
    {"bamboo", "bambooForest"},
    {"livingRoom", "livingRoom"},
    {"living", "livingRoom"},
    {"sleepingRoom", "sleepingRoom"},
    {"toilet", "toilet"},
    {"undergroundRoom", "undergroundRoom"},
    {"undergroundRoomB", "undergroundRoom"},
    {"underground", "undergroundRoom"},
    {"hallway", "hallway"},
};
