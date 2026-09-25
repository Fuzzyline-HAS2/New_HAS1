#pragma once
#include <IoTGloveLocation.h>

// Installed device IDs use these uppercase room prefixes (HAS3:<device ID>).
// Use the user-specified server room IDs independently of BLE device naming.
constexpr iotglove::BeaconMapEntry kBeaconMap[] = {
    {'B', "bamboo"},  // BI1/BI2, BR1/BR2, BD1/BD2, BE, BT
    {'L', "living"},    // LA (Living Altar)
    {'T', "toilet"},
    {'S', "sleeping"},
    {'U', "underground"},
    {'H', "hallway"},
};
