#pragma once

namespace beetle_config {
constexpr int kUartRx = 6;
constexpr int kUartTx = 5;
// Schematic: TTGO GPIO12 -> Beetle symbol pin 4, labelled GPIO1 (not GPIO3).
constexpr int kResetRequest = 1;
constexpr uint32_t kBaud = 115200;
constexpr uint32_t kHeartbeatMs = 1000;
constexpr uint32_t kLocationReportMs = 1000;
constexpr uint32_t kTtgoTimeoutMs = 6000;
constexpr uint32_t kWatchdogMs = 30000;
constexpr uint32_t kOtaTimeoutMs = 180000;
}
