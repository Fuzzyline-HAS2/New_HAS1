#include <IoTGloveProtocol.h>
#include <assert.h>
#include <string.h>
#include <string>

using namespace iotglove::wire;

static bool decode(Decoder& decoder, const char* text, uint32_t at, Frame& output) {
  bool received = false;
  for (const char* p = text; *p; ++p) received = decoder.feed(*p, at, output) || received;
  return received;
}

int main() {
  Frame input;
  strcpy(input.type, "LOC");
  input.id = UINT32_MAX;
  assert(put(input, "bamboo"));
  assert(put(input, "-68"));
  assert(put(input, "250"));
  assert(put(input, "1"));
  char line[kMaxLine + 1];
  const size_t length = format(line, sizeof(line), input);
  assert(length && strcmp(line, "IG1|LOC|4294967295|bamboo|-68|250|1\n") == 0);
  Decoder decoder;
  Frame output;
  for (size_t i = 0; i < length - 1; ++i) assert(!decoder.feed(line[i], 100, output));
  assert(decoder.feed('\n', 101, output));
  assert(output.id == input.id && output.count == 4 && strcmp(output.args[1], "-68") == 0);
  uint32_t unsignedValue = 123;
  assert(uint32("4294967295", unsignedValue) && unsignedValue == UINT32_MAX);
  assert(!uint32("4294967296", unsignedValue));
  assert(!uint32("", unsignedValue));
  assert(!uint32("+1", unsignedValue));
  assert(!uint32("1x", unsignedValue));
  int32_t signedValue = 0;
  assert(int32("-2147483648", signedValue) && signedValue == INT32_MIN);
  assert(!int32("2147483648", signedValue));
  assert(!int32("-2147483649", signedValue));
  assert(!int32("-", signedValue));
  assert(!put(input, "bad|token"));
  assert(!format(line, 8, input));
  assert(!decode(decoder, "IG2|PING|1\n", 200, output));
  assert(!decode(decoder, "IG1|PING|1||x\n", 200, output));
  assert(!decode(decoder, "IG1|PING|1\r\n", 200, output));
  assert(!decode(decoder, "IG1|PING|1|1|2|3|4|5|6|7|8|9\n", 200, output));
  assert(!decode(decoder, "IG1|PING|4294967296\n", 200, output));
  const std::string oversized(300, 'A');
  assert(!decode(decoder, oversized.c_str(), 200, output));
  assert(!decode(decoder, "IG1|PING|1\n", 200, output));
  assert(decode(decoder, "IG1|PING|1\n", 200, output));
  assert(!decode(decoder, "IG1|PI", 300, output));
  assert(!decode(decoder, "NG|1\n", 551, output));
  assert(decode(decoder, "IG1|PING|2\n", 552, output));
  decoder.reset();
  assert(!decode(decoder, "IG1|PING|", UINT32_MAX - 10, output));
  assert(decode(decoder, "1\n", 15, output));
}
