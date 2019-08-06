/*
 * Copyright (C) 2011 The Android Open Source Project
 * Copyright (C) 2019 The LineageOS Project
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

#include "ui.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <functional>
#include <string>

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/parseint.h>
#include <android-base/properties.h>
#include <android-base/strings.h>
#include <cutils/android_reboot.h>
#include <cutils/properties.h>
#include <minui/minui.h>

#include <volume_manager/VolumeManager.h>

#include "common.h"
#include "roots.h"
#include "device.h"

static constexpr int UI_WAIT_KEY_TIMEOUT_SEC = 120;
static constexpr const char* BRIGHTNESS_FILE = BACKLIGHT_PATH "/brightness";
static constexpr const char* MAX_BRIGHTNESS_FILE = BACKLIGHT_PATH "/max_brightness";
static constexpr const char* BRIGHTNESS_FILE_SDM =
    "/sys/class/backlight/panel0-backlight/brightness";
static constexpr const char* MAX_BRIGHTNESS_FILE_SDM =
    "/sys/class/backlight/panel0-backlight/max_brightness";

RecoveryUI::RecoveryUI()
    : brightness_normal_(50),
      brightness_dimmed_(25),
      brightness_file_(BRIGHTNESS_FILE),
      max_brightness_file_(MAX_BRIGHTNESS_FILE),
      touch_screen_allowed_(true),
      kTouchLowThreshold(RECOVERY_UI_TOUCH_LOW_THRESHOLD),
      kTouchHighThreshold(RECOVERY_UI_TOUCH_HIGH_THRESHOLD),
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
      has_swiped_(false),
      is_bootreason_recovery_ui_(false),
      volumes_changed_(false),
      screensaver_state_(ScreensaverState::DISABLED) {
  char propval[PROPERTY_VALUE_MAX];
  property_get("ro.build.version.release", propval, "(unknown)");
  android_version_ = std::string("Android ") + propval;
  property_get("ro.lineage.version", propval, "(unknown)");
  lineage_version_ = std::string("LineageOS ") + propval;

  pthread_mutex_init(&event_queue_mutex, nullptr);
  pthread_cond_init(&event_queue_cond, nullptr);
  memset(key_pressed, 0, sizeof(key_pressed));
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

// Reads input events, handles special hot keys, and adds to the key queue.
static void* InputThreadLoop(void*) {
  while (true) {
    if (!ev_wait(-1)) {
      ev_dispatch();
    }
  }
  return nullptr;
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

  pthread_create(&input_thread_, nullptr, InputThreadLoop, nullptr);
  return true;
}

void RecoveryUI::Stop() {
  if (!android::base::WriteStringToFile("0", BRIGHTNESS_FILE)) {
    PLOG(WARNING) << "Failed to write brightness file";
  }
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
  // Arbitrary swipe threshold to ignore the following release.
  // Use MenuItemHeight as a quick way to get a density sensitive value
  static const int kSwipeThresh = MenuItemHeight() / 30;

  if (touch_pos_.y() <= gr_fb_height()) {
    if (MenuShowing() && MenuScrollable()) {
      int dy = touch_pos_.y() - touch_track_.y();
      if (abs(dy) > kSwipeThresh) {
        has_swiped_ = true;
      }
      if (abs(dy) >= MenuItemHeight()) {
        int key = (dy < 0) ? KEY_SCROLLDOWN : KEY_SCROLLUP;  // natural scrolling
        ProcessKey(key, 1);  // press key
        ProcessKey(key, 0);  // and release it
        int sgn = (dy > 0) - (dy < 0);
        touch_track_.y(touch_track_.y() + sgn * MenuItemHeight());
        has_swiped_ = true;
      }
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
  if (has_swiped_) {
    has_swiped_ = false;
    return;
  }

  // Check for horizontal swipe
  Point delta = touch_pos_ - touch_start_;
  if (abs(delta.y()) < kTouchLowThreshold && abs(delta.x()) > kTouchHighThreshold) {
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

// Process a key-up or -down event.  A key is "registered" when it is
// pressed and then released, with no other keypresses or releases in
// between.  Registered keys are passed to CheckKey() to see if it
// should trigger a visibility toggle, an immediate reboot, or be
// queued to be processed next time the foreground thread wants a key
// (eg, for the menu).
//
// We also keep track of which keys are currently down so that
// CheckKey can call IsKeyPressed to see what other keys are held when
// a key is registered.
//
// updown == 1 for key down events; 0 for key up events
void RecoveryUI::ProcessKey(int key_code, int updown) {
  bool register_key = false;
  bool long_press = false;
  bool reboot_enabled;

  pthread_mutex_lock(&event_queue_mutex);
  key_pressed[key_code] = updown;
  if (updown) {
    ++key_down_count;
    key_last_down = key_code;
    key_long_press = false;
    key_timer_t* info = new key_timer_t;
    info->ui = this;
    info->key_code = key_code;
    info->count = key_down_count;
    pthread_t thread;
    pthread_create(&thread, nullptr, &RecoveryUI::time_key_helper, info);
    pthread_detach(thread);
  } else {
    if (key_last_down == key_code) {
      long_press = key_long_press;
      register_key = true;
    }
    key_last_down = -1;
  }
  reboot_enabled = enable_reboot;
  pthread_mutex_unlock(&event_queue_mutex);

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

void* RecoveryUI::time_key_helper(void* cookie) {
  key_timer_t* info = static_cast<key_timer_t*>(cookie);
  info->ui->time_key(info->key_code, info->count);
  delete info;
  return nullptr;
}

void RecoveryUI::time_key(int key_code, int count) {
  usleep(750000);  // 750 ms == "long"
  bool long_press = false;
  pthread_mutex_lock(&event_queue_mutex);
  if (key_last_down == key_code && key_down_count == count) {
    long_press = key_long_press = true;
  }
  pthread_mutex_unlock(&event_queue_mutex);
  if (long_press) KeyLongPress(key_code);
}

void RecoveryUI::EnqueueKey(int key_code) {
  pthread_mutex_lock(&event_queue_mutex);
  const int queue_max = sizeof(event_queue) / sizeof(event_queue[0]);
  if (event_queue_len < queue_max) {
    InputEvent event(key_code);
    event_queue[event_queue_len++] = event;
    pthread_cond_signal(&event_queue_cond);
  }
  pthread_mutex_unlock(&event_queue_mutex);
}

void RecoveryUI::EnqueueTouch(const Point& pos) {
  pthread_mutex_lock(&event_queue_mutex);
  const int queue_max = sizeof(event_queue) / sizeof(event_queue[0]);
  if (event_queue_len < queue_max) {
    InputEvent event(pos);
    event_queue[event_queue_len++] = event;
    pthread_cond_signal(&event_queue_cond);
  }
  pthread_mutex_unlock(&event_queue_mutex);
}

RecoveryUI::InputEvent RecoveryUI::WaitInputEvent() {
  pthread_mutex_lock(&event_queue_mutex);

  // Time out after UI_WAIT_KEY_TIMEOUT_SEC, unless a USB cable is
  // plugged in.
  do {
    struct timeval now;
    struct timespec timeout;
    gettimeofday(&now, nullptr);
    timeout.tv_sec = now.tv_sec;
    timeout.tv_nsec = now.tv_usec * 1000;
    timeout.tv_sec += UI_WAIT_KEY_TIMEOUT_SEC;

    int rc = 0;
    while (event_queue_len == 0 && rc != ETIMEDOUT) {
      struct timespec key_timeout;
      gettimeofday(&now, nullptr);
      key_timeout.tv_sec = now.tv_sec + 1;
      key_timeout.tv_nsec = now.tv_usec * 1000;
      rc = pthread_cond_timedwait(&event_queue_cond, &event_queue_mutex, &key_timeout);
      if (rc == ETIMEDOUT) {
        if (VolumesChanged()) {
          pthread_mutex_unlock(&event_queue_mutex);
          InputEvent event(KEY_REFRESH);
          return event;
        }
        if (key_timeout.tv_sec <= timeout.tv_sec) {
          rc = 0;
          ui->Redraw();
        }
      }
    }

    if (screensaver_state_ != ScreensaverState::DISABLED) {
      if (rc == ETIMEDOUT) {
        // Lower the brightness level: NORMAL -> DIMMED; DIMMED -> OFF.
        if (screensaver_state_ == ScreensaverState::NORMAL) {
          if (android::base::WriteStringToFile(std::to_string(brightness_dimmed_value_),
                                               brightness_file_)) {
            LOG(INFO) << "Brightness: " << brightness_dimmed_value_ << " (" << brightness_dimmed_
                      << "%)";
            screensaver_state_ = ScreensaverState::DIMMED;
          }
        } else if (screensaver_state_ == ScreensaverState::DIMMED) {
          if (android::base::WriteStringToFile("0", brightness_file_)) {
            LOG(INFO) << "Brightness: 0 (off)";
            screensaver_state_ = ScreensaverState::OFF;
          }
        }
      } else if (screensaver_state_ != ScreensaverState::NORMAL) {
        // Drop the first key if it's changing from OFF to NORMAL.
        if (screensaver_state_ == ScreensaverState::OFF) {
          if (event_queue_len > 0) {
            memcpy(&event_queue[0], &event_queue[1], sizeof(int) * --event_queue_len);
          }
        }

        // Reset the brightness to normal.
        if (android::base::WriteStringToFile(std::to_string(brightness_normal_value_),
                                             brightness_file_)) {
          screensaver_state_ = ScreensaverState::NORMAL;
          LOG(INFO) << "Brightness: " << brightness_normal_value_ << " (" << brightness_normal_
                    << "%)";
        }
      }
    }
  } while (IsUsbConnected() && event_queue_len == 0);

  InputEvent event;
  if (event_queue_len > 0) {
    event = event_queue[0];
    memcpy(&event_queue[0], &event_queue[1], sizeof(event_queue[0]) * --event_queue_len);
  }
  pthread_mutex_unlock(&event_queue_mutex);
  return event;
}

void RecoveryUI::CancelWaitKey() {
  pthread_mutex_lock(&event_queue_mutex);
  InputEvent event(KEY_REFRESH);
  event_queue[event_queue_len++] = event;
  pthread_cond_signal(&event_queue_cond);
  pthread_mutex_unlock(&event_queue_mutex);
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
  pthread_mutex_lock(&event_queue_mutex);
  int pressed = key_pressed[key];
  pthread_mutex_unlock(&event_queue_mutex);
  return pressed;
}

bool RecoveryUI::IsLongPress() {
  pthread_mutex_lock(&event_queue_mutex);
  bool result = key_long_press;
  pthread_mutex_unlock(&event_queue_mutex);
  return result;
}

bool RecoveryUI::HasThreeButtons() {
  return has_power_key && has_up_key && has_down_key;
}

bool RecoveryUI::HasPowerKey() const {
  return has_power_key;
}

bool RecoveryUI::HasTouchScreen() const {
  return has_touch_screen;
}

void RecoveryUI::FlushKeys() {
  pthread_mutex_lock(&event_queue_mutex);
  event_queue_len = 0;
  pthread_mutex_unlock(&event_queue_mutex);
}

RecoveryUI::KeyAction RecoveryUI::CheckKey(int key, bool is_long_press) {
  pthread_mutex_lock(&event_queue_mutex);
  key_long_press = false;
  pthread_mutex_unlock(&event_queue_mutex);

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
    pthread_mutex_lock(&event_queue_mutex);
    bool reboot_enabled = enable_reboot;
    pthread_mutex_unlock(&event_queue_mutex);

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

void RecoveryUI::KeyLongPress(int) {
}

void RecoveryUI::SetEnableReboot(bool enabled) {
  pthread_mutex_lock(&event_queue_mutex);
  enable_reboot = enabled;
  pthread_mutex_unlock(&event_queue_mutex);
}

bool RecoveryUI::VolumesChanged() {
  bool ret = volumes_changed_;
  volumes_changed_ = false;
  return ret;
}
