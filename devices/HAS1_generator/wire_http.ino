// Generator-only CAS endpoint. Never uses HAS2_Wifi's shared HTTPClient.
#include <HTTPClient.h>
#include <has2_http_body.h>

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
  snapshot.completionProtocol = json["completion_protocol"].is<unsigned>() && json["completion_protocol"].as<unsigned>()==1 ? 1 : 0;
  snapshot.count = json["battery_pack"].as<int>();
  snapshot.maximum = json["max_battery_pack"].as<int>();
  return snapshot.maximum >= 1 && snapshot.maximum <= 4; // GET may reveal an old invalid accumulated count.
}

// One outstanding wire operation. Repeated main-loop calls poll its result;
// changing count/context/generation rejects its old result instead of applying it.
static bool wireHttpPending=false,wireHttpDone=false;
static String wireHttpKey;
static char wireHttpExpectedDevice[32]={};
static uint32_t wireHttpRequestId=0;
static uint32_t wireHttpGeneration=0,wireHttpNetworkGeneration=0;
static GeneratorWireResult wireHttpOutcome=GeneratorWireResult::Failed;
static GeneratorWireSnapshot wireHttpResponse;
static void WireHttpResult(const NetworkRequest &request,const Has2BoundedTransport::Result &response,bool stale) {
  wireHttpDone=true;
  wireHttpOutcome=GeneratorWireResult::Failed;
  if(stale||!response.ok()||(response.status!=200&&response.status!=409))return;
  JsonDocument json;
  if(deserializeJson(json,response.payload)||!json["success"].is<bool>()||
     !WireParseSnapshot(json,wireHttpExpectedDevice,wireHttpResponse))return;
  if(response.status==409&&!json["success"].as<bool>()&&json["error"].is<const char*>()&&
     !strcmp(json["error"].as<const char*>(),"CONFLICT"))wireHttpOutcome=GeneratorWireResult::Conflict;
  else if(response.status==200&&json["success"].as<bool>())wireHttpOutcome=GeneratorWireResult::Ok;
  if(wireHttpOutcome!=GeneratorWireResult::Failed)GeneratorNetworkObserveProtocol(wireHttpResponse);
  (void)request;
}
GeneratorWireResult WireHttpRequest(bool set,const char *device,int count,const char *epoch,uint32_t revision,GeneratorWireSnapshot &snapshot) {
  if(!device||!device[0]||strlen(device)>=sizeof(snapshot.device))return GeneratorWireResult::Failed;
  const uint32_t generation=WireSnapshot().generation();
  const uint32_t gameGeneration=GeneratorNetworkGeneration();
  String path=String("/has2.php?request=")+(set?"SetGeneratorWires":"GetGeneratorWires")+
      "&mac="+WireUrlEncode(has2wifi.GetMacAddress().c_str())+"&key="+WireUrlEncode(device);
  if(set)path+="&value="+String(count)+"&epoch="+WireUrlEncode(epoch)+"&revision="+String(revision);
  if(wireHttpPending) {
    if(!wireHttpDone) {
      if(wireHttpKey!=path||wireHttpGeneration!=generation||wireHttpNetworkGeneration!=gameGeneration) {
        if(NetworkReplaceQueued(wireHttpRequestId,path,gameGeneration,generation,set?count:-1)) {
          wireHttpKey=path;wireHttpGeneration=generation;wireHttpNetworkGeneration=gameGeneration;
          snprintf(wireHttpExpectedDevice,sizeof(wireHttpExpectedDevice),"%s",device);
        }
      }
      return GeneratorWireResult::Pending;
    }
    wireHttpPending=false;wireHttpDone=false;
    if(wireHttpKey!=path||wireHttpGeneration!=generation||wireHttpNetworkGeneration!=gameGeneration)return GeneratorWireResult::Failed;
    snapshot=wireHttpResponse;
    if(wireHttpOutcome==GeneratorWireResult::Ok&&set&&
       (strcmp(snapshot.epoch,epoch)||snapshot.count!=count||snapshot.revision<=revision))return GeneratorWireResult::Failed;
    return wireHttpOutcome;
  }
  snprintf(wireHttpExpectedDevice,sizeof(wireHttpExpectedDevice),"%s",device);
  wireHttpRequestId=NetworkSubmit(NetworkKind::Wire,path,set,gameGeneration,generation,WireHttpResult,set?count:-1);
  if(!wireHttpRequestId)return GeneratorWireResult::Failed;
  wireHttpKey=path;wireHttpGeneration=generation;wireHttpNetworkGeneration=gameGeneration;
  wireHttpPending=true;return GeneratorWireResult::Pending;
}
