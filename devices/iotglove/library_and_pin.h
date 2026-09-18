#pragma once

// Confirmed wiring. GPIO12 is exclusively the Beetle reset request.
#define IOTGLOVE_LED_PIN 25
#define IOTGLOVE_LED_COUNT 4
#define IOTGLOVE_CHIP_PIN 26
#define IOTGLOVE_BUTTON_PIN 27
#define IOTGLOVE_MOTOR_PIN 13
#define IOTGLOVE_UART_RX 36
#define IOTGLOVE_UART_TX 32
#define IOTGLOVE_BEETLE_RESET_PIN 12
#define IOTGLOVE_UART_BAUD 115200

#ifndef IOTGLOVE_TRAINING
#define IOTGLOVE_TRAINING 0
#endif

// These defaults deliberately disable measurement/reporting until the actual
// divider, ADC wire and supported battery range have been measured. GPIO35 is
// only the old glove's candidate. Values can be provided as compiler defines.
#ifndef IOTGLOVE_BATTERY_PIN
#define IOTGLOVE_BATTERY_PIN 35
#endif
#ifndef IOTGLOVE_BATTERY_DIVIDER_RATIO
#define IOTGLOVE_BATTERY_DIVIDER_RATIO 0.0f
#endif
#ifndef IOTGLOVE_BATTERY_CALIBRATION
#define IOTGLOVE_BATTERY_CALIBRATION 1.0f
#endif
#ifndef IOTGLOVE_BATTERY_MIN_MV
#define IOTGLOVE_BATTERY_MIN_MV 0U
#endif
#ifndef IOTGLOVE_BATTERY_MAX_MV
#define IOTGLOVE_BATTERY_MAX_MV 0U
#endif

namespace iotglove {
constexpr unsigned kServerFreshMs = 15000;
constexpr unsigned kLocationFreshMs = 5000;
constexpr unsigned kResetPulseMs = 150;
constexpr unsigned kResetCooldownMs = 30000;
constexpr unsigned kBeetleOtaTimeoutMs = 300000;
}  // namespace iotglove
