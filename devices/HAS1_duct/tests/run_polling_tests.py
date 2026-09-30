#!/usr/bin/env python3
"""Exercise real HTTP/polling/ArduinoJson code and real duct transitions with transport doubles."""
from pathlib import Path
import os
import runpy
import re
import subprocess
import tempfile

suite = runpy.run_path(str(Path(__file__).with_name('run_cooldown_tests.py')))
root = suite['ROOT']
include = Path(os.environ.get('ARDUINOJSON_INCLUDE', str(Path.home() / 'Documents/Arduino/libraries/ArduinoJson/src')))
if not (include / 'ArduinoJson.h').is_file():
    raise SystemExit('Set ARDUINOJSON_INCLUDE to ArduinoJson src directory')
source = suite['source'].split('int main(int argc')[0]
a = source.index('struct Value {')
b = source.index('struct Has1BleBeacon', a)
source = source[:a] + '#include <ArduinoJson.h>\nStaticJsonDocument<2048> my, tag; StaticJsonDocument<512> shift_machine;\n' + source[b:]
# Match the library document capacities; use v6-compatible storage for the duct cache.
source = source.replace('static JsonDocument cur;', 'static StaticJsonDocument<2048> cur;')
source = source.replace('} has2wifi;', '};\n// POLLING_DECLARATIONS', 1)
lib = (root.parents[1] / 'libraries/HAS2_Wifi/HAS2_Wifi.cpp').read_text()
functions = '\n'.join(suite['function'](lib, 'HAS2_Wifi::' + name) for name in
                      ['LoopFresh', 'HttpRequest', 'JsonParsingFresh'])
# Host std::string provides the equivalent Arduino String operations.
functions = functions.replace(' + String(httpcode)', ' + std::to_string(httpcode)').replace('HOST_NAME.substring(7)', 'HOST_NAME.substr(7)')
source = source.replace('// POLLING_DECLARATIONS', r'''
struct Response { int status; String body; };
struct Transport {
    std::vector<Response> replies;
    size_t consumed = 0;
    void setReuse(bool) {} void begin(String) {} void end() {}
    int GET() { if (consumed >= replies.size()) std::abort(); return replies[consumed++].status; }
    String getString() { return replies[consumed - 1].body; }
    String errorToString(int) { return "transport failure"; }
} http;
constexpr int HTTP_CODE_OK = 200;
Logger* _has2DebugPrint = &Serial;
int firmwareUpdates = 0;
struct Esp { int restarts = 0; void restart() { ++restarts; } } ESP;
class HAS2_Wifi : public Wifi {
    String server = "http://test/has2.php", my_mac = "test", HOST_NAME = "http://test";
    // FRESH_PENDING_DECLARATION
    bool HttpRequest(String, String, bool = false);
    bool JsonParsingFresh(String, String);
    void JsonParsing(String, String) { std::abort(); } // Fresh path must not use legacy parser.
    void MaintainWifi() {}
    void FirmwareUpdate(String, String) { ++firmwareUpdates; }
public:
    void LoopFresh(void (*Func)(void));
} has2wifi;
''')
# Extract the production initializer so boot behavior cannot drift in the double.
header = (root.parents[1] / 'libraries/HAS2_Wifi/HAS2_Wifi.h').read_text()
source = source.replace('// FRESH_PENDING_DECLARATION',
                        re.search(r'bool freshReceivePending = (?:true|false);', header).group())
