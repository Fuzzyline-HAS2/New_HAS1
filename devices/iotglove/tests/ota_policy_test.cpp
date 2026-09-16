#include <IoTGloveOta.h>
#include <assert.h>
#include <string>

using namespace iotglove::ota;

int main() {
  Command command;
  assert(parseCommand("github", command) == ParseResult::Valid);
  assert(command.ttgo.version == 0 && command.beetle.version == 0);
  assert(parseCommand("github@12:7", command) == ParseResult::Valid);
  assert(command.ttgo.version == 12 && command.beetle.version == 7);
  assert(parseCommand("github@12", command) == ParseResult::Valid);
  assert(command.ttgo.version == 12 && command.beetle.version == 12);
  assert(parseCommand("github@2147483647:2147483647", command) == ParseResult::Valid);
  assert(command.ttgo.version == INT32_MAX && command.beetle.version == INT32_MAX);
  for (const char* malformed : {"github@", "github@0", "github@01", "github@-1", "github@+1",
      "github@12:", "github@:7", "github@12:0", "github@12:7:1", "github@2147483648",
      "github@4294967296", "github@12\n", "github@12/../7", "github_prd@v12",
      "github@99999999999999999999999999999999", "githubish"}) {
    assert(parseCommand(malformed, command) == ParseResult::Malformed);
    assert(command.ttgo.version == 0 && command.beetle.version == 0);
  }
  assert(parseCommand(nullptr, command) == ParseResult::NotCommand);
  assert(parseCommand("setting", command) == ParseResult::NotCommand);

  char url[160];
  assert(archiveBaseUrl("iotglove", 12, url, sizeof(url)));
  assert(std::string(url) == "https://github.com/Fuzzyline-HAS2/New_HAS1/releases/download/iotglove-v12/");
  assert(archiveBaseUrl("iotglove_beetle", 7, url, sizeof(url)));
  assert(std::string(url).find("iotglove_beetle-v7/") != std::string::npos);
  assert(!archiveBaseUrl("HAS1_duct", 7, url, sizeof(url)));
  assert(!archiveBaseUrl("iotglove", 0, url, sizeof(url)));
  assert(!archiveBaseUrl("iotglove", 7, url, 10));
  assert(url[0] == 0);

  const std::string signature = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
  const std::string text = "IGOTA1|iotglove|12|1|min_spiffs|" + signature + "\n";
  Metadata metadata;
  assert(parseMetadata(text.data(), text.size(), metadata));
  assert(metadata.imageHmac[0] == 0x01 && metadata.imageHmac[31] == 0xef);
  assert(matchesMetadata(metadata, "iotglove", 12, 1));
  assert(!matchesMetadata(metadata, "iotglove_beetle", 12, 1));
  assert(!matchesMetadata(metadata, "iotglove", 13, 1));
  assert(!matchesMetadata(metadata, "iotglove", 12, 2));
  assert(!matchesMetadata(metadata, nullptr, 12, 1));
  for (const std::string& invalid : {
      "IGOTA2|iotglove|12|1|min_spiffs|" + signature + "\n",
      "IGOTA1|iotglove|012|1|min_spiffs|" + signature + "\n",
      "IGOTA1|iotglove|12|0|min_spiffs|" + signature + "\n",
      "IGOTA1|iotglove|12|1|default|" + signature + "\n",
      "IGOTA1|iotglove|12|1|min_spiffs|" + signature + "extra\n",
      "IGOTA1|iotglove|12|1|min_spiffs|" + signature + "|x\n",
      "IGOTA1|iotglove|12|1|min_spiffs|" + signature + "\r\n",
      "IGOTA1|iotglove|12|1|min_spiffs|" + signature.substr(1) + "\n",
      "IGOTA1|iotglove|12|1|min_spiffs|G" + signature.substr(1) + "\n",
      text + "\n", text.substr(0, text.size() - 1)}) {
    assert(!parseMetadata(invalid.data(), invalid.size(), metadata));
  }
  std::string nul = text; nul[10] = '\0';
  assert(!parseMetadata(nul.data(), nul.size(), metadata));
  assert(!parseMetadata(nullptr, text.size(), metadata));
  uint8_t copied[32]; memcpy(copied, metadata.imageHmac, 32);
  assert(equalHmac(copied, metadata.imageHmac));
  copied[0] ^= 1; assert(!equalHmac(copied, metadata.imageHmac));
  copied[0] ^= 1; copied[31] ^= 1; assert(!equalHmac(copied, metadata.imageHmac));
}
