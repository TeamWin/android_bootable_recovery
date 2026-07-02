/*
 * Copyright (C) 2011 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "recovery_ui/ui.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <chrono>
#include <functional>
#include <string>
#include <thread>

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/parseint.h>
#include <android-base/properties.h>
#include <android-base/strings.h>
#include <android-base/stringprintf.h>

#include "minui/minui.h"
#include "otautil/sysutil.h"

using namespace std::chrono_literals;

constexpr int UI_WAIT_KEY_TIMEOUT_SEC = 120;
constexpr const char* BRIGHTNESS_FILE = "/sys/class/leds/lcd-backlight/brightness";
constexpr const char* MAX_BRIGHTNESS_FILE = "/sys/class/leds/lcd-backlight/max_brightness";
constexpr const char* BRIGHTNESS_FILE_SDM = "/sys/class/backlight/panel0-backlight/brightness";
constexpr const char* MAX_BRIGHTNESS_FILE_SDM =
    "/sys/class/backlight/panel0-backlight/max_brightness";
constexpr const char* BRIGHTNESS_FILE_PWM =
    "/sys/class/backlight/pwm-backlight.0/brightness";
constexpr const char* MAX_BRIGHTNESS_FILE_PWM =
    "/sys/class/backlight/pwm-backlight.0/max_brightness";

constexpr int kDefaultTouchLowThreshold = 50;
constexpr int kDefaultTouchHighThreshold = 90;

RecoveryUI::RecoveryUI()
    : brightness_normal_(50),
      brightness_dimmed_(25),
      brightness_file_(BRIGHTNESS_FILE),
      max_brightness_file_(MAX_BRIGHTNESS_FILE),
      touch_screen_allowed_(false),
      fastbootd_logo_enabled_(false),
      touch_low_threshold_(android::base::GetIntProperty("ro.recovery.ui.touch_low_threshold",
                                                         kDefaultTouchLowThreshold)),
      touch_high_threshold_(android::base::GetIntProperty("ro.recovery.ui.touch_high_threshold",
                                                          kDefaultTouchHighThreshold)),
      key_interrupted_(false),
      key_queue_len(0),
      key_last_down(-1),
      key_long_press(false),
      key_down_count(0),
      enable_reboot(true),
      consecutive_power_keys(0),
      has_power_key(false),
      has_up_key(false),
      has_down_key(false),
      has_touch_screen(false),
      touch_slot_(0),
      is_bootreason_recovery_ui_(false),
      screensaver_state_(ScreensaverState::DISABLED) {
  memset(key_pressed, 0, sizeof(key_pressed));
}

RecoveryUI::~RecoveryUI() {
  ev_exit();
input_thread_stopped_ = true;
  if (input_thread_.joinable()) {
    input_thread_.join();
  }
#ifdef TW_SAMSUNG_TSP_TOUCH_FIX
  tsp_watchdog_stopped_ = true;
  if (tsp_watchdog_thread_.joinable()) {
    tsp_watchdog_thread_.join();
  }
#endif
}

void RecoveryUI::OnKeyDetected(int key_code) {
  if (key_code == KEY_POWER) {
    has_power_key = true;
  } else if (key_code == KEY_DOWN || key_code == KEY_VOLUMEDOWN) {
    has_down_key = true;
  } else if (key_code == KEY_UP || key_code == KEY_VOLUMEUP) {
    has_up_key = true;
  } else if (key_code == ABS_MT_POSITION_X || key_code == ABS_MT_POSITION_Y) {
    has_touch_screen = true;
  }
}

bool RecoveryUI::InitScreensaver() {
  // Disabled.
  if (brightness_normal_ == 0 || brightness_dimmed_ > brightness_normal_) {
    return false;
  }
  if (access(brightness_file_.c_str(), R_OK | W_OK)) {
    if (!access(BRIGHTNESS_FILE_SDM, R_OK | W_OK)) {
      brightness_file_ = BRIGHTNESS_FILE_SDM;
    } else {
      brightness_file_ = BRIGHTNESS_FILE_PWM;
    }
  }

  if (access(max_brightness_file_.c_str(), R_OK)) {
    if (!access(MAX_BRIGHTNESS_FILE_SDM, R_OK)) {
      max_brightness_file_ = MAX_BRIGHTNESS_FILE_SDM;
    } else {
      max_brightness_file_ = MAX_BRIGHTNESS_FILE_PWM;
    }
  }
  // Set the initial brightness level based on the max brightness. Note that reading the initial
  // value from BRIGHTNESS_FILE doesn't give the actual brightness value (bullhead, sailfish), so
  // we don't have a good way to query the default value.
  std::string content;
  if (!android::base::ReadFileToString(max_brightness_file_, &content)) {
    PLOG(WARNING) << "Failed to read max brightness";
    return false;
  }

  unsigned int max_value;
  if (!android::base::ParseUint(android::base::Trim(content), &max_value)) {
    LOG(WARNING) << "Failed to parse max brightness: " << content;
    return false;
  }

  brightness_normal_value_ = max_value * brightness_normal_ / 100.0;
  brightness_dimmed_value_ = max_value * brightness_dimmed_ / 100.0;
  if (!android::base::WriteStringToFile(std::to_string(brightness_normal_value_),
                                        brightness_file_)) {
    PLOG(WARNING) << "Failed to set brightness";
    return false;
  }

  LOG(INFO) << "Brightness: " << brightness_normal_value_ << " (" << brightness_normal_ << "%)";
  screensaver_state_ = ScreensaverState::NORMAL;
  return true;
}

bool RecoveryUI::Init(const std::string& /* locale */) {
  ev_init(std::bind(&RecoveryUI::OnInputEvent, this, std::placeholders::_1, std::placeholders::_2),
          touch_screen_allowed_);

  ev_iterate_available_keys(std::bind(&RecoveryUI::OnKeyDetected, this, std::placeholders::_1));

  if (touch_screen_allowed_) {
    ev_iterate_touch_inputs(std::bind(&RecoveryUI::OnKeyDetected, this, std::placeholders::_1));

    // Parse /proc/cmdline to determine if it's booting into recovery with a bootreason of
    // "recovery_ui". This specific reason is set by some (wear) bootloaders, to allow an easier way
    // to turn on text mode. It will only be set if the recovery boot is triggered from fastboot, or
    // with 'adb reboot recovery'. Note that this applies to all build variants. Otherwise the text
    // mode will be turned on automatically on debuggable builds, even without a swipe.
    std::string cmdline;
    if (android::base::ReadFileToString("/proc/cmdline", &cmdline)) {
      is_bootreason_recovery_ui_ = cmdline.find("bootreason=recovery_ui") != std::string::npos;
    } else {
      // Non-fatal, and won't affect Init() result.
      PLOG(WARNING) << "Failed to read /proc/cmdline";
    }
  }

  if (!InitScreensaver()) {
    LOG(INFO) << "Screensaver disabled";
  }

  // Create a separate thread that handles input events.
  input_thread_ = std::thread([this]() {
    while (!this->input_thread_stopped_) {
      if (!ev_wait(500)) {
        ev_dispatch();
      }
    }
  });
#ifdef TW_SAMSUNG_TSP_TOUCH_FIX
  // Watchdog thread to recover Samsung TSP touch after random mid-session failures.
  tsp_watchdog_stopped_ = false;
  tsp_watchdog_thread_ = std::thread([this]() {
    // Wait for TSP to fully initialise before monitoring
    std::this_thread::sleep_for(std::chrono::seconds(10));
    while (!this->tsp_watchdog_stopped_) {
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      SamsungTSPTouchFix();
    }
  });
#endif
  return true;
}

void RecoveryUI::OnTouchDetected(int dx, int dy) {
  enum SwipeDirection { UP, DOWN, RIGHT, LEFT } direction;

  // We only consider a valid swipe if:
  // - the delta along one axis is below touch_low_threshold_;
  // - and the delta along the other axis is beyond touch_high_threshold_.
  if (abs(dy) < touch_low_threshold_ && abs(dx) > touch_high_threshold_) {
    direction = dx < 0 ? SwipeDirection::LEFT : SwipeDirection::RIGHT;
  } else if (abs(dx) < touch_low_threshold_ && abs(dy) > touch_high_threshold_) {
    direction = dy < 0 ? SwipeDirection::UP : SwipeDirection::DOWN;
  } else {
    LOG(DEBUG) << "Ignored " << dx << " " << dy << " (low: " << touch_low_threshold_
               << ", high: " << touch_high_threshold_ << ")";
    return;
  }

  // Allow turning on text mode with any swipe, if bootloader has set a bootreason of recovery_ui.
  if (is_bootreason_recovery_ui_ && !IsTextVisible()) {
    ShowText(true);
    return;
  }

  LOG(DEBUG) << "Swipe direction=" << direction;
  switch (direction) {
    case SwipeDirection::UP:
      ProcessKey(KEY_UP, 1);  // press up key
      ProcessKey(KEY_UP, 0);  // and release it
      break;

    case SwipeDirection::DOWN:
      ProcessKey(KEY_DOWN, 1);  // press down key
      ProcessKey(KEY_DOWN, 0);  // and release it
      break;

    case SwipeDirection::LEFT:
    case SwipeDirection::RIGHT:
      ProcessKey(KEY_POWER, 1);  // press power key
      ProcessKey(KEY_POWER, 0);  // and release it
      break;
  };
}

