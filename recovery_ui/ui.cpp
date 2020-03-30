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
#include <volume_manager/VolumeManager.h>

#include "minui/minui.h"
#include "otautil/sysutil.h"

using namespace std::chrono_literals;

constexpr int UI_WAIT_KEY_TIMEOUT_SEC = 120;
constexpr const char* BRIGHTNESS_FILE = "/sys/class/leds/lcd-backlight/brightness";
constexpr const char* MAX_BRIGHTNESS_FILE = "/sys/class/leds/lcd-backlight/max_brightness";
constexpr const char* BRIGHTNESS_FILE_SDM = "/sys/class/backlight/panel0-backlight/brightness";
constexpr const char* MAX_BRIGHTNESS_FILE_SDM =
    "/sys/class/backlight/panel0-backlight/max_brightness";

constexpr int kDefaultTouchLowThreshold = 50;
constexpr int kDefaultTouchHighThreshold = 90;

RecoveryUI::RecoveryUI()
    : brightness_normal_(50),
      brightness_dimmed_(25),
      brightness_file_(BRIGHTNESS_FILE),
      max_brightness_file_(MAX_BRIGHTNESS_FILE),
      touch_screen_allowed_(true),
      fastbootd_logo_enabled_(false),
      touch_low_threshold_(android::base::GetIntProperty("ro.recovery.ui.touch_low_threshold",
                                                         kDefaultTouchLowThreshold)),
      touch_high_threshold_(android::base::GetIntProperty("ro.recovery.ui.touch_high_threshold",
                                                          kDefaultTouchHighThreshold)),
      key_interrupted_(false),
      event_queue_len(0),
      key_last_down(-1),
      key_long_press(false),
      key_down_count(0),
      enable_reboot(true),
      consecutive_power_keys(0),
      last_key(-1),
      has_power_key(false),
      has_up_key(false),
      has_down_key(false),
      has_touch_screen(false),
      touch_slot_(0),
      touch_finger_down_(false),
      touch_saw_x_(false),
      touch_saw_y_(false),
      touch_reported_(false),
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
}

void RecoveryUI::OnTouchDeviceDetected(int fd) {
  char name[256];
  char path[PATH_MAX];
  char buf[4096];

  memset(name, 0, sizeof(name));
  if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) < 0) {
    return;
  }
  sprintf(path, "/sys/board_properties/virtualkeys.%s", name);
  int vkfd = open(path, O_RDONLY);
  if (vkfd < 0) {
    LOG(INFO) << "vkeys: could not open " << path;
    return;
  }
  ssize_t len = read(vkfd, buf, sizeof(buf));
  close(vkfd);
  if (len <= 0) {
    LOG(ERROR) << "vkeys: could not read " << path;
    return;
  }
  buf[len] = '\0';

  char* p = buf;
  char* endp;
  for (size_t n = 0; p < buf + len && *p == '0'; ++n) {
    int val[6];
    int f;
    for (f = 0; *p && f < 6; ++f) {
      val[f] = strtol(p, &endp, 0);
      if (p == endp) break;
      p = endp + 1;
    }
    if (f != 6 || val[0] != 0x01) break;
    vkey_t vk;
    vk.keycode = val[1];
    vk.min_ = Point(val[2] - val[4] / 2, val[3] - val[5] / 2);
    vk.max_ = Point(val[2] + val[4] / 2, val[3] + val[5] / 2);
    virtual_keys_.push_back(vk);
  }
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
    brightness_file_ = BRIGHTNESS_FILE_SDM;
  }
  if (access(max_brightness_file_.c_str(), R_OK)) {
    max_brightness_file_ = MAX_BRIGHTNESS_FILE_SDM;
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
    ev_iterate_touch_inputs(
        std::bind(&RecoveryUI::OnTouchDeviceDetected, this, std::placeholders::_1),
        std::bind(&RecoveryUI::OnKeyDetected, this, std::placeholders::_1));

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

  return true;
}

void RecoveryUI::CalibrateTouch(int fd) {
  struct input_absinfo info;
  static bool calibrated = false;

  if (calibrated) return;

  memset(&info, 0, sizeof(info));
  if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &info) == 0) {
    touch_min_.x(info.minimum);
    touch_max_.x(info.maximum);
  }
  memset(&info, 0, sizeof(info));
  if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &info) == 0) {
    touch_min_.y(info.minimum);
    touch_max_.y(info.maximum);
  }

  calibrated = true;
}

void RecoveryUI::OnTouchPress() {
  touch_start_ = touch_track_ = touch_pos_;
}

