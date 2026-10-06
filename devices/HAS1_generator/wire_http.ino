// Generator-only CAS endpoint. Never uses HAS2_Wifi's shared HTTPClient.
#include <HTTPClient.h>

static String WireUrlEncode(const char *value) {
  static const char hex[] = "0123456789ABCDEF";
  String result;
  for (const uint8_t *p = reinterpret_cast<const uint8_t *>(value); *p; ++p) {
    const uint8_t c = *p;
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') result += char(c);
    else { result += '%'; result += hex[c >> 4]; result += hex[c & 15]; }
  }
  return result;
}

static bool WireCopyJsonString(JsonVariantConst value, char *output, size_t capacity) {
  if (!value.is<const char *>()) return false;
  const JsonString jsonText = value.as<JsonString>();
  const char *text = jsonText.c_str();
  const size_t length = jsonText.size();
  if (strlen(text) != length) return false; // Reject embedded NUL identity/context strings.
  if (!length || length >= capacity) return false;
  memcpy(output, text, length + 1);
  return true;
}

static bool WireParseSnapshot(const JsonDocument &json, const char *device,
                              GeneratorWireSnapshot &snapshot) {
  if (!WireCopyJsonString(json["device_name"], snapshot.device, sizeof(snapshot.device)) ||
      strcmp(snapshot.device, device) != 0 ||
      !WireCopyJsonString(json["epoch"], snapshot.epoch, sizeof(snapshot.epoch)) ||
      !WireCopyJsonString(json["game_state"], snapshot.gameState, sizeof(snapshot.gameState)) ||
      !WireCopyJsonString(json["device_state"], snapshot.deviceState, sizeof(snapshot.deviceState)) ||
      !json["revision"].is<uint32_t>() || !json["battery_pack"].is<int>() ||
      !json["max_battery_pack"].is<int>()) return false;
  if (strlen(snapshot.epoch) != 36) return false;
  for (unsigned i = 0; i < 36; ++i) {
    const char c = snapshot.epoch[i];
    if (i == 8 || i == 13 || i == 18 || i == 23) { if (c != '-') return false; }
    else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
  }
  snapshot.revision = json["revision"].as<uint32_t>();
  snapshot.count = json["battery_pack"].as<int>();
  snapshot.maximum = json["max_battery_pack"].as<int>();
  return snapshot.maximum >= 1 && snapshot.maximum <= 4; // GET may reveal an old invalid accumulated count.
}

GeneratorWireResult WireHttpRequest(bool set, const char *device, int count,
                                    const char *epoch, uint32_t revision,
                                    GeneratorWireSnapshot &snapshot) {
  if (WiFi.status() != WL_CONNECTED || !device || !device[0] || strlen(device) >= sizeof(snapshot.device))
    return GeneratorWireResult::Failed;
  String url = String(GENERATOR_SERVER_URL) + "/has2.php?request=" +
      (set ? "SetGeneratorWires" : "GetGeneratorWires") +
      "&mac=" + WireUrlEncode(WiFi.macAddress().c_str()) + "&key=" + WireUrlEncode(device);
  if (set) url += "&value=" + String(count) + "&epoch=" + WireUrlEncode(epoch) + "&revision=" + String(revision);
  HTTPClient request;
  request.setConnectTimeout(700);
  request.setTimeout(700);
  request.setReuse(false);
  if (!request.begin(url)) return GeneratorWireResult::Failed;
  const int status = request.GET();
  if (status != 200 && status != 409) {
    Serial.printf("[WireSync] %s HTTP=%d\n", set ? "set" : "get", status);
    request.end(); return GeneratorWireResult::Failed;
  }
  const int length = request.getSize();
  char body[1025];
  if (length <= 0 || length >= static_cast<int>(sizeof(body))) {
    Serial.println("[WireSync] invalid/missing response length");
    request.end(); return GeneratorWireResult::Failed;
  }
  auto *stream = request.getStreamPtr();
  size_t received = 0;
  const uint32_t started = millis();
  while (received < static_cast<size_t>(length) && uint32_t(millis() - started) < 1000) {
    const int available = stream->available();
    if (available > 0) {
      const size_t remaining = static_cast<size_t>(length) - received;
      const size_t chunk = static_cast<size_t>(available) < remaining ? static_cast<size_t>(available) : remaining;
      const int read = stream->read(reinterpret_cast<uint8_t *>(body) + received, chunk);
      if (read <= 0) break;
      received += static_cast<size_t>(read);
    } else if (!request.connected()) break;
    else delay(1);
  }
  request.end();
  if (received != static_cast<size_t>(length)) return GeneratorWireResult::Failed;
  JsonDocument json;
  if (deserializeJson(json, body, received) || !json["success"].is<bool>() ||
      !WireParseSnapshot(json, device, snapshot)) return GeneratorWireResult::Failed;
  if (status == 409 && !json["success"].as<bool>() && json["error"].is<const char *>() &&
      strcmp(json["error"].as<const char *>(), "CONFLICT") == 0) return GeneratorWireResult::Conflict;
  if (status != 200 || !json["success"].as<bool>()) return GeneratorWireResult::Failed;
  if (set && (strcmp(snapshot.epoch, epoch) != 0 || snapshot.count != count || snapshot.revision <= revision))
    return GeneratorWireResult::Failed;
  return GeneratorWireResult::Ok;
}
