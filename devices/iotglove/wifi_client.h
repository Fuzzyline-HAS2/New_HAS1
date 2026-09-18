#pragma once
#include "game_state.h"

namespace iotglove {
struct GameResult {
  enum class Status : uint8_t { Success, Unknown, Rejected };
  uint32_t sequence = 0;
  Status status = Status::Unknown;
};
enum class NetworkOtaStatus : uint8_t { Idle, Queued, Running, Skipped, Failed };

// Single worker owns HAS2_Wifi, JSON, HTTP and OTA; all loop-facing calls are nonblocking.
bool networkBegin(int firmwareVersion, int partitionVersion);
bool networkPoll(ServerSnapshot& snapshot);
bool networkPollResult(GameResult& result);
bool networkSubmit(const GameEvent& event);
void networkRequestSnapshot();
void networkReportLocation(const char* room);
void networkReportBattery(float volts);
// Call only after matching Beetle OTA updated/skipped result, outside a live game.
bool networkRequestOta(uint32_t targetVersion = 0, const char* sourceCommand = "");
NetworkOtaStatus networkOtaStatus();
void networkClearOtaStatus();
}  // namespace iotglove
