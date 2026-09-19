#include "HAS1BleBeacon.h"
#include <esp_bt.h>
#include <freertos/FreeRTOS.h>

#include <functional>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

using namespace Has1BleBeacon;

#define REQUIRE(condition) do { if (!(condition)) throw std::runtime_error( \
    std::string(__func__) + ":" + std::to_string(__LINE__) + ": " #condition); } while (false)

namespace {
uint32_t fakeNow = 100;
unsigned starts = 0;
unsigned registrations = 0;
unsigned sendReadyChecks = 0;
unsigned criticalDepth = 0;
portMUX_TYPE* activeMux = nullptr;
bool startResult = true;
bool available = true;
bool immediateCompletion = false;
esp_err_t registerResult = ESP_OK;
const esp_vhci_host_callback_t* registeredCallback = nullptr;
std::vector<std::vector<uint8_t>> sent;

uint16_t lastOpcode() {
  REQUIRE(!sent.empty());
  return static_cast<uint16_t>(sent.back()[1] | (sent.back()[2] << 8));
}

void completeEvent(uint16_t opcode, uint8_t status = 0) {
  REQUIRE(registeredCallback != nullptr);
  uint8_t event[] = {4, 0x0e, 4, 1, static_cast<uint8_t>(opcode & 255),
                     static_cast<uint8_t>(opcode >> 8), status};
  REQUIRE(registeredCallback->notify_host_recv(event, sizeof(event)) == 0);
}

void statusEvent(uint16_t opcode, uint8_t status) {
  REQUIRE(registeredCallback != nullptr);
  uint8_t event[] = {4, 0x0f, 4, status, 1, static_cast<uint8_t>(opcode & 255),
                     static_cast<uint8_t>(opcode >> 8)};
  REQUIRE(registeredCallback->notify_host_recv(event, sizeof(event)) == 0);
}

void prepare() {
  setDeviceName("tagmachine_1");
  begin();
  REQUIRE(starts == 1 && registrations == 1);
  REQUIRE(registeredCallback != nullptr);
}

void startupFailure() {
  startResult = false;
  setDeviceName("valid");
  begin();
  startResult = true;
  begin();
  for (unsigned i = 0; i < 10000; ++i) { ++fakeNow; poll(); }
  REQUIRE(starts == 1 && registrations == 0);
  REQUIRE(sendReadyChecks == 0 && sent.empty());
  REQUIRE(diagnostics().lastError == Error::StartupFailed);
  REQUIRE(diagnostics().failures == 1);
}

void registrationFailure() {
  registerResult = -1;
  begin();
  registerResult = ESP_OK;
  begin();
  for (unsigned i = 0; i < 10000; ++i) { ++fakeNow; poll(); }
  REQUIRE(starts == 1 && registrations == 1);
  REQUIRE(sendReadyChecks == 0 && sent.empty());
  REQUIRE(diagnostics().lastError == Error::StartupFailed);
}

void immediateCallbackAndIdempotence() {
  immediateCompletion = true;
  prepare();
  for (unsigned i = 0; i < 32 && diagnostics().step != Step::Running; ++i) {
    begin();
    poll();
    ++fakeNow;
  }
  REQUIRE(diagnostics().step == Step::Running);
  REQUIRE(diagnostics().radio == RadioState::On);
  REQUIRE(diagnostics().failures == 0);
  REQUIRE(starts == 1 && registrations == 1);
  REQUIRE(sent.size() == 5);
  for (unsigned i = 0; i < 1000; ++i) { ++fakeNow; poll(); }
  REQUIRE(sent.size() == 5);
}

void successfulStatusIsNotFinal() {
  prepare();
  poll();
  REQUIRE(lastOpcode() == 0x0c03);
  statusEvent(lastOpcode(), 0);
  ++fakeNow;
  poll();
  REQUIRE(diagnostics().awaitingCompletion);
  REQUIRE(diagnostics().sentCommands == 1);
  REQUIRE(diagnostics().step == Step::Reset);
  completeEvent(lastOpcode());
  const unsigned checks = sendReadyChecks;
  poll(false);
  REQUIRE(sendReadyChecks == checks);
  REQUIRE(!diagnostics().awaitingCompletion);
  REQUIRE(diagnostics().step == Step::Parameters);
  poll();
  REQUIRE(lastOpcode() == 0x2006);
  statusEvent(lastOpcode(), 0x0c);
  poll(false);
  REQUIRE(diagnostics().lastError == Error::CommandRejected);
  REQUIRE(diagnostics().hciStatus == 0x0c);
  REQUIRE(diagnostics().failures == 1);
  REQUIRE(!diagnostics().awaitingCompletion);
  REQUIRE(sent.size() == 2);
}

void timelyMailboxConsumption() {
  prepare();
  poll();
  ++fakeNow;
  completeEvent(lastOpcode());
  fakeNow += 5000;
  const unsigned checks = sendReadyChecks;
  poll(false); // Drain completion before checking deadlines, even when busy.
  REQUIRE(sendReadyChecks == checks);
  REQUIRE(diagnostics().failures == 0);
  REQUIRE(diagnostics().step == Step::Parameters);
  REQUIRE(diagnostics().radio == RadioState::Off);
  REQUIRE(!diagnostics().awaitingCompletion);
  immediateCompletion = true;
  for (unsigned i = 0; i < 32 && diagnostics().step != Step::Running; ++i) { ++fakeNow; poll(); }
  REQUIRE(diagnostics().step == Step::Running);
  REQUIRE(diagnostics().failures == 0);
}

void wrongAndDuplicateEvents() {
  prepare();
  poll();
  completeEvent(0x2006); // An unrelated opcode must not claim the outstanding Reset.
  poll(false);
  REQUIRE(diagnostics().awaitingCompletion);
  REQUIRE(diagnostics().pendingOpcode == 0x0c03);
  completeEvent(0x0c03);
  completeEvent(0x0c03, 0x0c); // Duplicate cannot overwrite the first final response.
  completeEvent(0); // Unsolicited/no-op completion cannot replace a published result.
  poll(false);
  REQUIRE(diagnostics().step == Step::Parameters);
  REQUIRE(diagnostics().failures == 0);
  REQUIRE(!diagnostics().awaitingCompletion);
  REQUIRE(sent.size() == 1);
}

void unavailableTransportService() {
  prepare();
  available = false;
  unsigned serviceHeartbeats = 0;
  for (unsigned i = 0; i < 10000; ++i) {
    ++serviceHeartbeats;
    ++fakeNow;
    poll();
  }
  REQUIRE(serviceHeartbeats == 10000);
  REQUIRE(sent.empty());
  REQUIRE(starts == 1 && registrations == 1);
  REQUIRE(diagnostics().lastError == Error::SendUnavailable);
  REQUIRE(diagnostics().failures > 0 && diagnostics().failures < 6);
}
} // namespace

