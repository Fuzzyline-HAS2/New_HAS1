#!/usr/bin/env python3
"""Compile real firmware functions with host doubles; requires Python 3 and clang++."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def function(source, name):
    match = re.search(r"(?:void|int|bool)\s+" + name + r"\s*\([^)]*\)[^{]*\{", source)
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
                     if re.match(r"^(?:bool|int|String|GameState)\s+\w+\s*(?:[;=]|\[)", line))
core = (ROOT / "HAS1_duct_function.ino").read_text().replace('#include "HAS1_duct.h"', '')
game = (ROOT / "game_state.ino").read_text()
timer = (ROOT / "timer.ino").read_text()
sensor = (ROOT / "sensor.ino").read_text()
body = core + "\n" + "\n".join(function(game, n) for n in
    ["ApplyCurrentNeopixel", "EnterTaggerMode", "ExitTaggerMode", "SettingFunc", "ReadyFunc"])
body += "\n" + function(timer, "CooltimeTimerFunc") + "\n" + function(sensor, "CardChecking")
prototypes = "\n".join(re.findall(r"^(?:void|int|bool)\s+\w+\([^)]*\)", body, re.M))
prototypes = prototypes.replace("void DuctOpen(bool switch_push)", "void DuctOpen(bool switch_push = false)")
prototypes = ";\n".join(prototypes.splitlines()) + ";\n"
source = (Path(__file__).with_name("host_harness.cpp").read_text()
          .replace("// FIRMWARE_GLOBALS", globals_)
          .replace("// FIRMWARE_FUNCTIONS", prototypes + body))
cases = ["normal", "block_close_exit", "block_exit_close", "freeze_resume",
         "admin_available", "admin_cooldown", "admin_block_before_close",
         "admin_block_after_close", "admin_inside_block", "normal_admin_override",
         "blocked_button", "tagger_gate", "admin_early_back", "reset_pending_close"]
with tempfile.TemporaryDirectory(prefix="duct-cooldown-") as tmp:
    src, exe = Path(tmp) / "test.cpp", Path(tmp) / "test"
    src.write_text(source)
    subprocess.run(["clang++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", str(src), "-o", str(exe)], check=True)
    for case in cases:
        subprocess.run([str(exe), case], check=True)
print(f"PASS: {len(cases)} firmware cooldown scenarios")
