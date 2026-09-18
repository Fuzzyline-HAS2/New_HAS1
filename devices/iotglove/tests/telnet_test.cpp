#include "telnet_policy.h"

#include <assert.h>
#include <string>

using namespace iotglove;

static std::string commands(TelnetCommandParser& parser, const std::string& input) {
  std::string output;
  for (uint8_t byte : input) {
    char command = 0;
    if (parser.feed(byte, command)) output += command;
  }
  return output;
}

static std::string wifiLog(WifiLifecycleFilter& filter, const std::string& input) {
  std::string output;
  for (uint8_t byte : input) {
    const char* safe = filter.feed(byte);
    if (safe) output += safe;
  }
  return output;
}

int main() {
  ConsoleByteRing<8> ring;
  const std::string backlog = "0123456789abcdef";
  ring.write(reinterpret_cast<const uint8_t*>(backlog.data()), backlog.size());
  assert(ring.size() == 8 && ring.dropped() == 8);
  uint8_t data[12] = {};
  assert(ring.read(data, 3) == 3 && std::string(reinterpret_cast<char*>(data), 3) == "89a");
  ring.write(reinterpret_cast<const uint8_t*>("ghi"), 3);
  assert(ring.read(data, sizeof(data)) == 8 && std::string(reinterpret_cast<char*>(data), 8) == "bcdefghi");
  assert(ring.read(data, sizeof(data)) == 0);
  // A producer can overrun a stopped client indefinitely without growth.
  for (unsigned n = 0; n < 10000; ++n) ring.write(reinterpret_cast<const uint8_t*>("x"), 1);
  assert(ring.size() == 8 && ring.dropped() == 10000);

  TelnetCommandParser parser;
  assert(commands(parser, "s\r\np\nb\nu\n?\n") == "spbu?");
  assert(commands(parser, "status\nupdate\nbackup\nss\n s\ns \n") == "");
  assert(commands(parser, std::string("s\xff\xfb", 3)).empty()); // Fragmented WILL option.
  assert(commands(parser, std::string("\x01\r\n", 3)) == "s");
  assert(commands(parser, std::string("\xff\xfa", 2) + "b\nu\ns\n" + std::string("\xff\xf0", 2) + "p\n") == "p");
  assert(commands(parser, std::string("s\xff\xff\n", 4)).empty());
  assert(commands(parser, "s").empty());
  parser.reset(); // A new client cannot finish an old client's command.
  assert(commands(parser, "\n").empty());
  assert(commands(parser, std::string(10000, 'u') + "\np\n") == "p");
  assert(commands(parser, "s\bp\n") == "p");

  ConsoleCommandBudget budget;
  for (unsigned n = 0; n < 8; ++n) assert(budget.allow(0));
  for (unsigned n = 0; n < 10000; ++n) assert(!budget.allow(999));
  assert(budget.allow(1000));
  ConsoleCommandBudget wrap;
  for (unsigned n = 0; n < 8; ++n) assert(wrap.allow(UINT32_MAX - 500));
  assert(!wrap.allow(400));
  assert(wrap.allow(499));

  WifiLifecycleFilter filter;
  const auto safe = wifiLog(filter, "Try WiFi: secret-network\nTry saved WiFi: private\nWiFi connected\n");
  assert(safe == "[wifi] attempting configured network\n[wifi] attempting configured network\n[wifi] connected\n");
  assert(wifiLog(filter, "https://host/?key=secret\npassword: secret\n{\"secret\":1}\n").empty());
  assert(wifiLog(filter, "WiFi connected extra-secret\n").empty());
  assert(wifiLog(filter, "Try WiFi:" + std::string(500, 's') + "\n").empty());
  assert(wifiLog(filter, "WiFi scan failed\nWiFi connect failed\nWiFi disconnected. Reconnecting...\n") ==
      "[wifi] scan failed\n[wifi] connection attempt failed\n[wifi] reconnecting\n");

  BeetleLogTracker tracker;
  diagnostics::Log log;
  log.bootId = 10; log.sequence = 5; log.event = diagnostics::Event::Ota;
  log.code = diagnostics::Code::Checking; log.value = 42;
  assert(!tracker.accept(log, false, 10));
  assert(!tracker.accept(log, true, 11));
  assert(tracker.accept(log, true, 10));
  assert(!tracker.accept(log, true, 10));
  log.sequence = 4; assert(!tracker.accept(log, true, 10));
  log.sequence = 1; log.event = diagnostics::Event::Boot;
  log.code = diagnostics::Code::Reason; log.value = 7;
  assert(tracker.accept(log, true, 10)); // Late BOOT snapshot is independently cached.
  assert(tracker.reasonKnown(10) && tracker.reason() == 7);
  assert(!tracker.accept(log, true, 10));
  assert(!tracker.reasonKnown(11));
  log.bootId = 11; log.value = 3;
  assert(tracker.accept(log, true, 11) && tracker.reason() == 3);
  log.bootId = 10; assert(!tracker.accept(log, true, 11));
  return 0;
}