source += functions + r'''
int callbacks = 0;
void changed() { ++callbacks; DataChange(); }
void poll(std::vector<Response> responses) {
    http.replies = responses; http.consumed = 0;
    has2wifi.LoopFresh(changed);
    check(http.consumed == responses.size(), "all expected HTTP requests consumed");
}
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    if (String(argv[1]) == "boot_update") {
        // Legacy Setup has already fetched update and consumed the shift flag.
        my["device_name"] = "duct"; my["device_state"] = "update"; my["game_state"] = "ready";
        const Response idle{200, "{\"shift_machine\":0,\"watchdog\":0}"};
        poll({idle, {-1, ""}});
        check(callbacks == 0 && firmwareUpdates == 0, "boot update requires fresh validation");
        poll({idle, {200, "{\"device_name\":\"duct\",\"device_type\":\"duct\",\"device_state\":\"update\",\"game_state\":\"ready\"}"}});
        check(callbacks == 1 && firmwareUpdates == 1, "boot update survives consumed notification");
        poll({idle}); poll({idle});
        check(callbacks == 1 && firmwareUpdates == 1, "idle polls do not repeat callback or update");
        std::cout << "PASS boot update refresh and idle polling\n";
        return 0;
    }
    const bool blocked = String(argv[1]) == "blockade";
    my["device_name"] = "duct"; my["game_state"] = "activate"; my["device_state"] = "activate";
    DataChange(); game_state = activate; cooltime_set = 5; cooltime_add = 0;
    openNormal(); advance(4000);
    if (blocked) EnterTaggerMode();
    const auto previous = my.as<String>();
    auto invariant = [&]() {
        check(callbacks == 0 && !duct_available, "failed poll cannot replay cached activate");
        check(tagger_mode == blocked, "failed poll preserves blockade");
        check(my.as<String>() == previous, "failed response cannot partially replace snapshot");
        check(ESP.restarts == 0, "failed Loop cannot replay cached watchdog");
    };
    shift_machine["shift_machine"] = 1; shift_machine["watchdog"] = 1;
    poll({{-1, ""}}); invariant();
    poll({{500, "server failure"}}); invariant();
    for (const auto* bad : {"", "{\"shift_machine\":1,", "null", "[]", "{}", "{\"shift_machine\":{},\"watchdog\":0}", "{\"shift_machine\":\"bad\",\"watchdog\":0}"}) {
        poll({{200, bad}}); invariant();
    }
    const Response shift{200, "{\"shift_machine\":1,\"watchdog\":0}"};
    poll({shift, {-1, ""}}); invariant();
    poll({shift, {503, "unavailable"}}); invariant();
    for (const auto* bad : {"", "{\"device_state\":\"activate\",\"game_state\":\"activate\",",
                            "null", "[]", "{}", "{\"device_state\":\"activate\"}",
                            "{\"device_state\":7,\"game_state\":\"activate\"}"}) {
        poll({shift, {200, bad}}); invariant();
    }
    const Response activation{200, "{\"device_name\":\"duct\",\"device_state\":\"activate\",\"game_state\":\"activate\"}"};
    // ReceiveMine clears the server notification before delivering its response.
    // Recover the pending snapshot even if the next Loop reports no change.
    poll({{200, "{\"shift_machine\":\"0\",\"watchdog\":\"0\"}"}, activation});
    check(callbacks == 1 && duct_available && !tagger_mode, "fresh repeated activate overrides local cooldown/blockade");
    check(!cooltime_timer.isEnabled(cooltime_timer_id), "fresh activation deletes old timer");
    poll({shift, activation});
    check(callbacks == 2 && duct_available, "repeated fresh activation remains idempotent");
    std::cout << "PASS real polling/JSON failures and recovery: " << argv[1] << '\n';
}
'''
# Arduino String(nullptr) is empty; std::string(nullptr) is undefined.
source = re.sub(r'\(String\)\(const char \*\)\s*(\w+\["[^"\n]+"\])', r'String(\1 | "")', source)
with tempfile.TemporaryDirectory(prefix='duct-polling-') as tmp:
    src, exe = Path(tmp) / 'poll.cpp', Path(tmp) / 'poll'
    src.write_text(source)
    subprocess.run(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-Wno-deprecated-declarations', '-fsanitize=address,undefined',
                    '-I', str(root), '-I', str(include), str(src), '-o', str(exe)], check=True)
    for mode in ['cooldown', 'blockade', 'boot_update']:
        subprocess.run([str(exe), mode], check=True)
