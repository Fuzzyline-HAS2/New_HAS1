#pragma once
#include "HAS1BlackBox.h"
#include "BlackBoxEvidence.h"
#if defined(ARDUINO_ARCH_ESP32)
#include <WiFi.h>
#include <cstdio>
namespace has1blackbox {
// Only copies metadata provided by the Wi-Fi event. No radio query or scan.
inline void logWifiMetadata(WiFiEvent_t event,const WiFiEventInfo_t &info) {
  const uint8_t *bssid=nullptr;
  int channel=Unknown,reason=0;
  if(event==ARDUINO_EVENT_WIFI_STA_CONNECTED) {
    bssid=info.wifi_sta_connected.bssid;channel=info.wifi_sta_connected.channel;
  } else if(event==ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    bssid=info.wifi_sta_disconnected.bssid;reason=info.wifi_sta_disconnected.reason;
  } else if(event==ARDUINO_EVENT_WIFI_STA_GOT_IP) {
    const auto &ip=info.got_ip.ip_info;
    // Canonical big-endian numeric IPv4, independent of host byte order.
    blackbox.log(EventType::WifiEvent,
        ipv4FromNetworkBytes(reinterpret_cast<const uint8_t *>(&ip.ip.addr)),
        ipv4FromNetworkBytes(reinterpret_cast<const uint8_t *>(&ip.gw.addr)),
        ipv4FromNetworkBytes(reinterpret_cast<const uint8_t *>(&ip.netmask.addr)),
        info.got_ip.ip_changed,"wifi.ip.gw.mask");
    return;
  } else if(event!=ARDUINO_EVENT_WIFI_STA_LOST_IP) return;
  char text[28]="wifi.no_bssid";
  if(bssid) std::snprintf(text,sizeof(text),"ap:%02X%02X%02X%02X%02X%02X",
      bssid[0],bssid[1],bssid[2],bssid[3],bssid[4],bssid[5]);
  blackbox.log(EventType::WifiEvent,event,reason,channel,0,text);
}
} // namespace has1blackbox
#endif
