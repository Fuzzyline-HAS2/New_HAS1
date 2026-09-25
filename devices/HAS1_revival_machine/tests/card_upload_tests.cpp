#include <cassert>
#include <cstdint>
#include <cstring>
#include <deque>
#include <iostream>
#include <string>
#include <vector>
#include "pn532_transport.h"
#include "card_upload.h"
#include <Preferences.h>
static uint64_t fakeMs = 1000;
uint32_t millis() { return static_cast<uint32_t>(fakeMs); }
uint32_t micros() { return static_cast<uint32_t>(fakeMs * 1000); }
void delay(unsigned long value) { fakeMs += value; }
void delayMicroseconds(unsigned int value) { fakeMs += value / 1000; }
static std::vector<std::string> output;
void CardUploadOutput(const char* line) { output.emplace_back(line); }
static uint8_t memoryPages[231][4] = {};
static uint8_t versionBytes[8] = {0, 4, 4, 2, 1, 0, 0x0F, 3};
static uint8_t sessionBytes[8] = {1, 0, 0, 0, 0, 0, 1, 0};
static unsigned sessionChecks = 0;
static bool present = false;
static uint8_t tagUid[7] = {4, 1, 2, 3, 4, 5, 6};
static unsigned pnStages = 0, selectedStages = 0;
static std::deque<Pn532Result> selections;
static Pn532Result failNextOperation = Pn532Result::Ok;
static bool writeThenFail = false, corruptAfterWrite = false;
struct Write { uint8_t page; std::vector<uint8_t> data; };
static std::vector<Write> writes;
static std::vector<uint8_t> reads;
static std::vector<Pn532Result> observed;
Pn532Result RfidUploadSelect(uint8_t* uid, uint8_t& length) {
  ++pnStages; ++selectedStages;
  auto result = present ? Pn532Result::Ok : Pn532Result::NoTarget;
  if (!selections.empty()) { result = selections.front(); selections.pop_front(); }
  length = result == Pn532Result::Ok ? 7 : 0;
  if (length) memcpy(uid, tagUid, length);
  return result;
}
void RfidUploadObserve(Pn532Result result) { observed.push_back(result); }
struct FakeReader {
  const char* phaseName() const { return "fake"; }
  const char* faultName() const { return "fake"; }
  uint8_t lastTagStatus() const { return 0; }
  const char* lastSessionStep() const { return "none"; }
  uint8_t lastSessionResponseLength() const { return 0; }
  uint8_t lastSessionResponseByte(uint8_t) const { return 0; }
  Pn532Result next() { auto result = failNextOperation; failNextOperation = Pn532Result::Ok; return result; }
  Pn532Result getTagVersion(uint8_t* data, const Pn532Deadline& deadline) {
    ++pnStages; assert(deadline.remaining() <= 100); memcpy(data, versionBytes, 8); return next();
  }
  Pn532Result readPages(uint8_t page, uint8_t* data, const Pn532Deadline& deadline) {
    ++pnStages; assert(deadline.remaining() <= 100 && page <= 227); reads.push_back(page);
    const unsigned passwordPage = versionBytes[6] == 0x0F ? 43 : versionBytes[6] == 0x11 ? 133 : 229;
    if (versionBytes[3] == 5) assert(page <= 36 || page == 0xE2);
    else assert(unsigned(page) + 3 < passwordPage); // Never include PWD/PACK in a four-page read.
    memcpy(data, memoryPages[page], 16); return next();
  }
  Pn532Result readNtagI2cSession(uint8_t* data, const Pn532Deadline& deadline) {
    ++pnStages; ++sessionChecks;
    assert(deadline.remaining() <= 250 && versionBytes[3] == 5);
    memcpy(data, sessionBytes, 8); return next();
  }
  Pn532Result writePage(uint8_t page, const uint8_t* data, const Pn532Deadline& deadline) {
    ++pnStages; assert(deadline.remaining() <= 100 && page >= 4 && page <= 39);
    writes.push_back({page, std::vector<uint8_t>(data, data + 4)});
    memcpy(memoryPages[page], data, 4);
    if (corruptAfterWrite && page == 7 && writes.size() > 1) memoryPages[page][0] ^= 1;
    if (writeThenFail) { writeThenFail = false; return Pn532Result::TransportFault; }
    return next();
  }
} pn532;
#define _HAS1_REVIVAL_MACHINE_H_
#include "card_upload.ino"

