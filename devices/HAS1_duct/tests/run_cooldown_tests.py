#!/usr/bin/env python3
"""Compile real firmware functions with host doubles; requires Python 3 and clang++."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def function(source, name):
    match = re.search(r"(?:void|int|bool|Mp3Phrase)\s+" + name + r"\s*\([^)]*\)[^{]*\{", source)
    if not match:
        raise RuntimeError(f"Missing firmware function: {name}")
    start, pos, depth = match.start(), match.end(), 1
    while depth:
        depth += (source[pos] == "{") - (source[pos] == "}")
        pos += 1
    return source[start:pos]


header = (ROOT / "HAS1_duct.h").read_text()
# Actual scalar state declarations, including newly added lifecycle flags.
globals_ = "\n".join(line for line in header.splitlines()
                     if re.match(r"^(?:bool|int|unsigned long|String|GameState)\s+\w+\s*(?:[;=]|\[)", line))
core = (ROOT / "HAS1_duct_function.ino").read_text().replace('#include "HAS1_duct.h"', '')
game = (ROOT / "game_state.ino").read_text()
timer = (ROOT / "timer.ino").read_text()
sensor = (ROOT / "sensor.ino").read_text()
audio = (ROOT / "audio_queue.ino").read_text().replace('#include "HAS1_duct.h"', '')
body = core + "\n" + "\n".join(function(game, n) for n in
    ["ApplyCurrentNeopixel", "EnterTaggerMode", "ExitTaggerMode", "SettingFunc", "ReadyFunc", "ActivateFunc",
     "ActivateRunOnce"])
body += "\n" + function(timer, "CooltimeTimerFunc") + "\n" + function(timer, "CooltimeFinish")
body += "\n" + "\n".join(function(sensor, name) for name in
                           ["CardChecking", "CooltimeMp3", "RemainingTimeMp3", "Mp3PlayLargeFolder"])
prototypes = "\n".join(re.findall(r"^(?:void|int|bool)\s+\w+\([^)]*\)", body, re.M))
prototypes = prototypes.replace("void DuctOpen(bool switch_push)", "void DuctOpen(bool switch_push = false)")
prototypes = ";\n".join(prototypes.splitlines()) + ";\n"
source = (Path(__file__).with_name("host_harness.cpp").read_text()
          .replace("// FIRMWARE_GLOBALS", globals_ + "\n" + prototypes)
          .replace("// DOMAIN_AUDIO_FACTORY",
                   function(audio, "Mp3LanguageFolder") + "\n" + function(audio, "Mp3MakePhrase"))
          .replace("// ACTUAL_AUDIO_FUNCTIONS", audio)
          .replace("// FIRMWARE_FUNCTIONS", body))
cases = ["normal", "block_close_exit", "block_exit_close", "freeze_resume",
         "admin_available", "admin_cooldown", "admin_block_before_close",
         "admin_block_after_close", "admin_inside_block", "normal_admin_override",
         "blocked_button", "tagger_gate", "admin_early_back", "reset_pending_close",
         "cooldown_button_feedback", "blockade_button_feedback",
         "switch_counts", "switch_tag_share_count", "blocked_open_not_counted",
         "server_cooltime_fallback",
         "audio_0", "audio_28", "audio_60", "audio_90",
         "blockade_remaining_audio", "blockade_reentry_audio", "blockade_button_preserves_close",
         "open_audio_paths", "server_activate", "server_activate_door_open", "server_activate_blockade"]
with tempfile.TemporaryDirectory(prefix="duct-cooldown-") as tmp:
    src, exe = Path(tmp) / "test.cpp", Path(tmp) / "test"
    src.write_text(source)
    subprocess.run(["clang++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-I", str(ROOT), str(src), "-o", str(exe)], check=True)
    for case in cases:
        subprocess.run([str(exe), case], check=True)
    audio_cases = ["audio_fifo", "audio_door_timers", "audio_v2_blockade", "audio_overflow", "audio_duplicate",
                   "audio_missing", "audio_wrap", "audio_language", "audio_stale", "audio_folder9_language"]
    audio_main = Path(__file__).with_name("audio_harness.cpp").read_text()
    src.write_text(source[:source.index("int main(int argc")] + audio_main)
    subprocess.run(["clang++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-DACTUAL_AUDIO",
                    "-fsanitize=address,undefined", "-I", str(ROOT), str(src), "-o", str(exe)], check=True)
    for case in audio_cases:
        subprocess.run([str(exe), case], check=True)
print(f"PASS: {len(cases)} firmware cooldown scenarios")
print(f"PASS: {len(audio_cases)} actual audio scheduler scenarios")