int RecoveryUI::OnInputEvent(int fd, uint32_t epevents) {
  struct input_event ev;
  if (ev_get_input(fd, epevents, &ev) == -1) {
    return -1;
  }

  // Touch inputs handling.
  //
  // We handle the touch inputs by tracking the position changes between initial contacting and
  // upon lifting. touch_start_X/Y record the initial positions, with touch_finger_down set. Upon
  // detecting the lift, we unset touch_finger_down and detect a swipe based on position changes.
  //
  // Per the doc Multi-touch Protocol at below, there are two protocols.
  // https://www.kernel.org/doc/Documentation/input/multi-touch-protocol.txt
  //
  // The main difference between the stateless type A protocol and the stateful type B slot protocol
  // lies in the usage of identifiable contacts to reduce the amount of data sent to userspace. The
  // slot protocol (i.e. type B) sends ABS_MT_TRACKING_ID with a unique id on initial contact, and
  // sends ABS_MT_TRACKING_ID -1 upon lifting the contact. Protocol A doesn't send
  // ABS_MT_TRACKING_ID -1 on lifting, but the driver may additionally report BTN_TOUCH event.
  //
  // For protocol A, we rely on BTN_TOUCH to recognize lifting, while for protocol B we look for
  // ABS_MT_TRACKING_ID being -1.
  //
  // Touch input events will only be available if touch_screen_allowed_ is set.

  if (ev.type == EV_SYN) {
    if (touch_screen_allowed_ && ev.code == SYN_REPORT) {
      // There might be multiple SYN_REPORT events. We should only detect a swipe after lifting the
      // contact.
      if (touch_finger_down_ && !touch_swiping_) {
        touch_start_X_ = touch_X_;
        touch_start_Y_ = touch_Y_;
        touch_swiping_ = true;
      } else if (!touch_finger_down_ && touch_swiping_) {
        touch_swiping_ = false;
        OnTouchDetected(touch_X_ - touch_start_X_, touch_Y_ - touch_start_Y_);
      }
    }
    return 0;
  }

  if (ev.type == EV_REL) {
    if (ev.code == REL_Y) {
      // accumulate the up or down motion reported by
      // the trackball.  When it exceeds a threshold
      // (positive or negative), fake an up/down
      // key event.
      rel_sum += ev.value;
      if (rel_sum > 3) {
        ProcessKey(KEY_DOWN, 1);  // press down key
        ProcessKey(KEY_DOWN, 0);  // and release it
        rel_sum = 0;
      } else if (rel_sum < -3) {
        ProcessKey(KEY_UP, 1);  // press up key
        ProcessKey(KEY_UP, 0);  // and release it
        rel_sum = 0;
      }
    }
  } else {
    rel_sum = 0;
  }

  if (touch_screen_allowed_ && ev.type == EV_ABS) {
    if (ev.code == ABS_MT_SLOT) {
      touch_slot_ = ev.value;
    }
    // Ignore other fingers.
    if (touch_slot_ > 0) return 0;

    switch (ev.code) {
      case ABS_MT_POSITION_X:
        touch_X_ = ev.value;
        touch_finger_down_ = true;
        break;

      case ABS_MT_POSITION_Y:
        touch_Y_ = ev.value;
        touch_finger_down_ = true;
        break;

      case ABS_MT_TRACKING_ID:
        // Protocol B: -1 marks lifting the contact.
        if (ev.value < 0) touch_finger_down_ = false;
        break;
    }
    return 0;
  }

  if (ev.type == EV_KEY && ev.code <= KEY_MAX) {
    if (touch_screen_allowed_) {
      if (ev.code == BTN_TOUCH) {
        // A BTN_TOUCH with value 1 indicates the start of contact (protocol A), with 0 means
        // lifting the contact.
        touch_finger_down_ = (ev.value == 1);
      }

      // Intentionally ignore BTN_TOUCH and BTN_TOOL_FINGER, which would otherwise trigger
      // additional scrolling (because in ScreenRecoveryUI::ShowFile(), we consider keys other than
      // KEY_POWER and KEY_UP as KEY_DOWN).
      if (ev.code == BTN_TOUCH || ev.code == BTN_TOOL_FINGER) {
        return 0;
      }
    }

    ProcessKey(ev.code, ev.value);
  }

  // For Lid switch handle
  if (ev.type == EV_SW) {
    SetSwCallback(ev.code, ev.value);
  }

  return 0;
}

// Processes a key-up or -down event. A key is "registered" when it is pressed and then released,
// with no other keypresses or releases in between. Registered keys are passed to CheckKey() to
// see if it should trigger a visibility toggle, an immediate reboot, or be queued to be processed
// next time the foreground thread wants a key (eg, for the menu).
//
// We also keep track of which keys are currently down so that CheckKey() can call IsKeyPressed()
// to see what other keys are held when a key is registered.
//
// updown == 1 for key down events; 0 for key up events
void RecoveryUI::ProcessKey(int key_code, int updown) {
  bool register_key = false;
  bool long_press = false;

  {
    std::lock_guard<std::mutex> lg(key_press_mutex);
    key_pressed[key_code] = updown;
    if (updown) {
      ++key_down_count;
      key_last_down = key_code;
      key_long_press = false;
      std::thread time_key_thread(&RecoveryUI::TimeKey, this, key_code, key_down_count);
      time_key_thread.detach();
    } else {
      if (key_last_down == key_code) {
        long_press = key_long_press;
        register_key = true;
      }
      key_last_down = -1;
    }
  }

  bool reboot_enabled = enable_reboot;
  if (register_key) {
    switch (CheckKey(key_code, long_press)) {
      case RecoveryUI::IGNORE:
        break;

      case RecoveryUI::TOGGLE:
        ShowText(!IsTextVisible());
        break;

      case RecoveryUI::REBOOT:
        if (reboot_enabled) {
          Reboot("userrequested,recovery,ui");
        }
        break;

      case RecoveryUI::ENQUEUE:
        EnqueueKey(key_code);
        break;
    }
  }
}

void RecoveryUI::TimeKey(int key_code, int count) {
  std::this_thread::sleep_for(750ms);  // 750 ms == "long"
  bool long_press = false;
  {
    std::lock_guard<std::mutex> lg(key_press_mutex);
    if (key_last_down == key_code && key_down_count == count) {
      long_press = key_long_press = true;
    }
  }
  if (long_press) KeyLongPress(key_code);
}

void RecoveryUI::EnqueueKey(int key_code) {
  std::lock_guard<std::mutex> lg(key_queue_mutex);
  const int queue_max = sizeof(key_queue) / sizeof(key_queue[0]);
  if (key_queue_len < queue_max) {
    key_queue[key_queue_len++] = key_code;
    key_queue_cond.notify_one();
  }
}