unsigned long millis() {
  REQUIRE(criticalDepth == 0);
  return fakeNow;
}

void testEnterCritical(portMUX_TYPE* mux) {
  REQUIRE(criticalDepth == 0);
  REQUIRE(mux != nullptr);
  activeMux = mux;
  ++criticalDepth;
}

void testExitCritical(portMUX_TYPE* mux) {
  REQUIRE(criticalDepth == 1 && mux == activeMux);
  --criticalDepth;
  activeMux = nullptr;
}

bool btStartMode(esp_bt_mode_t mode) {
  REQUIRE(criticalDepth == 0);
  REQUIRE(mode == BT_MODE_BLE);
  ++starts;
  return startResult;
}

esp_err_t esp_vhci_host_register_callback(const esp_vhci_host_callback_t* callback) {
  REQUIRE(criticalDepth == 0);
  ++registrations;
  if (registerResult == ESP_OK) registeredCallback = callback;
  return registerResult;
}

bool esp_vhci_host_check_send_available() {
  REQUIRE(criticalDepth == 0);
  ++sendReadyChecks;
  return available;
}

void esp_vhci_host_send_packet(uint8_t* data, uint16_t size) {
  REQUIRE(criticalDepth == 0);
  REQUIRE(available);
  REQUIRE(diagnostics().awaitingCompletion);
  sent.emplace_back(data, data + size);
  REQUIRE(diagnostics().pendingOpcode == lastOpcode());
  if (immediateCompletion) completeEvent(lastOpcode());
}

extern "C" bool bleInUse(void);

int main(int argc, char** argv) {
  const std::map<std::string, std::function<void()>> tests = {
    {"startup_failure", startupFailure}, {"registration_failure", registrationFailure},
    {"immediate_callback_idempotence", immediateCallbackAndIdempotence},
    {"successful_status_not_final", successfulStatusIsNotFinal},
    {"timely_mailbox_consumption", timelyMailboxConsumption},
    {"wrong_duplicate_events", wrongAndDuplicateEvents},
    {"unavailable_transport_service", unavailableTransportService}
  };
  try {
    REQUIRE(argc == 2);
    REQUIRE(bleInUse());
    const auto test = tests.find(argv[1]);
    REQUIRE(test != tests.end());
    test->second();
    REQUIRE(criticalDepth == 0);
    std::cout << "PASS adapter: " << test->first << '\n';
  } catch (const std::exception& error) {
    std::cerr << "FAIL adapter: " << error.what() << '\n';
    return 1;
  }
}
