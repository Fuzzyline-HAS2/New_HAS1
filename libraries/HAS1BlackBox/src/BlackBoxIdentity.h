#pragma once
namespace has1blackbox {
// Borrowed pointers only. A missing username selects the already discovered STA
// MAC for MQTT ACLs; it never supplies a password or changes broker auth policy.
inline const char *resolveMqttUsername(const char *configured,
                                       const char *brokerUri,
                                       const char *stationMac) {
  if (configured && configured[0])
    return configured;
  return brokerUri && brokerUri[0] && stationMac ? stationMac : "";
}
} // namespace has1blackbox
