#!/usr/bin/env python3
"""Replay release frames through vk_modify; requires Python 3 and g++.

Run from the recovery source root:
    python3 minuitwrp/tests/test_touch_release.py
"""
# SPDX-License-Identifier: Apache-2.0

from pathlib import Path
import subprocess
import sys
import tempfile


source_path = Path(sys.argv[1]) if len(sys.argv) > 1 else (
    Path(__file__).resolve().parents[2] / "minuitwrp/events.cpp"
)
source = source_path.read_text()
structs = source[source.index("struct virtualkey {"):source.index("static struct pollfd ev_fds")]
helper = source[source.index("static int vk_tp_to_screen("):source.index("/* Translate a virtual key")]
function = source[source.index("static int vk_modify("):source.index("\nint ev_get(")]
prefix = r"""
#include <linux/input.h>
#include <poll.h>
#include <cstdio>
#include <cstring>
#include <iostream>
#define TW_NO_HAPTICS 1
#define ABS_MT_POSITION 0x2a
#define ABS_MT_AMPLITUDE 0x2b
#define LOGI(...) std::fprintf(stderr, __VA_ARGS__)
int gr_fb_width() { return 1080; }
int gr_fb_height() { return 2400; }
static inline int ABS(int value) { return value < 0 ? -value : value; }
"""
main = r"""
int main() {
    struct ev device = {};
    std::strcpy(device.deviceName, "himax-touchscreen");
    device.p.xi.maximum = device.mt_p.xi.maximum = 1079;
    device.p.yi.maximum = device.mt_p.yi.maximum = 2399;
    int type, code, value;
    int releases = 0;
    int release_x = -1, release_y = -1;
    while (std::cin >> type >> code >> value) {
        struct input_event event = {};
        event.type = type;
        event.code = code;
        event.value = value;
        if (!vk_modify(&device, &event) && event.type == EV_ABS && event.code == 0) {
            ++releases;
            release_x = event.value >> 16;
            release_y = event.value & 0xffff;
        }
    }
    std::cout << releases << " " << release_x << " " << release_y << "\n";
}
"""
# These synthetic traces use the event order observed on the panel, with
# neutral coordinates. They contain no device identifiers or captured logs.
cases = {
    "pressure-only release": [
        (3, 53, 100), (3, 54, 200), (3, 48, 20), (3, 58, 20), (0, 0, 0),
        (3, 48, 0), (3, 58, 0), (0, 0, 0),
    ],
    "negative tracking-ID release": [
        (3, 57, 1), (3, 53, 100), (3, 54, 200), (0, 0, 0),
        (3, 57, -1), (0, 0, 0),
    ],
    "Himax negative ID followed by zero pressure": [
        (3, 57, 1), (3, 53, 100), (3, 54, 200), (3, 48, 20), (3, 58, 20), (0, 0, 0),
        (3, 57, -1), (3, 48, 0), (3, 58, 0), (1, 330, 0), (0, 0, 0),
    ],
    "Himax unlock swipe followed by release": [
        (3, 57, 1), (3, 53, 100), (3, 54, 2000), (3, 48, 20), (3, 58, 20),
        (1, 330, 1), (0, 0, 0),
        (3, 53, 400), (0, 0, 0), (3, 53, 950), (0, 0, 0),
        (3, 57, -1), (3, 48, 0), (3, 58, 0), (1, 330, 0), (0, 0, 0),
    ],
}
with tempfile.TemporaryDirectory(prefix="twrp-touch-test-") as directory:
    cpp = Path(directory) / "replay.cpp"
    binary = Path(directory) / "replay"
    cpp.write_text(prefix + structs + helper + function + main)
    subprocess.run(["g++", "-std=c++17", "-O2", str(cpp), "-o", str(binary)], check=True)
    for name, events in cases.items():
        trace = "".join(f"{event_type} {code} {value}\n" for event_type, code, value in events)
        result = tuple(map(int, subprocess.check_output([str(binary)], input=trace, text=True).split()))
        expected = (1, 950, 2000) if "swipe" in name else (1, 100, 200)
        if result != expected:
            raise SystemExit(f"FAIL: {name}: expected {expected}, got {result}")
        print(f"PASS: {name}")