void RecoveryUI::OnTouchTrack() {
  if (touch_pos_.y() <= gr_fb_height()) {
    while (abs(touch_pos_.y() - touch_track_.y()) >= MenuItemHeight()) {
      int dy = touch_pos_.y() - touch_track_.y();
      int key = (dy < 0) ? KEY_SCROLLDOWN : KEY_SCROLLUP;
      ProcessKey(key, 1);  // press key
      ProcessKey(key, 0);  // and release it
      int sgn = (dy > 0) - (dy < 0);
      touch_track_.y(touch_track_.y() + sgn * MenuItemHeight());
    }
  }
}

void RecoveryUI::OnTouchRelease() {
  // Allow turning on text mode with any swipe, if bootloader has set a bootreason of recovery_ui.
  if (is_bootreason_recovery_ui_ && !IsTextVisible()) {
    ShowText(true);
    return;
  }

  // Check vkeys.  Only report if touch both starts and ends in the vkey.
  if (touch_start_.y() > gr_fb_height() && touch_pos_.y() > gr_fb_height()) {
    for (const auto& vk : virtual_keys_) {
      if (vk.inside(touch_start_) && vk.inside(touch_pos_)) {
        ProcessKey(vk.keycode, 1);  // press key
        ProcessKey(vk.keycode, 0);  // and release it
      }
    }
    return;
  }

  // If we tracked a vertical swipe, ignore the release
  if (touch_track_ != touch_start_) {
    return;
  }

  // Check for horizontal swipe
  Point delta = touch_pos_ - touch_start_;
  if (abs(delta.y()) < touch_low_threshold_ && abs(delta.x()) > touch_high_threshold_) {
    int key = (delta.x() < 0) ? KEY_BACK : KEY_POWER;
    ProcessKey(key, 1);  // press key
    ProcessKey(key, 0);  // and release it
    return;
  }

  // Simple touch
  EnqueueTouch(touch_pos_);
}