#ifdef TW_SAMSUNG_TSP_TOUCH_FIX
New chat
Search chats
Images
New
Library
New notebook
Cannot Access Shared Link
Android Work Apps Volume Fix
Silence Work Profile, Keep Personal Loud
Microsoft Teams on Linux
Earn Google Play Points Guide
Android Beta Updates With Unlocked Bootloader
Setting Up "Hey Google" Voice Activation
Git Script Fix: Branch Name
`twrpdtgen` Vendor Boot Image Support
Acknowledged and Ready
Android 17 QPR1 Beta 7 Release
IMS on Pixel Treble GSIs
Termux Firmware Extraction Troubleshooting
Fixing Debian Package Installation Errors
USB-C Audio and Charge Adapter
Identifying a Cricket-Like Sound
Identify Sound: Cricket or Other?
Pixel Phones With Temperature Sensors
WhatsApp Usernames Rolling Out Now
Phone Unlock Request
Root Pixel on Chromebook Guide
Building Remotely with Crave.io
Arm Hair Regrowth After Waxing
Crave AOSP Queue Times
Using Crave for AOSP Builds
Swapping Phones on WhatsApp Calls
Honor MagicOS 11 Release Details
Redirecting Offensive Language
Installing Android Games on Child's Tablet
Linux .repo Directory Creation Issues
TWRP 12.1 Manifest Size
Labour Leadership Race Frontrunners
Rebasing TWRP to AOSP 16
Joining Honor Beta China with VPN
The honor club on the my honor app is not updated and I can't find the honor community app
Honor Club App Issues and Solutions
MagicOS 11 Beta Release Tracking
Git Remote Add Command Error
Adding Git Remote Repository
Android App Bubbles Limit Explained
Android Beta Play System Update Delay
Spelling "Suka Belut"
Android Device Battery Tracking Widget
Android 17 QPR1 Beta 6 Release
Samsung Chromebook Celeron Core Count
Android 17 Canary vs. QPR1
Honor Agentic OS: AI-Native Workspace
Unlock Pixel Bootloader: Step-by-Step Guide
GitHub Markdown Table Ticks
Install GSI on Google Pixel
Honor Android 17 Beta Rollout Timeline
Android Beta Program Exit Guide
Pixel Issue Tracker Mobile Options
Best Open Source Android Note Apps
Top Free Open-Source Android Apps
Git Remote Add Fork Command
Android 17 QPR1 Blur UI
Pixel 10a Charging Speed
Pixel 10 MagSafe Compatibility Explained
Android 17 QPR1 Stable Release
Install Linux on Chromebook: Two Ways
Honor May Update Status
No Flipping Confirmed
Hot Dog Dinner Versatility
Device Tree Build Error: JSON Parsing
JSON Parsing Error in Build
India-South Korea Strategic Partnership
TWRP Build Fails With Errors
LineageOS Device Tree Missing Components
Cozy Blanket Assistance
FAILED: In file included from build/make/core/main.mk:1193: build/make/core/Makefile:148: error: VINTF metadata found in PRODUCT_COPY_FILES: vendor/samsung/m55xq/proprietary/vendor/etc/vintf/manifest_cape.xml:system/vendor/etc/vintf/manifest_cape.xml, use DEVICE_MANIFEST_FILE / DEVICE_MATRIX_FILE / vintf_compatibility_matrix / vintf_fragments instead!.
TWRP Excludes Space Savings Inquiry
Debugging Missing Build Rule Error
Reducing TWRP Recovery Image Size
Shrinking TWRP Recovery Image Size
things needed for xiaomis mtk twrp (keymint 2.0)
[31mERROR: files are incompatible: Device manifest and framework compatibility matrix are incompatible: Sepolicy version 202504 doesn't satisify the requirements.
[ 46% 51/109 6s remaining] //system/sepolicy/contexts:vendor_hwservice_contexts_test running contexts test: vendor_hwservice_contexts_test [common] FAILED: out/soong/.intermediates/system/sepolicy/contexts/vendor_hwservice_contexts_test/android_common/timestamp out/host/linux-x86/bin/checkfc -e -l out/soong/.intermediates/system/sepolicy/precompiled_sepolicy/android_common/a13ve/precompiled_sepolicy out/soong/.intermediates/system/sepolicy/contexts/vendor_hwservice_contexts/android_common/a13ve/vendor_hwservice_contexts && touch out/soong/.intermediates/system/sepolicy/contexts/vendor_hwservice_contexts_test/android_common/timestamp # hash of input list: a48744ab095cfaceaacb026f9a59f84307004cc0c8367bd97e2409f92164ff0d libsepol.context_from_record: type hal_graphics_allocator is not defined libsepol.context_from_record: could not create context structure out/soong/.intermediates/system/sepolicy/contexts/vendor_hwservice_contexts/android_common/a13ve/vendor_hwservice_contexts:  line 5 has invalid context u:object_r:hal_graphics_allocator:s0 Error: could not load context file from out/soong/.intermediates/system/sepolicy/contexts/vendor_hwservice_contexts/android_common/a13ve/vendor_hwservice_contexts [ 47% 52/109 6s remaining] //system/sepolicy/contexts:merged_hwservice_contexts_test running contexts test: merged_hwservice_contexts_test [common] FAILED: out/soong/.intermediates/system/sepolicy/contexts/merged_hwservice_contexts_test/android_common/timestamp out/host/linux-x86/bin/checkfc -e -l out/soong/.intermediates/system/sepolicy/precompiled_sepolicy/android_common/a13ve/precompiled_sepolicy out/soong/.intermediates/system/sepolicy/contexts/merged_hwservice_contexts/android_common/merged_hwservice_contexts && touch out/soong/.intermediates/system/sepolicy/contexts/merged_hwservice_contexts_test/android_common/timestamp # hash of input list: 5944f74dd5e5c57642b3766177bfdc33d238d96bfd870acf42a98fa58debf77b libsepol.context_from_record: type hal_graphics_allocator is not defined libsepol.context_from_record: could not create context structure out/soong/.intermediates/system/sepolicy/contexts/merged_hwservice_contexts/android_common/merged_hwservice_contexts:  line 92 has invalid context u:object_r:hal_graphics_allocator:s0 Error: could not load context file from out/soong/.intermediates/system/sepolicy/contexts/merged_hwservice_contexts/android_common/merged_hwservice_contexts [ 48% 53/109 6s remaining] build out/target/product/a13ve/obj/PACKAGING/check_vintf_all_intermediates/check_vintf_compatible.log FAILED: out/target/product/a13ve/obj/PACKAGING/check_vintf_all_intermediates/check_vintf_compatible.log /target/product/a13ve/system/etc/vintf/manifest/manifest_services_android.frameworks.location.xml out/target/product/a13ve/system/etc/vintf/manifest/manifest_services_android.frameworks.vibrator.xml out/target/product/a13ve/system_ext/etc/vintf/manifest.xml out/target/product/a13ve/system_ext/etc/vintf/manifest/android.hidl.allocator@1.0-service.xml out/target/product/a13ve/vendor/etc/vintf/compatibility_matrix.xml out/target/product/a13ve/vendor/etc/vintf/manifest.xml out/target/product/a13ve/vendor/etc/vintf/manifest/android.hardware.cas@1.2-service.xml out/target/product/a13ve/vendor/etc/vintf/manifest/android.hardware.drm-service.clearkey.xml out/target/product/a13ve/vendor/etc/vintf/manifest/android.hardware.health@2.1.xml out/target/product/a13ve/vendor/etc/vintf/manifest/bluetooth_audio.xml out/target/product/a13ve/vendor/etc/vintf/manifest/deviceManifest.xml out/target/product/a13ve/vendor/etc/vintf/manifest/engmode_manifest.xml out/target/product/a13ve/vendor/etc/vintf/manifest/manifest_android.hardware.drm@1.4-service.widevine.xml out/target/product/a13ve/vendor/etc/vintf/manifest/manifest_hwcomposer.xml out/target/product/a13ve/vendor/etc/vintf/manifest/nxp.android.hardware.nfc@1.2-service.xml out/target/product/a13ve/vendor/etc/vintf/manifest/sec_c2_manifest_default0_1_2.xml out/target/product/a13ve/vendor/etc/vintf/manifest/vaultkeeper_manifest.xml out/target/product/a13ve/vendor/etc/vintf/manifest/vendor.samsung.hardware.radio.exclude.mediatek.xml out/target/product/a13ve/vendor/etc/vintf/manifest/vendor.samsung.hardware.radio.exclude_ship.mediatek.xml out/target/product/a13ve/vendor/etc/vintf/manifest/vendor.samsung.hardware.radio_manifest_2_30.xml out/target/product/a13ve/vendor/etc/vintf/manifest/vendor.samsung.hardware.security.fkeymaster-service.xml out/target/product/a13ve/vendor/etc/vintf/manifest/vendor.samsung.hardware.security.widevine.keyprov-service.xml out/target/product/a13ve/vendor/etc/vintf/manifest/vendor.samsung.hardware.sehradio_manifest_2_30.xml out/target/product/a13ve/vendor/etc/vintf/manifest/vendor.samsung.hardware.thermal@1.0-manifest.xml out/target/product/a13ve/vendor/etc/vintf/manifest/vendor.samsung.hardware.tlc.iccc@1.0-manifest.xml out/target/product/a13ve/vendor/etc/vintf/manifest/vendor.samsung.hardware.tlc.kg@1.1-manifest.xml out/target/product/a13ve/vendor/etc/vintf   out/target/product/a13ve/system/etc/vintf/compatibility_matrix.8.xml   out/target/product/a13ve/system/etc/vintf/compatibility_matrix.device.xml   out/target/product/a13ve/system/etc/vintf/manifest.xml   out/target/product/a13ve/system/etc/vintf/manifest/android.frameworks.stats-service.xml   out/target/product/a13ve/system/etc/vintf/manifest/android.system.keystore2-service.xml   out/target/product/a13ve/system/etc/vintf/manifest/android.system.net.netd-service.xml   out/target/product/a13ve/system/etc/vintf/manifest/android.system.suspend-service.xml   out/target/product/a13ve/system/etc/vintf/manifest/android.system.vold-service.xml   out/target/product/a13ve/system/etc/vintf/manifest/manifest_android.frameworks.cameraservice.service.xml   out/target/product/a13ve/system/etc/vintf/manifest/manifest_media_c2_software.xml   out/target/product/a13ve/system/etc/vintf/manifest/manifest_services_android.frameworks.devicestate.xml   out/target/product/a13ve/system/etc/vintf/manifest/manifest_services_android.frameworks.location.xml   out/target/product/a13ve/system/etc/vintf/manifest/manifest_services_android.frameworks.vibrator.xml   out/target/pr   out/target/product/a13ve/obj/PACKAGING/check_vintf_all_intermediates/kernel_version.txt Args:    --kernel out/target/product/a13ve/obj/PACKAGING/check_vintf_all_intermediates/kernel_version.txt:out/target/product/a13ve/obj/PACKAGING/check_vintf_all_intermediates/kernel_configs.txt --dirmap /system:out/target/product/a13ve/system --dirmap /vendor:out/target/product/a13ve/vendor --dirmap /odm:out/target/product/a13ve/odm --dirmap /product:out/target/product/a13ve/product --dirmap /system_ext:out/target/product/a13ve/system_ext --dirmap /apex:out/target/product/a13ve/obj/PACKAGING/check_vintf_all_intermediates/apex  --property ro.product.first_api_level=31 For ODM SKU = EMPTY_ODM_SKU_PLACEHOLDER, vendor SKU = EMPTY_VENDOR_SKU_PLACEHOLDER [INFO] Cannot parse "out/target/product/a13ve/obj/PACKAGING/check_vintf_all_intermediates/kernel_version.txt" as kernel version, parsing as GKI kernel release. [INFO] Cannot parse "out/target/product/a13ve/obj/PACKAGING/check_vintf_all_intermediates/kernel_version.txt" as GKI kernel release, parsing as kernel release [INFO] Cannot parse "out/target/product/a13ve/obj/PACKAGING/check_vintf_all_intermediates/kernel_version.txt" as kernel release [INFO] Cannot parse "4.19.191-28701252-abA137FXXS9EYE2" as kernel version, parsing as GKI kernel release. [INFO] Cannot parse "4.19.191-28701252-abA137FXXS9EYE2" as GKI kernel release, parsing as kernel release [INFO] "4.19.191-28701252-abA137FXXS9EYE2" is not a mainline kernel.                                                                                                          [INFO] Successfully parsed content of out/target/product/a13ve/obj/PACKAGING/check_vintf_all_intermediates/kernel_version.txt: 4.19.191-28701252-abA137FXXS9 [INFO] List 'out/target/product/a13ve/obj/PACKAGING/check_vintf_all_intermediates/apex/com.android.adbd/etc/vintf/': NAME_NOT_FOUND [INFO] List 'out/target/product/a13ve/obj/PACKAGING/check_vintf_all_intermediates/apex/com.android.adservices/etc/vintf/': NAME_NOT_FOUND [INFO] List 'out/target/product/a13ve/obj/PACKAGING/check_vintf_all_intermediates/apex/com.android.apex.cts.shim/etc/vintf/': NAME_NOT_FOUND [INFO] List 'out/target/product/a13ve/obj/PACKAGING/check_vintf_all_intermediates/apex/com.android.appsearch/etc/vintf/': NAME_NOT_FOUND [INFO] List 'out/target/product/a13ve/obj/PACKAGING/check_vintf_all_intermediates/apex/com.android.art/etc/vintf/': NAME_NOT_FOUND [INFO] List 'out/target/product/a13ve/obj/PACKAGING/check_vintf_all_intermediates/apex/com.android.bt/etc/vintf/': NAME_NOT_FOUND [INFO] List 'out/target/product/a13ve/obj/PACKAGING/check_vintf_all_intermediates/apex/com.android.cellbroadcast/etc/vintf/': NAME_NOT_FOUND [INFO] List 'out/target/product/a13ve/obj/PACKAGING/check_vintf_all_intermediates/apex/com.android.configinfrastructure/etc/vintf/': NAME_NOT_FOUND [INFO] List 'out/target/product/a13ve/obj/PACKAGING/check_vintf_all_intermediates/apex/com.android.conscrypt/etc/vintf/': NAME_NOT_FOUND [INFO] List 'out/target/product/a13ve/obj/PACKAGING/check_vintf_all_intermediates/apex/com.android.crashrecovery/etc/vintf/': NAME_NOT_FOUND [INFO] List 'out/target/product/a13ve/obj/PACKAGING/check_vintf_all_intermediates/apex/com.android.devicelock/etc/vintf/': NAME_NOT_FOUND [INFO] List 'out/target/product/a13ve/obj/PACKAGING/check_vintf_all_intermediates/apex/com.android.extservices/etc/vintf/': NAME_NOT_FOUND [INFO] List 'out/target/product/a13ve/obj/PACKAGING/check_vintf_all_intermediates/apex/com.android.healthfitness/etc/vintf/': NAME_NOT_FOUND [INFO] List 'out/target/product/a13ve/obj/PACKAGING/check_vintf_all_intermediates/apex/com.android.i18n [INFO] List 'out/target/product/a13ve/obj/PACKAGING/check_vintf_all_intermediates/apex/com.android.wifi/etc/vintf/': NAME_NOT_FOUND [INFO] Sysprop apex.all.ready is missing, default to '' [INFO] Get modified time 'out/target/product/a13ve/obj/PACKAGING/check_vintf_all_intermediates/apex/apex-info-list.xml': OK [INFO] getDeviceHalManifest: Reading VINTF information. [INFO] Sysprop ro.boot.product.vendor.sku= [INFO] Fetch 'out/target/product/a13ve/vendor/etc/vintf/manifest.xml': OK [INFO] Sysprop apex.all.ready is missing, default to '' [INFO] Fetch 'out/target/product/a13ve/obj/PACKAGING/check_vintf_all_intermediates/apex/apex-info-list.xml': OK [INFO] Loaded APEX Infos from /apex/apex-info-list.xml [INFO] List 'out/target/product/a13ve/vendor/etc/vintf/manifest/': OK [INFO] Fetch 'out/target/product/a13ve/vendor/etc/vintf/manifest/vaultkeeper_manifest.xml': OK [INFO] Fetch 'out/target/product/a13ve/vendor/etc/vintf/manifest/android.hardware.drm-service.clearkey.xml': OK [INFO] Fetch 'out/target/product/a13ve/vendor/etc/vintf/manifest/vendor.samsung.hardware.security.widevine.keyprov-service.xml': OK [INFO] Fetch 'out/target/product/a1 [INFO] Sysprop apex.all.ready is missing, default to '' [INFO] Get modified time 'out/target/product/a13ve/obj/PACKAGING/check_vintf_all_intermediates/apex/apex-info-list.xml': OK [INFO] Sysprop apex.all.ready is missing, default to '' [INFO] Get modified time 'out/target/product/a13ve/obj/PACKAGING/check_vintf_all_intermediates/apex/apex-info-list.xml': OK [INFO] fetched kernel level from RuntimeInfo '' [INFO] Sysprop apex.all.ready is missing, default to '' [INFO] Get modified time 'out/target/product/a13ve/obj/PACKAGING/check_vintf_all_intermediates/apex/apex-info-list.xml': OK [INFO] getFrameworkCompatibilityMatrix: Reading VINTF information. [INFO] List 'out/target/product/a13ve/system/etc/vintf/': OK [INFO] Fetch 'out/target/product/a13ve/system/etc/vintf/compatibility_matrix.7.xml': OK [INFO] Fetch 'out/target/product/a13ve/system/etc/vintf/compatibility_matrix.device.xml': OK [INFO] Fetch 'out/target/product/a13ve/system/etc/vintf/compatibility_matrix.202404.xml': OK [INFO] Fetch 'out/target/product/a13ve/system/etc/vintf/manifest.xml': OK [INFO] Fetch [INFO] Fetch 'out/target/product/a13ve/system/etc/vintf/compatibility_matrix.202404.xml': OK [INFO] Fetch 'out/target/product/a13ve/system/etc/vintf/manifest.xml': OK [INFO] Fetch 'out/target/product/a13ve/system/etc/vintf/compatibility_matrix.6.xml': OK                                                                                       [INFO] Fetch 'out/target/product/a13ve/system/etc/vintf/compatibility_matrix.5.xml': OK [INFO] Fetch 'out/target/product/a13ve/system/etc/vintf/compatibility_matrix.202504.xml': OK [INFO] Fetch 'out/target/product/a13ve/system/etc/vintf/compatibility_matrix.8.xml': OK [INFO] List 'out/target/product/a13ve/system_ext/etc/vintf/': OK [INFO] Fetch 'out/target/product/a13ve/system_ext/etc/vintf/manifest.xml': OK [INFO] List 'out/target/product/a13ve/product/etc/vintf/': OK [INFO] Fetch 'out/target/product/a13ve/product/etc/vintf/compatibility_matrix.lineage.xml': OK [INFO] Sysprop apex.all.ready is missing, default to '' [INFO] Get modified time 'out/target/product/a13ve/obj/PACKAGING/check_vintf_all_intermediates/apex/apex-info-list.xml': OK [INFO] Sysprop apex.all.ready is missing, default to '' [INFO] Get modified time '     vendor.samsung.hardware.security.fkeymaster.ISehFkeymaster/default (@1)     vendor.samsung.hardware.security.vaultkeeper@2.0::ISehVaultKeeper/default     vendor.samsung.hardware.security.widevine.keyprov.ISehWidevineKeyProv/default (@1)     vendor.samsung.hardware.snap.ISehSnap/default (@1)     vendor.samsung.hardware.sysinput@1.3::ISehSysInputDev/default     vendor.samsung.hardware.thermal@1.0::ISehThermal/default     vendor.samsung.hardware.tlc.iccc@1.0::ISehIccc/default     vendor.samsung.hardware.tlc.kg@1.1::ISehKg/default     vendor.samsung.hardware.wifi.hostapd@4.0::ISehHostapd/default     vendor.samsung.hardware.wifi.supplicant@3.1::ISehSupplicant/default Suggested fix: 1. Update deprecated HALs to the latest version. 2. Check for any typos in device manifest or framework compatibility matrices with FCM version >= 5. 3. For new platform HALs, add them to any framework compatibility matrix with FCM version >= 5 where applicable. 4. For device-specific HALs, add to DEVICE_FRAMEWORK_COMPATIBILITY_MATRIX_FILE or DEVICE_PRODUCT_COMPATIBILITY_MATRIX_FILE. 5. For `android.*` HALs that are using unexpected instance names, the instance names need to be added to the AOSP framework compatibility matrices. A regex wildcard can be used if the instance names are proprietary. : Success INCOMPATIBLE 11:44:11 ninja failed with: exit status 1 #### failed to build some targets (01:53 (mm:ss)) ####
Building LineageOS for arm64 Devices
LineageOS Source Build File Removal
VINTF Metadata Build Error
TWRP Boardconfig Exclude Flags
Gemini Offers Assistance and Tools
Calculating One Billion to the Tenth
Gemini draw me a picture of a dog holding a flower
draw me a picture of a Down Syndrome dog
AI Thought Partner Introduction
Confirming Early Return Home
Free C Programming Learning Resources
Learning C For Free
Adding EGL to TWRP Build
Research Plan for EGL in TWRP
Build Error: SyntaxError in check_radio_versions.py
Android Build System Errors Analysis
Device and App Lag Troubleshooting
Android Build Unresolved Symbol Errors
Unresolved Library Dependencies in Android Build
`sprintf` Format String Error Explained
Add NFC to LineageOS Device Tree
AOSP Device.mk Configuration Guide
SystemLink EFS Mount Points Explained
VaultKeeper Permission Error During Initialization
Keymint TEE Error: Empty DAK
Dad: What's On Your Mind?
AOSP Security Patch Level Configuration
Arrival and Assistance Offer
Android Encryption Libraries and .so Files
AOSP KeyMint Service Libraries
How to make usb OTG work on TWRP with snapdragon, using kernel usb
AOSP BoardConfig Template Creation
QCOM's QTI: Qualcomm Technologies, Inc.
AOSP Keystore Service Integration Guide
Keystore2 Database Directory Error
Building an AOSP Tree
Android Keystore Service Lazy Start
Python Ninja Build Error: Unknown Option
AOSP Build Types: Eng, User, Userdebug
Android Recovery Image Build Failure
TWRP Build Fails: Missing Task Profiles
Best Places to Learn C
Current Time in Australia
How to fix usb OTG aosp
Android BoardConfig.mk Flags Explained
Lost Bag Report
Sentence Fragment Correction
Convert IMG to TAR Archive
Convert IMG to TAR Script
Creating Android Device Trees Explained
Incomplete Question, Clarification Needed
What is the weather in kettering tomorrow
Pocahontas Movie Identification
Ant Species and Their Diversity
"Moulton Spiders" Explained: Context Needed
AI Doesn't Have a Mother

Activity
Kettering, UK
Based on your places (home)
Update location
Conversation with Gemini
https://claude.ai/share/5b1be30c-272f-49a1-af6f-e0d4c9d319ec read this conversation with claude

I am unable to read the content of that specific Claude shared link. The page appears to be a dynamic application interface that does not render the conversation text in a way that my browsing tool can extract.

If you would like me to discuss or analyze the conversation, could you please copy and paste the text from that chat directly into this conversation? I would be happy to help once you provide it.

gsi_touch_fix_final (1).zip

zip

Study this zip file, it is a module that fixes the touchscreen on a gsi, how do I implement the touch fix in twrp-12.1





Ran 2 commands, viewed a file

Ran 2 commands, viewed a file



Got it. Here's a full breakdown of what this module does and how to port it to TWRP 12.1.

What the module does

The fix in service.sh:



sh

fix() { su -c 'echo check_connection > /sys/class/sec/tsp/cmd && cat /sys/class/sec/tsp/cmd_result'; }dir="/sys/class/backlight/panel/brightness"while true; do sleep .6 new=$(cat "$dir") [ "$old" != "$new" ] && [ "$new" != "0" ] && fix old=$newdone

It polls brightness every 600ms, and whenever it changes to a non-zero value (i.e. screen wakes up), it writes check_connection to the Samsung TSP (touchscreen processor) sysfs cmd node. This re-initialises the touch controller after screen-off.

This is the same sec/tsp sysfs interface you've already been working with on the A34x/A13VE.

Porting to TWRP 12.1

In recovery there's no Magisk, no service.sh lifecycle — you need to run this differently. Two approaches:

Option A: postrecovery.sh hook (cleanest)

TWRP 12.1 sources support a TW_POSTRECOVERY_SCRIPT or you can place a script in the ramdisk that gets exec'd after init. The most reliable hook in OrangeFox/TWRP is via the init .rc system.

Add a service in your device's recovery.rc or a separate touch_fix.rc:



rc

service tsp_touch_fix /sbin/tsp_touch_fix.sh class main user root group root seclabel u:r:recovery:s0 oneshot disabledon property:sys.boot_completed=1 start tsp_touch_fix

But since TWRP doesn't set sys.boot_completed, trigger on a recovery-specific property instead — or just use on init and let the loop run from startup:



rc

service tsp_touch_fix /sbin/tsp_touch_fix.sh class main user root group root seclabel u:r:recovery:s0

The script itself — /sbin/tsp_touch_fix.sh



sh

#!/sbin/shfix() { echo check_connection > /sys/class/sec/tsp/cmd cat /sys/class/sec/tsp/cmd_result}dir="/sys/class/backlight/panel/brightness"while true; do sleep 0.6 new=$(cat "$dir" 2>/dev/null) [ "$old" != "$new" ] && [ "$new" != "0" ] && fix old=$newdone

Drop this into your device tree and install to the ramdisk:

In Android.mk or BoardConfig.mk:



makefile

PRODUCT_COPY_FILES += $(LOCAL_PATH)/tsp_touch_fix.sh:recovery/root/sbin/tsp_touch_fix.sh

And the rc file:



makefile

PRODUCT_COPY_FILES += $(LOCAL_PATH)/touch_fix.rc:recovery/root/etc/init/touch_fix.rc

Make the script executable — add to your makefile or Android.mk:



makefile

$(shell chmod 755 $(LOCAL_PATH)/tsp_touch_fix.sh)

Option B: Simpler — call it from init.recovery.*.rc directly

If you already have a init.recovery.mt6877.rc or similar in your device tree, just append:



rc

service tsp_touch_fix /sbin/tsp_touch_fix.sh class main user root group root seclabel u:r:recovery:s0

No property trigger needed — it starts with the main class on recovery boot.

Key differences from the Magisk module

Magisk moduleTWRP portservice.sh run by Magisk daemon on Android bootScript run by recovery init via .rc serviceUses su -c wrapperAlready running as root in recovery — drop su -c/system/bin/sh shebangUse /sbin/shMagisk handles restart on crashAdd oneshot or let the while true loop handle it

The core sysfs command is identical — check_connection on sec/tsp works the same in recovery as in Android.

Quick sanity check before wiring it up

From a recovery adb shell, verify the node exists on your device:



sh

ls /sys/class/sec/tsp/cmdecho check_connection > /sys/class/sec/tsp/cmdcat /sys/class/sec/tsp/cmd_result

If cmd_result returns something like check_connection:OK or 1:OK, the TSP responds and this fix will work. If the node doesn't exist at all, the driver isn't exposing the Samsung TSP sysfs interface and a different approach (like a direct ioctl) would be needed.



This fix works for one screen lock, I can unlock it again after, but after it locks a second time the screen doesn't respond again



The loop logic has a bug for your use case. It only fires fix on a brightness change to non-zero, but it doesn't account for the TSP going dead again on the second screen-off → screen-on transition consistently.

The real issue: after the second lock, the brightness change still happens, but the TSP needs the check_connection poke to arrive slightly after the panel is fully on, not just when brightness starts changing. On the second cycle the timing is likely off, or the TSP is in a deeper sleep state that needs the command sent twice.

Fix 1: Send check_connection twice with a small delay



sh

#!/sbin/shfix() { echo check_connection > /sys/class/sec/tsp/cmd cat /sys/class/sec/tsp/cmd_result sleep 0.3 echo check_connection > /sys/class/sec/tsp/cmd cat /sys/class/sec/tsp/cmd_result}dir="/sys/class/backlight/panel/brightness"while true; do sleep 0.4 new=$(cat "$dir" 2>/dev/null) if [ "$old" != "$new" ] && [ "$new" != "0" ]; then sleep 0.2 fix fi old=$newdone

The sleep 0.2 before fix gives the panel time to fully initialise before poking the TSP.

Fix 2: Fire on every wake, track screen-off state explicitly

The original logic fires on any brightness change to non-zero, which can miss cases where brightness stays the same value across a lock/unlock cycle (e.g. always 255). Track screen state by watching for the zero→nonzero transition only:



sh

#!/sbin/shfix() { echo check_connection > /sys/class/sec/tsp/cmd cat /sys/class/sec/tsp/cmd_result}dir="/sys/class/backlight/panel/brightness"screen_was_off=0while true; do sleep 0.3 new=$(cat "$dir" 2>/dev/null) if [ "$new" = "0" ]; then screen_was_off=1 elif [ "$screen_was_off" = "1" ]; then # Screen just came back on from off state sleep 0.2 fix sleep 0.3 fix # Second poke for stubborn TSP screen_was_off=0 fidone

This is more reliable — it only fires after a confirmed off→on transition, not on any brightness delta. The double fix covers the TSP being in deep sleep after the second lock.

Fix 3: Also try input_booster or set_grip_data if check_connection alone isn't enough

From recovery adb shell, check what other commands the TSP exposes:



sh

cat /sys/class/sec/tsp/cmd_list

Some Samsung TSP drivers respond better to sw_reset followed by check_connection after the second sleep cycle:



sh

fix() { echo check_connection > /sys/class/sec/tsp/cmd cat /sys/class/sec/tsp/cmd_result}hard_fix() { echo sw_reset > /sys/class/sec/tsp/cmd cat /sys/class/sec/tsp/cmd_result sleep 0.5 echo check_connection > /sys/class/sec/tsp/cmd cat /sys/class/sec/tsp/cmd_result}

Then on the second+ wake use hard_fix instead of fix. You can track wake count with a counter variable.

I'd start with Fix 2 — the explicit screen_was_off state tracking is the most likely to be reliable across multiple lock/unlock cycles. Test with adb shell running the script manually first before baking it into the rc service.



Will fix 2 solve the problem if you wake up the screen and infinite amount of times



Yes — Fix 2 handles infinite cycles correctly. Walk through the logic:



sh

if [ "$new" = "0" ]; then screen_was_off=1 # set flag every time screen goes offelif [ "$screen_was_off" = "1" ]; then fix screen_was_off=0 # reset flag, ready for next cyclefi

Every single off→on transition goes through the same path regardless of how many times it's happened. screen_was_off gets set to 1 on every screen-off and reset to 0 after every fix. There's no counter that saturates, no state that accumulates — it's stateless across cycles.

The only failure mode would be if the loop somehow reads a non-zero brightness value while the screen is still off (e.g. a brief brightness flicker during AOSP animation), which would set screen_was_off=0 prematurely and miss the actual wake. If you see that happening, tighten the threshold:



sh

if [ "$new" = "0" ] || [ "$new" -lt "5" ] 2>/dev/null; then screen_was_off=1

But that's unlikely to be an issue in practice. The basic Fix 2 should hold up indefinitely.



I tested fix 2, screen is still not responding after the second wake up



The double fix call isn't the issue then — the TSP is actually not getting re-initialised properly. Let's diagnose what's actually happening.

First, run this manually in adb shell while reproducing the problem:



sh

# Watch cmd_result in real time as you lock/unlockwhile true; do echo check_connection > /sys/class/sec/tsp/cmd result=$(cat /sys/class/sec/tsp/cmd_result) bright=$(cat /sys/class/backlight/panel/brightness) echo "brightness=$bright result=$result" sleep 0.5done

Lock and unlock twice while this runs. What does cmd_result return on the second wake? Specifically:

Does it return OK but touch still doesn't work?

Does it return an error/fail?

Does the brightness node actually hit 0 on the second lock?

Also run:



sh

cat /sys/class/sec/tsp/cmd_list

And paste both outputs — the cmd_list and what you see from the loop across two lock/unlock cycles. That'll tell us whether check_connection is actually reaching the TSP or if we need a different command entirely.



#a13ve:/ # # Watch cmd_result in real time as you lock/unlock

hile ta13ve:/ # while true; do

> echo check_connection > /sys/class/sec/tsp/cmd

> result=$(cat /sys/class/sec/tsp/cmd_result)

> bright=$(cat /sys/class/backlight/panel/brightness)

> echo "brightness=$bright result=$result"

> sleep 0.5

> done

brightness=200 result=check_connection:NG

brightness=0 result=check_connection:NG

brightness=200 result=check_connection:NG

brightness=200 result=check_connection:NG

brightness=200 result=check_connection:NG

brightness=200 result=check_connection:NG

brightness=200 result=check_connection:NG

brightness=0 result=check_connection:NG

brightness=200 result=check_connection:NG

brightness=200 result=check_connection:NG

brightness=200 result=check_connection:NG

brightness=200 result=check_connection:NG

brightness=0 result=check_connection:NG

brightness=200 result=check_connection:NG

brightness=200 result=check_connection:NG

brightness=200 result=check_connection:NG

brightness=200 result=check_connection:NG

brightness=200 result=check_connection:NG

brightness=200 result=check_connection:NG

brightness=200 result=check_connection:NG

brightness=200 result=check_connection:NG

brightness=200 result=check_connection:NG

brightness=200 result=check_connection:NG

brightness=200 result=check_connection:NG

brightness=200 result=check_connection:NG

brightness=200 result=check_connection:NG

brightness=200 result=check_connection:NG

brightness=200 result=check_connection:NG

brightness=200 result=check_connection:NG

brightness=200 result=check_connection:NG

brightness=200 result=check_connection:NG

brightness=200 result=check_connection:NG

brightness=200 result=check_connection:NG

brightness=200 result=check_connection:NG

brightness=200 result=check_connection:NG

brightness=0 result=check_connection:NG

brightness=0 result=check_connection:NG

brightness=200 result=check_connection:NG

brightness=200 result=check_connection:NG

brightness=200 result=check_connection:NG

brightness=200 result=check_connection:NG

brightness=200 result=check_connection:NG

brightness=200 result=check_connection:NG

brightness=200 result=check_connection:NG

brightness=200 result=check_connection:NG

^C

130|a13ve:/ # cat /sys/class/sec/tsp/cmd_list

++factory command list++

fw_update

get_fw_ver_bin

get_fw_ver_ic

get_x_num

get_y_num

get_chip_vendor

get_chip_name

get_threshold

check_connection

glove_mode

aot_enable

set_sip_mode

set_game_mode

ear_detect_enable

prox_lp_scan_mode

dead_zone_enable

set_lpwg_dump

spay_enable

run_self_lpwg_rawdata_read

run_self_lpwg_rawdata_read_all_lcdoff

run_self_lpwg_noise_min_read

run_self_lpwg_noise_min_read_all_lcdoff

run_self_lpwg_noise_max_read

run_self_lpwg_noise_max_read_all_lcdoff

run_self_fdm_rawdata_read

run_self_fdm_rawdata_read_all_lcdoff

run_self_fdm_noise_min_read

run_self_fdm_noise_min_read_all_lcdoff

run_self_fdm_noise_max_read

run_self_fdm_noise_max_read_all_lcdoff

run_self_open_raw_read

run_self_open_raw_read_all

run_self_open_uni_read

run_self_open_uni_read_all

run_self_short_read

run_self_rawdata_read

run_self_rawdata_read_all

run_self_ccdata_read

run_self_ccdata_read_all

run_self_noise_min_read

run_self_noise_max_read

run_trx_short_test

run_sram_test

run_prox_intensity_read_all

factory_cmd_result_all

incell_power_control

factory_lcdoff_cmd_result_all

a13ve:/ #

Show more



The watchdog is still firing too early or check_connection is returning NG during normal TSP initialisation on boot, triggering the power cycle at the wrong time.

Check what check_connection returns on the A12s when touch is working normally:



sh

echo check_connection > /sys/class/sec/tsp/cmdcat /sys/class/sec/tsp/cmd_result

If it returns NG even when touch is working, then check_connection is not a reliable health indicator on this device and the watchdog will always falsely trigger the fix.

In that case we need a different health check. Try:



sh

# Check if input device exists as health indicatorls /dev/input/event3# Check input/enabled nodecat /sys/class/sec/tsp/enabled 2>/dev/null

If event3 disappearing is the only reliable indicator of touch failure on the A12s, we should change SamsungTSPTouchFix to check for the input device existence rather than check_connection. Paste the check_connection result when touch is working.



a12s:/ # echo check_connection > /sys/class/sec/tsp/cmd

ass/sec/tsp/cmd_resulta12s:/ # cat /sys/class/sec/tsp/cmd_result

check_connection:NG

a12s:/ # # Check if input device exists as health indicator

la12s:/ # ls /dev/input/event3

/dev/input/event3

a12s:/ #

a12s:/ # # Check input/enabled node

a12s:/ # cat /sys/class/sec/tsp/enabled 2>/dev/null



Confirmed — check_connection returns NG even when touch is working fine on the A12s. So the watchdog is blindly triggering the fix on every poll, including on boot, killing touch before you even use it.

The only reliable health indicator on the A12s is whether /dev/input/event3 exists. Change SamsungTSPTouchFix to use that instead:

OLD:



cpp

static void SamsungTSPTouchFix() { const char* tsp_cmd = "/sys/class/sec/tsp/cmd"; const char* tsp_result = "/sys/class/sec/tsp/cmd_result"; if (access(tsp_cmd, W_OK) != 0) return; // Check connection first - only intervene if TSP reports failure android::base::WriteStringToFile("check_connection", tsp_cmd); std::string result; android::base::ReadFileToString(tsp_result, &result); if (result.find("NG") != std::string::npos) { // TSP is dead - full power cycle sequence android::base::WriteStringToFile("incell_power_control,0", tsp_cmd); android::base::WriteStringToFile("incell_power_control,1", tsp_cmd); android::base::WriteStringToFile("fw_update", tsp_cmd); }}

NEW:



cpp

static void SamsungTSPTouchFix() { const char* tsp_cmd = "/sys/class/sec/tsp/cmd"; if (access(tsp_cmd, W_OK) != 0) return; // On some devices check_connection always returns NG even when touch is // working. Use input device existence as the health indicator instead. // If the touchscreen input device is present, touch is working fine. if (access("/dev/input/event3", F_OK) == 0) return; // Input device gone - full power cycle sequence to recover TSP android::base::WriteStringToFile("incell_power_control,0", tsp_cmd); android::base::WriteStringToFile("incell_power_control,1", tsp_cmd); android::base::WriteStringToFile("fw_update", tsp_cmd);}

However hardcoding event3 is fragile — it could be a different number on other devices. Check what the touchscreen event node is on the A13VE:



sh

getevent -l | grep -i "touch\|tsp"

If it's also event3 we can keep it. If it's different we need to make the event node configurable via another BoardConfig flag like TW_SAMSUNG_TSP_INPUT_DEV.

Based on the log from your conversation, we have identified exactly why the initial implementations failed: check_connection returns NG (No Good) even when your touchscreen is fully functional. This means relying on that node as a health check creates a false negative, causing any watchdog loop to constantly reset or misfire.

Since you are implementing this in TWRP 12.1, the most reliable, maintenance-free method is using a dynamic shell script service triggered by recovery's init.recovery.*.rc. This avoids hardcoding /dev/input/event3 in C++ source code (which can change across kernel compiles) and instead dynamically finds the touchscreen node.

Here is the complete implementation plan to fix the touch persistence issue indefinitely across infinite lock/unlock cycles.

1. The Optimized Fix Script (tsp_touch_fix.sh)
Instead of hardcoding an event number, this script looks at /sys/class/input/input*/name to find the correct touch device dynamically. It monitors the screen brightness state and runs the deep hardware reset sequence (incell_power_control) whenever the screen wakes up.

Create a file named tsp_touch_fix.sh in your device tree:

Bash
#!/sbin/sh

# 1. Dynamically detect the touch input device name
TOUCH_DEV=""
for name_file in /sys/class/input/input*/name; do
    if grep -qE "sec_ts|tsp|touchscreen|clearpad" "$name_file" 2>/dev/null; then
        # Extract the eventX number from the path
        INPUT_DIR=$(dirname "$name_file")
        TOUCH_DEV=$(ls "$INPUT_DIR" | grep -oE "event[0-9]+")
        break
    fi
done

# Fallback if detection fails
[ -z "$TOUCH_DEV" ] && TOUCH_DEV="event3"

TSP_CMD="/sys/class/sec/tsp/cmd"
BRIGHTNESS_DIR="/sys/class/backlight/panel/brightness"

# Full hardware initialization sequence
reset_tsp() {
    if [ -f "$TSP_CMD" ]; then
        # Pull power, restore power, force firmware update handshake
        echo "incell_power_control,0" > "$TSP_CMD"
        sleep 0.1
        echo "incell_power_control,1" > "$TSP_CMD"
        sleep 0.2
        echo "fw_update" > "$TSP_CMD"
    fi
}

screen_was_off=0

# Loop indefinitely to handle infinite sleep/wake cycles
while true; do
    sleep 0.4
    
    # Check if brightness node exists
    if [ -f "$BRIGHTNESS_DIR" ]; then
        current_brightness=$(cat "$BRIGHTNESS_DIR" 2>/dev/null)
    else
        current_brightness=200 # Fallback if node differs
    fi

    if [ "$current_brightness" = "0" ]; then
        screen_was_off=1
    elif [ "$screen_was_off" = "1" ] || [ ! -e "/dev/input/$TOUCH_DEV" ]; then
        # Trigger if screen woke up OR if the input event node dropped offline
        sleep 0.2
        reset_tsp
        screen_was_off=0
    fi
done
2. Integrating into TWRP 12.1 Build Tree
Step A: Update your init.recovery.hardware.rc
Open your init.recovery.mt6877.rc (or whichever matching platform/device rc file your tree uses) and append the service definition at the bottom:

Code snippet
service tsp_touch_fix /sbin/tsp_touch_fix.sh
    class main
    user root
    group root
    seclabel u:r:recovery:s0
    disabled

on property:sys.listeners.registered=true
    start tsp_touch_fix

on post-fs-data
    start tsp_touch_fix
Step B: Copy the Files via device.mk / BoardConfig.mk
Ensure the script is copied to the recovery ramdisk root and assigned the correct execution permissions during the compilation phase. Add this to AndroidProducts.mk or device_[codename].mk:

Makefile
# Copy the touchscreen watchdog fix script
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/tsp_touch_fix.sh:recovery/root/sbin/tsp_touch_fix.sh

# Ensure execution permissions are set on build time
$(shell chmod 755 $(LOCAL_PATH)/tsp_touch_fix.sh)
Why this solves the infinite wake issue:
Bypasses check_connection Entirely: It no longer asks the driver if it's healthy (which lied with NG). It assumes that if the screen goes to 0 brightness, the TSP needs an explicit power cycle toggle on recovery wake.

State Resetting: The screen_was_off flag resets to 0 cleanly after every single execution, making it fully stateless across hundreds of sleep cycles.

Double Redundancy: Even if the screen state fails to register a sleep cycle due to an animation glitch, the [ ! -e "/dev/input/$TOUCH_DEV" ] condition ensures that if the kernel drops the touch input node entirely, the script immediately revives it.

based on claudes answeres i need to figure out which event it uses

To find out exactly which eventX node your touchscreen is using on the A13VE, you need to check it via an ADB shell while the device is booted into a state where touch is working (either normal Android OS or during that first initial recovery boot before it locks).

Run the following commands in your terminal:

Method 1: The Quick Filter (Recommended)
This command queries the Android input subsystem layer and isolates lines containing "touch" or "tsp" (Touchscreen Processor):

Bash
adb shell "getevent -p | grep -E -B 1 -A 4 'touch|tsp|sec_ts'"
What to look for:
Look for a line that starts with /dev/input/event.... For example, if it outputs something like this:

add device 3: /dev/input/event3

  name:     "sec_ts"

Then your device uses event3.

Method 2: Dump the System Input Registry
If the filter above is too strict, you can dump the full input device breakdown directly from the kernel filesystem directory:

Bash
adb shell "cat /proc/bus/input/devices"
How to parse the output:
Scroll through the text blocks until you find the section detailing your touchscreen. You are looking for the Name and Handlers lines:

Plaintext
I: Bus=0018 Vendor=0000 Product=0000 Version=0000
N: Name="sec_ts"                                 <-- Touchscreen Name
P: Phys=
S: Sysfs=/devices/virtual/input/input3
U: Uniq=
H: Handlers=mdm event3 cpufreq                   <-- Look for eventX here!
B: PROP=2
B: EV=b
In this example block, the Handlers= line explicitly states event3.

Method 3: Real-time Monitor (If touch is currently active)
If you want to be 100% physically certain before changing code, run this command:

Bash
adb shell getevent -q
Now, tap or swipe your finger across the screen. You will see columns of hex numbers streaming down the terminal interface. Look at the very first column of the stream:

Plaintext
/dev/input/event3: 0003 0039 00000021
/dev/input/event3: 0003 0035 000002b5
The prefix telling you where those inputs are coming from (/dev/input/event3) is your definitive touchscreen node index. Consolidate that target eventX back into your logic check configuration.

127|a12s:/ # getevent -p | grep -E -B 1 -A 4 'touch|tsp|sec_ts'                                                                                                

add device 5: /dev/input/event4

  name:     "sec_touchproximity"

  events:

    ABS (0003): 003e  : value 0, min 0, max -1, fuzz 0, flat 0, resolution 0

  input props:

    INPUT_PROP_DIRECT

--

add device 8: /dev/input/event3

  name:     "sec_touchscreen"

  events:

    KEY (0001): 008f  0145  014a  02be 

    ABS (0003): 0000  : value 0, min 0, max 719, fuzz 0, flat 0, resolution 0

                0001  : value 0, min 0, max 1599, fuzz 0, flat 0, resolution 0

a12s:/ # root@j-Aspire-A315-35:/home/j# adb shell

adb: no devices/emulators found

root@j-Aspire-A315-35:/home/j# adb shell

a13ve:/ # adb shell "getevent -p | grep -E -B 1 -A 4 'touch|tsp|sec_ts' 

> ^C

130|a13ve:/ # getevent -p | grep -E -B 1 -A 4 'touch|tsp|sec_ts'            

add device 5: /dev/input/event4

  name:     "sec_touchproximity"

  events:

    ABS (0003): 003e  : value 0, min 0, max -1, fuzz 0, flat 0, resolution 0

  input props:

    INPUT_PROP_DIRECT

--

add device 7: /dev/input/event3

  name:     "sec_touchscreen"

  events:

    KEY (0001): 008f  014a  01c7  02be 

    ABS (0003): 002f  : value 0, min 0, max 9, fuzz 0, flat 0, resolution 0

                0030  : value 0, min 0, max 255, fuzz 0, flat 0, resolution 0

a13ve:/ # root@j-Aspire-A315-35:/home/j# 





so how to i edit the commits i made on twrp to fix the scren

The logs confirm it: both your a12s and a13ve devices map the actual touchscreen (sec_touchscreen) to event3 (while event4 is just the proximity/ear-detect sub-sensor).

Since you need to edit the commits you already made in your custom TWRP source code tree to change the health check logic from check_connection to a direct event3 check, here is exactly how to clean up your git history and update the source code files.

Step 1: Update the C++ Source Code File
Go to the file where you wrote the SamsungTSPTouchFix() function inside your TWRP source tree and update its logic. It needs to drop the check_connection string check and use the access() health check for /dev/input/event3 instead.

Modify the function to match this configuration exactly:

C++
static void SamsungTSPTouchFix() {
    const char* tsp_cmd = "/sys/class/sec/tsp/cmd";
    
    // Safety check: if the sysfs command node doesn't exist, exit immediately
    if (access(tsp_cmd, W_OK) != 0) return;

    // Health check: Check if the actual touchscreen device node is alive.
    // If event3 is missing or dropped offline, the condition is true and we kick the TSP.
    if (access("/dev/input/event3", F_OK) != 0) {
        // Force the full incell power cycle sequence to recover the frozen panel hardware
        android::base::WriteStringToFile("incell_power_control,0", tsp_cmd);
        android::base::WriteStringToFile("incell_power_control,1", tsp_cmd);
        android::base::WriteStringToFile("fw_update", tsp_cmd);
    }
}
Step 2: Edit Your Git Commits
To clean this up like a pro rather than stacking a dozen "fix typo" commits on top of each other, you have two options depending on where your current git tree stands.

Scenario A: It was your very last commit (Easiest)
If the touch fix was the literal last commit you made on your branch and you haven't committed anything since:

Bash
# 1. Stage the updated source code file changes
git add path/to/your/modified_file.cpp

# 2. Overwrite your last commit directly without changing the commit message
git commit --amend --no-edit

# 3. Force push the updated commit back to your GitLab/GitHub working tree
git push origin HEAD --force
Scenario B: The touch fix is 2 or 3 commits back
If you made other structural changes (like adding the .rc files or makefiles) after you wrote the faulty C++ implementation, use an interactive rebase to alter the specific broken commit:

Bash
# 1. Open the interactive rebase tool for your last few commits (e.g., last 5)
git rebase -i HEAD~5
A text editor will open listing your recent commits. It will look similar to this:

Plaintext
pick a1b2c3d Added touch fix .rc files
pick e5f6g7h Implemented SamsungTSPTouchFix function
pick j9k0l1m Updated BoardConfig definitions
Find the commit that contains your C++ code modification (e5f6g7h).

Change the word pick at the start of that line to edit (or e).

Save and close the editor text file. Git will pause your repository state exactly at that commit.

Open your file, paste the updated SamsungTSPTouchFix() code blocks, and save.

Run the following commands to seal the rebase sequence:

Bash
git add path/to/your/modified_file.cpp
git rebase --continue
Git will automatically replay your other subsequent changes on top of the fixed code. Once the rebase completes cleanly, force-push it up to update your workspace repository:

Bash
git push origin HEAD --force
Now you are ready to fire off a clean recovery image compile via your cloud builder setup.

the screen still does not work when twrp first boots

If touch isn't working on the very first boot of TWRP (even before you lock/unlock the screen), it means the kernel/touchscreen driver didn't fully initialize the panel when recovery started up. Because your health check looks for /dev/input/event3, and that node hasn't been created yet, the service needs to trigger immediately on early boot to wake it up.

Here is why it's failing on first boot and how to fix it by updating your initialization scripts.

The Problem: Recovery Lifecycle Race Condition
When TWRP boots, your tsp_touch_fix service might be waiting for properties like sys.listeners.registered=true. However, recovery initialization moves incredibly fast, and the driver often skips loading properly without a hard kick at the very start of the boot cycle (on init or on early-init).

Additionally, your script might be exiting too early if it doesn't loop correctly or if it hits a permission wall when trying to write to /sys/ during early boot.

Step 1: Fix your .rc File (Force Early Initialization)
You need to force recovery to start the touch fix service immediately as the kernel initializes the file systems, and ensure it restarts if the driver drops it.

Update your init.recovery.mt6877.rc (or matching platform rc):

Code snippet
# Trigger the fix at the absolute earliest point when file systems are ready
on post-fs
    start tsp_touch_fix

# Also trigger it during standard init to ensure it catches it if post-fs was too early
on init
    start tsp_touch_fix

service tsp_touch_fix /sbin/tsp_touch_fix.sh
    class main
    user root
    group root
    seclabel u:r:recovery:s0
    oneshot
(Note: Changing it to oneshot without standard looping means we want it to run, kick the panel if needed, and let the while loop inside the shell script handle the rest).

Step 2: Update Your Script (tsp_touch_fix.sh)
If /dev/input/event3 doesn't exist on first boot, the script needs to run a hard reset immediately instead of waiting for a brightness change.

Update your tsp_touch_fix.sh to include a first-boot validation check before it enters the background monitoring loop:

Bash
#!/sbin/sh

TSP_CMD="/sys/class/sec/tsp/cmd"
BRIGHTNESS_DIR="/sys/class/backlight/panel/brightness"
TOUCH_DEV="/dev/input/event3"

reset_tsp() {
    if [ -f "$TSP_CMD" ]; then
        echo "incell_power_control,0" > "$TSP_CMD"
        sleep 0.1
        echo "incell_power_control,1" > "$TSP_CMD"
        sleep 0.2
        echo "fw_update" > "$TSP_CMD"
    fi
}

# --- FIRST BOOT CHECK ---
# If TWRP just booted and event3 doesn't exist, force an immediate kick
if [ ! -e "$TOUCH_DEV" ]; then
    reset_tsp
    sleep 0.5
fi

# --- CONTINUOUS MONITORING LOOP ---
screen_was_off=0

while true; do
    sleep 0.4
    
    if [ -f "$BRIGHTNESS_DIR" ]; then
        current_brightness=$(cat "$BRIGHTNESS_DIR" 2>/dev/null)
    else
        current_brightness=200
    fi

    if [ "$current_brightness" = "0" ]; then
        screen_was_off=1
    elif [ "$screen_was_off" = "1" ] || [ ! -e "$TOUCH_DEV" ]; then
        # If screen woke up OR event3 vanished entirely, kick the driver
        sleep 0.2
        reset_tsp
        screen_was_off=0
    fi
done
Step 3: Amend Your Commit and Build
Since you want to update your progress cleanly:

Update the files (init.recovery.*.rc and tsp_touch_fix.sh) in your device tree workspace.

Stage and blend the changes using git:

Bash
git add .
git commit --amend --no-edit
git push origin HEAD --force
Quick Debugging Tip
If you build this and touch still doesn't work on first boot but works if you run adb shell tsp_touch_fix.sh manually, it is 100% an SELinux permission denial. TWRP's init system won't let scripts write to /sys/class/sec/tsp/cmd unless allowed. If that happens, let me know and we will append a rule to your recovery.te SELinux policy file.

i want to fix it in the twrp sourcecode

To patch this directly in the native C++ TWRP source code (bootable/recovery), you need to loop the health check inside a dedicated background thread right when recovery boots.

If you only execute SamsungTSPTouchFix() once during initial startup, it will run before the kernel input devices are fully initialized by the kernel, find that event3 isn't there, trigger the fix once (perhaps too early), and then exit forever.

Here is how to implement a continuous, non-blocking background thread directly in the C++ TWRP source tree (recovery.cpp or recovery_ui.cpp) so that it fixes touch on first boot and across infinite sleep/wake cycles.

1. The Native C++ Implementation
Replace your current SamsungTSPTouchFix() implementation with this multi-threaded loop. This spawns an independent thread on startup that checks the panel's health every 500ms indefinitely.

C++
#include <thread>
#include <chrono>
#include <unistd.h>
#include <android-base/file.h>
#include <android-base/stringprintf.h>

static void SamsungTSPTouchFixWorker() {
    const char* tsp_cmd = "/sys/class/sec/tsp/cmd";
    const char* touch_node = "/dev/input/event3";
    const char* brightness_node = "/sys/class/backlight/panel/brightness";
    
    // Wait up to 5 seconds for sysfs infrastructure to appear after kernel boot
    int retry = 10;
    while (access(tsp_cmd, W_OK) != 0 && retry-- > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    if (access(tsp_cmd, W_OK) != 0) return; // Exit if node never appears

    bool screen_was_off = false;

    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        // 1. Check current brightness state
        std::string brightness_str;
        int current_brightness = 200; // Default assuming ON if reading fails
        if (android::base::ReadFileToString(brightness_node, &brightness_str)) {
            try {
                current_brightness = std::stoi(android::base::Trim(brightness_str));
            } catch (...) {}
        }

        if (current_brightness == 0) {
            screen_was_off = true;
            continue; // Screen is off; skip checks until it wakes
        }

        // 2. Trigger Conditions: Screen just woke up OR event3 vanished completely
        if (screen_was_off || access(touch_node, F_OK) != 0) {
            // Give the panel a split second to receive power before poking it
            std::this_thread::sleep_for(std::chrono::milliseconds(200));

            // Full power-cycle handshake sequence
            android::base::WriteStringToFile("incell_power_control,0", tsp_cmd);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            android::base::WriteStringToFile("incell_power_control,1", tsp_cmd);
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            android::base::WriteStringToFile("fw_update", tsp_cmd);

            screen_was_off = false; // Reset state tracking
        }
    }
}

// Call this function once from main() or recovery UI initialization
void StartSamsungTSPTouchFix() {
    std::thread fix_thread(SamsungTSPTouchFixWorker);
    fix_thread.detach(); // Let it run indefinitely in the background
}
2. Where to Hook It in recovery.cpp
To make sure it fires on the absolute initial startup sequence, call StartSamsungTSPTouchFix() near the beginning of main() inside bootable/recovery/recovery.cpp.

Look for the main(int argc, char argv) function, and place the initialization hook right after the logging and base directories are configured:

C++
int main(int argc, char** argv) {
    // ... Existing TWRP logging/volume setup code ...

    #if defined(TW_SAMSUNG_TSP_FIX) // Optional: wrap in a BoardConfig guard if desired
    StartSamsungTSPTouchFix();
    #endif

    // ... Rest of TWRP main UI launch logic ...
}
3. Dealing with SELinux (Why first boot might still block)
Even when written natively in C++, if TWRP is running in Enforcing mode, the recovery binary context (u:r:recovery:s0) is strictly forbidden by the kernel from writing to random sysfs paths like /sys/class/sec/tsp/cmd unless explicitly permitted.

If you compile this directly into the source and the screen still fails on first boot, add the following line to your device tree's recovery SELinux policy file (usually recovery.te in your sepolicy directory):

Plaintext
allow recovery sysfs_sec_tsp:file w_file_perms;
(Replace sysfs_sec_tsp with whatever your specific kernel/device tree maps the /sys/class/sec/ nodes to if you see an audit denial in adb logcat).

show me the original code compared to the new

To see exactly what changed, here is a side-by-side comparison of the original logic discussed in the Claude log versus the new background-threaded implementation.

The core flaw in the original code was that it executed exactly once during the initialization function, checked a status node (check_connection) that doesn't work correctly on your hardware, and then exited forever. The new version runs continuously in its own isolated memory thread (std::thread), uses a verified health node (/dev/input/event3), and loops indefinitely.

The C++ Code Comparison
C++
// ==========================================
// ORIGINAL CODE (From Claude Chat Log)
// ==========================================
static void SamsungTSPTouchFix() {
  const char* tsp_cmd = "/sys/class/sec/tsp/cmd";
  const char* tsp_result = "/sys/class/sec/tsp/cmd_result";
  
  if (access(tsp_cmd, W_OK) != 0) return;
  
  // ❌ FLAWED HEALTH CHECK:
  // check_connection returns "NG" even when touch works on your hardware.
  // This causes the watchdog to trigger blindly or fail entirely.
  android::base::WriteStringToFile("check_connection", tsp_cmd);
  std::string result;
  android::base::ReadFileToString(tsp_result, &result);
  
  if (result.find("NG") != std::string::npos) {
    // ❌ RUNS ONCE ONLY:
    // If this fires on early boot before the driver finishes loading,
    // it will try to reset the panel, fail, and never check again.
    android::base::WriteStringToFile("incell_power_control,0", tsp_cmd);
    android::base::WriteStringToFile("incell_power_control,1", tsp_cmd);
    android::base::WriteStringToFile("fw_update", tsp_cmd);
  }
}
⇓
C++
// ==========================================
// NEW NATIVE SOURCE CODE (Threaded Loop Fix)
// ==========================================
#include <thread>
#include <chrono>
#include <unistd.h>
#include <android-base/file.h>
#include <android-base/stringprintf.h>

static void SamsungTSPTouchFixWorker() {
    const char* tsp_cmd = "/sys/class/sec/tsp/cmd";
    const char* touch_node = "/dev/input/event3"; //  VERIFIED touch node
    const char* brightness_node = "/sys/class/backlight/panel/brightness";
    
    // 1. Wait up to 5 seconds on first boot for sysfs nodes to register
    int retry = 10;
    while (access(tsp_cmd, W_OK) != 0 && retry-- > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    if (access(tsp_cmd, W_OK) != 0) return; 

    bool screen_was_off = false;

    // 2. Continuous Monitoring Loop (Fixes infinite wake cycles)
    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        // Read current brightness safely
        std::string brightness_str;
        int current_brightness = 200; 
        if (android::base::ReadFileToString(brightness_node, &brightness_str)) {
            try {
                current_brightness = std::stoi(android::base::Trim(brightness_str));
            } catch (...) {}
        }

        if (current_brightness == 0) {
            screen_was_off = true;
            continue; // Screen is asleep; pause execution
        }

        // 3.  RELIABLE HEALTH CHECK CONDITIONS:
        // Trigger reset if: Screen just woke up OR event3 vanished entirely
        if (screen_was_off || access(touch_node, F_OK) != 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));

            // Hard pulse the hardware
            android::base::WriteStringToFile("incell_power_control,0", tsp_cmd);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            android::base::WriteStringToFile("incell_power_control,1", tsp_cmd);
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            android::base::WriteStringToFile("fw_update", tsp_cmd);

            screen_was_off = false; 
        }
    }
}

// 4.  BACKGROUND ENTRY HOOK
// Call this function once from main() inside bootable/recovery/recovery.cpp
void StartSamsungTSPTouchFix() {
    std::thread fix_thread(SamsungTSPTouchFixWorker);
    fix_thread.detach(); // Detaches the thread to live parallel to recovery UI
}

#endif

void RecoveryUI::SetScreensaverState(ScreensaverState state) {
  switch (state) {
    case ScreensaverState::NORMAL:
      if (android::base::WriteStringToFile(std::to_string(brightness_normal_value_),
                                           brightness_file_)) {
        screensaver_state_ = ScreensaverState::NORMAL;
        LOG(INFO) << "Brightness: " << brightness_normal_value_ << " (" << brightness_normal_
                  << "%)";
#ifdef TW_SAMSUNG_TSP_TOUCH_FIX
        SamsungTSPTouchFix();
#endif
        screensaver_state_ = ScreensaverState::DIMMED;
      } else {
        LOG(WARNING) << "Unable to set brightness to dim";
      }
      break;
    case ScreensaverState::OFF:
      if (android::base::WriteStringToFile("0", brightness_file_)) {
        LOG(INFO) << "Brightness: 0 (off)";
        screensaver_state_ = ScreensaverState::OFF;
      } else {
        LOG(WARNING) << "Unable to set brightness to off";
      }
      break;
    default:
      LOG(ERROR) << "Invalid screensaver state";
  }
}

int RecoveryUI::WaitKey() {
  std::unique_lock<std::mutex> lk(key_queue_mutex);

  // Check for a saved key queue interruption.
  if (key_interrupted_) {
    SetScreensaverState(ScreensaverState::NORMAL);
    return static_cast<int>(KeyError::INTERRUPTED);
  }

  // Time out after UI_WAIT_KEY_TIMEOUT_SEC, unless a USB cable is plugged in.
  do {
    bool rc = key_queue_cond.wait_for(lk, std::chrono::seconds(UI_WAIT_KEY_TIMEOUT_SEC), [this] {
      return this->key_queue_len != 0 || key_interrupted_;
    });
    if (key_interrupted_) {
      SetScreensaverState(ScreensaverState::NORMAL);
      return static_cast<int>(KeyError::INTERRUPTED);
    }
    if (screensaver_state_ != ScreensaverState::DISABLED) {
      if (!rc) {
        // Must be after a timeout. Lower the brightness level: NORMAL -> DIMMED; DIMMED -> OFF.
        if (screensaver_state_ == ScreensaverState::NORMAL) {
          SetScreensaverState(ScreensaverState::DIMMED);
        } else if (screensaver_state_ == ScreensaverState::DIMMED) {
          SetScreensaverState(ScreensaverState::OFF);
        }
      } else if (screensaver_state_ != ScreensaverState::NORMAL) {
        // Drop the first key if it's changing from OFF to NORMAL.
        if (screensaver_state_ == ScreensaverState::OFF) {
          if (key_queue_len > 0) {
            memcpy(&key_queue[0], &key_queue[1], sizeof(int) * --key_queue_len);
          }
        }

        // Reset the brightness to normal.
        SetScreensaverState(ScreensaverState::NORMAL);
      }
    }
  } while (IsUsbConnected() && key_queue_len == 0);

  int key = static_cast<int>(KeyError::TIMED_OUT);
  if (key_queue_len > 0) {
    key = key_queue[0];
    memcpy(&key_queue[0], &key_queue[1], sizeof(int) * --key_queue_len);
  }
  return key;
}

void RecoveryUI::InterruptKey() {
  {
    std::lock_guard<std::mutex> lg(key_queue_mutex);
    key_interrupted_ = true;
  }
  key_queue_cond.notify_one();
}

bool RecoveryUI::IsUsbConnected() {
  int fd = open("/sys/class/android_usb/android0/state", O_RDONLY);
  if (fd < 0) {
    printf("failed to open /sys/class/android_usb/android0/state: %s\n", strerror(errno));
    return 0;
  }

  char buf;
  // USB is connected if android_usb state is CONNECTED or CONFIGURED.
  int connected = (TEMP_FAILURE_RETRY(read(fd, &buf, 1)) == 1) && (buf == 'C');
  if (close(fd) < 0) {
    printf("failed to close /sys/class/android_usb/android0/state: %s\n", strerror(errno));
  }
  return connected;
}

bool RecoveryUI::IsKeyPressed(int key) {
  std::lock_guard<std::mutex> lg(key_press_mutex);
  int pressed = key_pressed[key];
  return pressed;
}

bool RecoveryUI::IsLongPress() {
  std::lock_guard<std::mutex> lg(key_press_mutex);
  bool result = key_long_press;
  return result;
}

bool RecoveryUI::HasThreeButtons() const {
  return has_power_key && has_up_key && has_down_key;
}

bool RecoveryUI::HasPowerKey() const {
  return has_power_key;
}

bool RecoveryUI::HasTouchScreen() const {
  return has_touch_screen;
}

void RecoveryUI::FlushKeys() {
  std::lock_guard<std::mutex> lg(key_queue_mutex);
  key_queue_len = 0;
}

RecoveryUI::KeyAction RecoveryUI::CheckKey(int key, bool is_long_press) {
  {
    std::lock_guard<std::mutex> lg(key_press_mutex);
    key_long_press = false;
  }

  // If we have power and volume up keys, that chord is the signal to toggle the text display.
  if (HasThreeButtons() || (HasPowerKey() && HasTouchScreen() && touch_screen_allowed_)) {
    if ((key == KEY_VOLUMEUP || key == KEY_UP) && IsKeyPressed(KEY_POWER)) {
      return TOGGLE;
    }
  } else {
    // Otherwise long press of any button toggles to the text display,
    // and there's no way to toggle back (but that's pretty useless anyway).
    if (is_long_press && !IsTextVisible()) {
      return TOGGLE;
    }

    // Also, for button-limited devices, a long press is translated to KEY_ENTER.
    if (is_long_press && IsTextVisible()) {
      EnqueueKey(KEY_ENTER);
      return IGNORE;
    }
  }

  // Press power seven times in a row to reboot.
  if (key == KEY_POWER) {
    bool reboot_enabled = enable_reboot;

    if (reboot_enabled) {
      ++consecutive_power_keys;
      if (consecutive_power_keys >= 7) {
        return REBOOT;
      }
    }
  } else {
    consecutive_power_keys = 0;
  }

  return (IsTextVisible() || screensaver_state_ == ScreensaverState::OFF) ? ENQUEUE : IGNORE;
}

void RecoveryUI::KeyLongPress(int) {}

void RecoveryUI::SetEnableReboot(bool enabled) {
  std::lock_guard<std::mutex> lg(key_press_mutex);
  enable_reboot = enabled;
}