static void command(const std::string& line) {
  for (unsigned char byte : line) CardUploadInput(byte);
  CardUploadInput('\n');
}
static void tick(uint32_t step = 200) {
  fakeMs += step; unsigned before = pnStages; CardUploadLoop();
  assert(pnStages - before <= 1 && "Only one PN stage allowed per loop");
}
static bool says(const std::string& part) {
  for (const auto& line : output) if (line.find(part) != std::string::npos) return true;
  return false;
}
static void untilStage(UploadStage stage) {
  for (int i = 0; i < 180 && uploadJob != UPLOAD_NONE && uploadStage != stage; ++i) tick();
  assert(uploadJob != UPLOAD_NONE && uploadStage == stage);
}
static void untilDone() {
  for (int i = 0; i < 180 && uploadJob != UPLOAD_NONE; ++i) tick();
  assert(uploadJob == UPLOAD_NONE);
}
static void arm(const char* text = "write G9P2") {
  command(text); assert(uploadJob != UPLOAD_NONE);
  present = false; untilStage(UPLOAD_PRESENT);
  present = true;
}
static void setup() {
  memoryPages[3][0] = 0xE1; memoryPages[3][1] = 0x10; memoryPages[3][2] = 0x12;
  memoryPages[4][0] = 3; memoryPages[4][1] = 0; memoryPages[4][2] = 0xFE;
  memoryPages[41][3] = 0xFF;
  CardUploadInit(); CardUploadConnected(); CardUploadSyncMode("card-upload");
  output.clear();
}
int main(int argc, char** argv) {
  assert(argc == 2); const std::string scenario = argv[1]; setup();
  if (scenario.rfind("i2c_", 0) == 0) {
    const uint8_t model[] = {0, 4, 4, 5, 2, 1, 0x13, 3};
    memcpy(versionBytes, model, 8); memoryPages[3][2] = 0x6D;
    if (scenario == "i2c_plus") versionBytes[5] = 2;
    if (scenario == "i2c_2k") versionBytes[6] = 0x15;
    if (scenario == "i2c_cc") memoryPages[3][2] = 0x12;
    if (scenario == "i2c_static_lock") memoryPages[2][2] = 1;
    if (scenario == "i2c_dynamic_lock") memoryPages[0xE2][1] = 1;
    if (scenario == "i2c_mirror") sessionBytes[0] |= 2;
    if (scenario == "i2c_pass_through") sessionBytes[0] |= 0x40;
    if (scenario == "i2c_rf_readonly" || scenario == "i2c_read_readonly") sessionBytes[0] = 0;
    if (scenario == "i2c_busy") sessionBytes[6] |= 2;
    if (scenario == "i2c_i2c_locked") sessionBytes[6] |= 0x40;
    if (scenario == "i2c_eeprom_error") sessionBytes[6] |= 4;
    if (scenario == "i2c_invalid_session") sessionBytes[6] = 0;
    if (scenario == "i2c_invalid_reserved") sessionBytes[7] = 1;
    // The normal RF-owned state must be accepted.
    if (scenario == "i2c_write") sessionBytes[6] = 0x21;
    bool reading = scenario == "i2c_read" || scenario == "i2c_read_readonly";
    arm(reading ? "read" : "write G9P2");
    if (scenario == "i2c_session_failure") {
      untilStage(UPLOAD_MEMORY); tick(); // Same-UID selection.
      failNextOperation = Pn532Result::TransportFault;
    }
    if (scenario == "i2c_changed_session" || scenario == "i2c_late_session_failure") {
      untilStage(UPLOAD_EMPTY); assert(writes.size() == 1);
      if (scenario == "i2c_changed_session") sessionBytes[0] |= 2;
      else { tick(); failNextOperation = Pn532Result::TransportFault; }
    }
    untilDone();
    if (scenario == "i2c_write") {
      assert(says("MODEL NT3H1101") && says("VERSION=0004040502011303") && says("OK WRITE verified"));
      assert(!memcmp(memoryPages[7], "G9P2", 4));
      const uint8_t empty[5] = {}; assert(!memcmp(memoryPages[4], empty, 5));
      assert(sessionChecks == writes.size() + reads.size() - 2); // Except static + locks.
      assert(reads[0] == 2 && reads[1] == 0xE2);
    } else if (reading) {
      assert(writes.empty() && says("OK READ complete") && sessionChecks == 9);
    } else if (scenario == "i2c_changed_session" || scenario == "i2c_late_session_failure") {
      assert(writes.size() == 1 && says("UNKNOWN") && !says("OK WRITE"));
    } else {
      assert(writes.empty() && says("ERROR") && !says("OK WRITE"));
    }
  } else if (scenario == "write213" || scenario == "write215" || scenario == "write216" || scenario == "factory_tlv") {
    if (scenario == "write215" || scenario == "write216") {
      versionBytes[6] = scenario == "write215" ? 0x11 : 0x13;
      memoryPages[3][2] = scenario == "write215" ? 0x3E : 0x6D;
      memoryPages[scenario == "write215" ? 131 : 227][3] = 0xFF;
    }
    if (scenario == "factory_tlv") { const uint8_t old[] = {1,3,0xA0,0x0C,0x34,3,0,0xFE}; memcpy(memoryPages[4], old, 8); }
    arm(); untilDone(); assert(says("OK WRITE verified"));
    assert(writes.size() >= 5 && writes[0].page == 7 && writes[1].page == 4);
    assert(writes[0].data == std::vector<uint8_t>({0,0,0,0}));
    assert(writes[writes.size()-2].page == 4 && writes.back().page == 7);
    assert(memcmp(memoryPages[7], "G9P2", 4) == 0);
    assert(memcmp(memoryPages[4], uploadImage.bytes, uploadWriteSize) == 0);
    if (scenario == "write213" || scenario == "factory_tlv") {
      const uint8_t expected[] = {1,3,0xA0,0x0C,0x34}; assert(!memcmp(memoryPages[4], expected, 5));
    } else { const uint8_t empty[5] = {}; assert(!memcmp(memoryPages[4], empty, 5)); }
    auto count = writes.size(); for (int i=0;i<100;++i) tick(); assert(writes.size() == count);
  } else if (scenario == "standard_write" || scenario == "long_literal") {
    command("layout standard");
    std::string input;
    if (scenario == "standard_write") { command("format text"); command("template {code}"); input="x"; }
    else input="https://"+std::string(120,'a');
    memcpy(memoryPages[7], "G9P2", 4);
    std::string writeCommand="write "+input; arm(writeCommand.c_str()); untilDone();
    assert(says("OK WRITE verified") && writes.front().page==7 && writes.back().page==7);
    assert(memcmp(memoryPages[4], uploadImage.bytes, uploadWriteSize)==0);
    assert(memcmp(memoryPages[7], "G9P2", 4)!=0);
  } else if (scenario == "read") {
    arm("read"); untilDone(); assert(writes.empty() && says("OK READ complete") && says("page36="));
  } else if (scenario == "held_requires_removal") {
    present = true; command("write G9P2"); for (int i=0;i<20;++i) tick();
    assert(uploadStage == UPLOAD_REMOVE && writes.empty());
    present = false; tick(); fakeMs += 400; selections = {Pn532Result::TransportFault}; tick();
    assert(uploadJob == UPLOAD_NONE && writes.empty());
  } else if (scenario == "uid_change_before" || scenario == "uid_change_after") {
    arm(); untilStage(scenario == "uid_change_before" ? UPLOAD_DYNAMIC : UPLOAD_EMPTY);
    tagUid[6] ^= 1; untilDone();
    assert(says("different UID"));
    assert(says(scenario == "uid_change_before" ? "ERROR different" : "UNKNOWN different"));
    assert(writes.size() == (scenario == "uid_change_before" ? 0 : 1));
  } else if (scenario == "lost_tag" || scenario == "transport_error") {
    arm(); untilStage(UPLOAD_DYNAMIC);
    if (scenario == "lost_tag") present = false;
    else selections = {Pn532Result::TransportFault};
    untilDone(); assert(writes.empty() && says("ERROR tag lost"));
  } else if (scenario == "uncertain_write" || scenario == "verify_mismatch") {
    arm(); untilStage(UPLOAD_INVALIDATE);
    if (scenario == "uncertain_write") writeThenFail = true;
    else corruptAfterWrite = true;
    untilDone(); assert(says("UNKNOWN") && !says("OK WRITE"));
    auto count=writes.size(); for(int i=0;i<20;++i) tick(); assert(writes.size()==count);
  } else if (scenario == "disconnect_before" || scenario == "disconnect_after" || scenario == "mode_change_after" || scenario == "cancel_after") {
    arm(); untilStage(scenario == "disconnect_before" ? UPLOAD_DYNAMIC : UPLOAD_EMPTY);
    auto count = writes.size();
    if (scenario == "mode_change_after") CardUploadSyncMode("open");
    else if (scenario == "cancel_after") command("cancel");
    else CardUploadDisconnected();
    assert(uploadJob == UPLOAD_NONE); for(int i=0;i<10;++i) tick(); assert(writes.size()==count);
    if (scenario != "disconnect_before") assert(std::string(uploadLastResult).find("UNKNOWN") == 0);
    CardUploadConnected(); assert(uploadJob == UPLOAD_NONE && CardUploadBlocksOpen());
  } else if (scenario == "timeout_after_write") {
    arm(); untilStage(UPLOAD_EMPTY); auto count=writes.size();
    fakeMs+=30000; CardUploadLoop();
    assert(uploadJob==UPLOAD_NONE && writes.size()==count && says("UNKNOWN job timed out"));
  } else if (scenario == "timeout" || scenario == "timeout_wrap") {
    if(scenario=="timeout_wrap") fakeMs=uint64_t(UINT32_MAX)-1000;
    command("write G9P2"); present=true; fakeMs+=30000; CardUploadLoop();
    assert(uploadJob==UPLOAD_NONE && writes.empty() && says("timed out"));
  } else if (scenario == "static_lock" || scenario == "dynamic_lock" || scenario == "protected" || scenario == "readonly" || scenario == "mirror" || scenario == "unknown_model" || scenario == "custom_tlv" || scenario == "custom_lock_tlv") {
    if(scenario=="static_lock") memoryPages[2][2]=1;
    if(scenario=="dynamic_lock") memoryPages[40][1]=1;
    if(scenario=="protected") memoryPages[41][3]=4;
    if(scenario=="readonly") memoryPages[3][3]=15;
    if(scenario=="mirror") memoryPages[41][0]=0x40;
    if(scenario=="unknown_model") versionBytes[1]=5;
    if(scenario=="custom_tlv") memoryPages[4][0]=0xFD;
    if(scenario=="custom_lock_tlv") { const uint8_t wrong[]={1,3,0xA0,0x10,0x44,3,0,0xFE}; memcpy(memoryPages[4],wrong,8); }
    arm(); untilDone(); assert(writes.empty() && says("ERROR"));
  } else if (scenario == "exit_gate") {
    CardUploadSyncMode("open"); assert(CardUploadBlocksGameplay() && CardUploadBlocksOpen());
    present=false; for(int i=0;i<5;++i) tick(); assert(CardUploadBlocksOpen());
    CardUploadSyncMode("github"); assert(CardUploadBlocksOpen());
    CardUploadSyncMode("activate"); assert(!CardUploadBlocksOpen() && !CardUploadBlocksGameplay());
  } else if (scenario == "exit_safe_then_unsafe") {
    CardUploadSyncMode("activate"); present=true; tick();
    CardUploadSyncMode("open"); present=false;
    for(int i=0;i<5;++i) { tick(); }
    assert(CardUploadBlocksOpen());
    CardUploadSyncMode("github"); assert(CardUploadBlocksOpen());
    CardUploadSyncMode("activate"); assert(!CardUploadBlocksOpen());
  } else if (scenario == "exit_deferred_recovery") {
    CardUploadSyncMode("activate"); present=false;
    selections={Pn532Result::Deadline}; tick();
    assert(CardUploadBlocksOpen() && observed.empty());
    for(int i=0;i<5;++i) tick();
    assert(!CardUploadBlocksOpen() && observed.empty());
  } else if (scenario == "exit_fault_breaks_removal") {
    CardUploadSyncMode("activate"); present=false; tick(); fakeMs+=500;
    selections={Pn532Result::TransportFault}; tick(); tick(); assert(CardUploadBlocksOpen());
    tick(); assert(CardUploadBlocksOpen()); tick(); assert(!CardUploadBlocksOpen());
  } else if (scenario == "mode_gate") {
    CardUploadSyncMode("ready"); command("write G9P2"); command("read"); command("save");
    assert(uploadJob==UPLOAD_NONE && preferencesWrites==0 && writes.empty());
  } else if (scenario == "line_overflow") {
    command("write G9P2"+std::string(192,' ')); assert(uploadJob==UPLOAD_NONE && says("discarded"));
    command("status"); assert(says("job=none"));
  } else if (scenario == "telnet_parser") {
    const uint8_t negotiation[]={255,251,1,255,250,24,'w','r','i','t','e',' ',255,240};
    for(auto byte:negotiation) CardUploadInput(byte);
    for(auto byte:std::string("write G9P3\b2\r\n")) CardUploadInput(byte);
    assert(uploadJob==UPLOAD_WRITE && std::string(uploadImage.code)=="G9P2");
  } else if (scenario == "settings_persist") {
    command("format text"); command("layout standard"); command("template badge-{code}"); command("save");
    assert(preferencesWrites==1 && uploadSettings.format==CardNdef::Format::Text);
    command("defaults"); assert(uploadSettings.format==CardNdef::Format::Uri);
    CardUploadDisconnected(); CardUploadConnected();
    assert(uploadSettings.format==CardNdef::Format::Text && uploadSettings.layout==CardNdef::Layout::Standard);
    assert(uploadJob==UPLOAD_NONE && std::string(uploadSettings.pattern)=="badge-{code}");
    command("preview hello"); assert(says("badge-hello"));
  } else if (scenario == "invalid_saved") {
    savedPreferences.assign(105,0xFF); CardUploadConnected();
    assert(uploadSettings.format==CardNdef::Format::Uri && uploadJob==UPLOAD_NONE);
  } else if (scenario == "literal_preview") {
    command("prefix https://www."); command("layout standard"); command("preview example.com");
    assert(says("content=https://www.example.com") && writes.empty() && pnStages==0);
  } else assert(false && "unknown scenario");
  for (const auto& write:writes) assert(write.page>=4 && write.page<=39);
  std::cout << "PASS " << scenario << '\n';
}