int RecoveryUI::OnInputEvent(int fd, uint32_t epevents) {
  struct input_event ev;
  if (ev_get_input(fd, epevents, &ev) == -1) {
    return -1;
  }

  // Touch inputs handling.
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
      // There might be multiple SYN_REPORT events. Only report press/release once.
      if (!touch_reported_ && touch_finger_down_) {
        if (touch_saw_x_ && touch_saw_y_) {
          OnTouchPress();
          touch_reported_ = true;
          touch_saw_x_ = touch_saw_y_ = false;
        }
      } else if (touch_reported_ && !touch_finger_down_) {
        OnTouchRelease();
        touch_reported_ = false;
        touch_saw_x_ = touch_saw_y_ = false;
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
    CalibrateTouch(fd);
    if (ev.code == ABS_MT_SLOT) {
      touch_slot_ = ev.value;
    }
    // Ignore other fingers.
    if (touch_slot_ > 0) return 0;

    switch (ev.code) {
      case ABS_MT_POSITION_X:
        touch_finger_down_ = true;
        touch_saw_x_ = true;
        touch_pos_.x(ev.value * gr_fb_width() / (touch_max_.x() - touch_min_.x()));
        if (touch_reported_ && touch_saw_y_) {
          OnTouchTrack();
          touch_saw_x_ = touch_saw_y_ = false;
        }
        break;

      case ABS_MT_POSITION_Y:
        touch_finger_down_ = true;
        touch_saw_y_ = true;
        touch_pos_.y(ev.value * gr_fb_height() / (touch_max_.y() - touch_min_.y()));
        if (touch_reported_ && touch_saw_x_) {
          OnTouchTrack();
          touch_saw_x_ = touch_saw_y_ = false;
        }
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
    std::lock_guard<std::mutex> lg(event_queue_mutex);
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
          android::volmgr::VolumeManager::Instance()->unmountAll();
          reboot("reboot,");
          while (true) {
            pause();
          }
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
    std::lock_guard<std::mutex> lg(event_queue_mutex);
    if (key_last_down == key_code && key_down_count == count) {
      long_press = key_long_press = true;
    }
  }
  if (long_press) KeyLongPress(key_code);
}

void RecoveryUI::EnqueueKey(int key_code) {
  std::lock_guard<std::mutex> lg(event_queue_mutex);
  const int queue_max = sizeof(event_queue) / sizeof(event_queue[0]);
  if (event_queue_len < queue_max) {
    InputEvent event(key_code);
    event_queue[event_queue_len++] = event;
    event_queue_cond.notify_one();
  }
}

void RecoveryUI::EnqueueTouch(const Point& pos) {
  std::lock_guard<std::mutex> lg(event_queue_mutex);
  const int queue_max = sizeof(event_queue) / sizeof(event_queue[0]);
  if (event_queue_len < queue_max) {
    InputEvent event(pos);
    event_queue[event_queue_len++] = event;
    event_queue_cond.notify_one();
  }
}

void RecoveryUI::SetScreensaverState(ScreensaverState state) {
  switch (state) {
    case ScreensaverState::NORMAL:
      if (android::base::WriteStringToFile(std::to_string(brightness_normal_value_),
                                           brightness_file_)) {
        screensaver_state_ = ScreensaverState::NORMAL;
        LOG(INFO) << "Brightness: " << brightness_normal_value_ << " (" << brightness_normal_
                  << "%)";
      } else {
        LOG(ERROR) << "Unable to set brightness to normal";
      }
      break;
    case ScreensaverState::DIMMED:
      if (android::base::WriteStringToFile(std::to_string(brightness_dimmed_value_),
                                           brightness_file_)) {
        LOG(INFO) << "Brightness: " << brightness_dimmed_value_ << " (" << brightness_dimmed_
                  << "%)";
        screensaver_state_ = ScreensaverState::DIMMED;
      } else {
        LOG(ERROR) << "Unable to set brightness to dim";
      }
      break;
    case ScreensaverState::OFF:
      if (android::base::WriteStringToFile("0", brightness_file_)) {
        LOG(INFO) << "Brightness: 0 (off)";
        screensaver_state_ = ScreensaverState::OFF;
      } else {
        LOG(ERROR) << "Unable to set brightness to off";
      }
      break;
    default:
      LOG(ERROR) << "Invalid screensaver state";
  }
}

RecoveryUI::InputEvent RecoveryUI::WaitInputEvent() {
  std::unique_lock<std::mutex> lk(event_queue_mutex);

  // Check for a saved key queue interruption.
  if (key_interrupted_) {
    SetScreensaverState(ScreensaverState::NORMAL);
    return InputEvent(EventType::EXTRA, KeyError::INTERRUPTED);
  }

  // Time out after UI_WAIT_KEY_TIMEOUT_SEC, unless a USB cable is plugged in.
  do {
    bool rc = event_queue_cond.wait_for(lk, std::chrono::seconds(UI_WAIT_KEY_TIMEOUT_SEC), [this] {
      return this->event_queue_len != 0 || key_interrupted_;
    });
    if (key_interrupted_) {
      SetScreensaverState(ScreensaverState::NORMAL);
      return InputEvent(EventType::EXTRA, KeyError::INTERRUPTED);
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
          if (event_queue_len > 0) {
            memcpy(&event_queue[0], &event_queue[1], sizeof(int) * --event_queue_len);
          }
        }

        // Reset the brightness to normal.
        SetScreensaverState(ScreensaverState::NORMAL);
      }
    }
  } while (IsUsbConnected() && event_queue_len == 0);

  InputEvent event;
  if (event_queue_len > 0) {
    event = event_queue[0];
    memcpy(&event_queue[0], &event_queue[1], sizeof(InputEvent) * --event_queue_len);
  }
  return event;
}

void RecoveryUI::CancelWaitKey() {
  EnqueueKey(KEY_AGAIN);
}

void RecoveryUI::InterruptKey() {
  {
    std::lock_guard<std::mutex> lg(event_queue_mutex);
    key_interrupted_ = true;
  }
  event_queue_cond.notify_one();
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
  std::lock_guard<std::mutex> lg(event_queue_mutex);
  int pressed = key_pressed[key];
  return pressed;
}

bool RecoveryUI::IsLongPress() {
  std::lock_guard<std::mutex> lg(event_queue_mutex);
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
  std::lock_guard<std::mutex> lg(event_queue_mutex);
  event_queue_len = 0;
}

RecoveryUI::KeyAction RecoveryUI::CheckKey(int key, bool is_long_press) {
  {
    std::lock_guard<std::mutex> lg(event_queue_mutex);
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

  last_key = key;
  return (IsTextVisible() || screensaver_state_ == ScreensaverState::OFF) ? ENQUEUE : IGNORE;
}

void RecoveryUI::KeyLongPress(int) {}

void RecoveryUI::SetEnableReboot(bool enabled) {
  std::lock_guard<std::mutex> lg(event_queue_mutex);
  enable_reboot = enabled;
}
