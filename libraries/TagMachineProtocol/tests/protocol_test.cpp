#include <assert.h>
#include <initializer_list>
#include <string.h>
#include "../src/TagMachineOtaProtocol.h"

using namespace tagmachine::ota_wire;

int main() {
  char line[kMaxLine + 2] = {};
  size_t size = formatCommand(line, sizeof(line), CommandType::Query, 42);
  assert(size == strlen("Q:42\n"));
  Command command;
  assert(parseCommand(line, size - 1, command));
  assert(command.type == CommandType::Query && command.request == 42 &&
         command.targetVersion == 0);

  size = formatCommand(line, sizeof(line), CommandType::Update, 42, 7);
  assert(size == strlen("U:42:7\n"));
  assert(parseCommand(line, size - 1, command));
  assert(command.type == CommandType::Update && command.request == 42 &&
         command.targetVersion == 7);

  size = formatVersion(line, sizeof(line), 42, 4, 1, 1234);
  Response response;
  assert(parseResponse(line, size - 1, response));
  assert(response.type == ResponseType::Version && response.request == 42);
  assert(response.protocol == 2 && response.firmware == 4 && response.partition == 1);
  assert(response.boot == 1234);

  size = formatOutcome(line, sizeof(line), 42, Outcome::Updated, 5, 1, 999);
  assert(parseResponse(line, size - 1, response));
  assert(response.type == ResponseType::Outcome && response.outcome == Outcome::Updated);
  assert(response.firmware == 5 && response.boot == 999);

  for (const char* invalid : {
      "Q:0", "Q:-1", "Q:1:2", "U:1:0", "U:1:2147483648", "U:1:2:3",
      "RV:1:1:4:1", "RV:1:1:4:1:0",
      "RO:1:X:4:1:2", "RO:1:U:0:1:2", "RO:1:U:4:1:2:",
      "IG1|OTA_RESULT|1|updated|4"}) {
    Command badCommand;
    Response badResponse;
    assert(!parseCommand(invalid, strlen(invalid), badCommand));
    assert(!parseResponse(invalid, strlen(invalid), badResponse));
  }

  // Every Beetle-to-TTGO extension begins with R, so a legacy TTGO consumes it
  // as control/status instead of dispatching it as a four-byte tag.
  assert(line[0] == 'R');
  return 0;
}
