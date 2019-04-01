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

#ifndef RECOVERY_UI_H
#define RECOVERY_UI_H

#include <linux/input.h>
#include <pthread.h>
#include <time.h>

#include <string>
#include <vector>

enum menu_type_t { MT_NONE, MT_LIST, MT_GRID };

class MenuItem {
 public:
  MenuItem() {}
  explicit MenuItem(const std::string& text, const std::string& icon_name = "",
                    const std::string& icon_name_sel = "")
      : text_(text), icon_name_(icon_name), icon_name_sel_(icon_name_sel) {}

  const std::string& text() const {
    return text_;
  }
  const std::string& icon_name() const {
    return icon_name_;
  }
  const std::string& icon_name_sel() const {
    return icon_name_sel_;
  }

 private:
  std::string text_;
  std::string icon_name_;
  std::string icon_name_sel_;
};
typedef std::vector<MenuItem> MenuItemVector;

/*
 * Simple representation of a (x,y) coordinate with convenience operators
 */
class Point {
 public:
  Point() : x_(0), y_(0) {}
  Point(int x, int y) : x_(x), y_(y) {}
  int x() const {
    return x_;
  }
  int y() const {
    return y_;
  }
  void x(int x) {
    x_ = x;
  }
  void y(int y) {
    y_ = y;
  }

  bool operator==(const Point& rhs) const {
    return (x() == rhs.x() && y() == rhs.y());
  }
  bool operator!=(const Point& rhs) const {
    return !(*this == rhs);
  }

  Point operator+(const Point& rhs) const {
    Point tmp;
    tmp.x_ = x_ + rhs.x_;
    tmp.y_ = y_ + rhs.y_;
    return tmp;
  }
  Point operator-(const Point& rhs) const {
    Point tmp;
    tmp.x_ = x_ - rhs.x_;
    tmp.y_ = y_ - rhs.y_;
    return tmp;
  }

 private:
  int x_;
  int y_;
};

// Abstract class for controlling the user interface during recovery.
class RecoveryUI {
 public:
  enum Icon {
    NONE,
    INSTALLING_UPDATE,
    ERASING,
    NO_COMMAND,
    ERROR
  };

  enum ProgressType {
    EMPTY,
    INDETERMINATE,
    DETERMINATE
  };

  enum KeyAction {
    ENQUEUE,
    TOGGLE,
    REBOOT,
    IGNORE
  };

  RecoveryUI();

  virtual ~RecoveryUI() {}

  // Initializes the object; called before anything else. UI texts will be initialized according to
  // the given locale. Returns true on success.
  virtual bool Init(const std::string& locale);

  virtual void Stop();

  // Shows a stage indicator. Called immediately after Init().
  virtual void SetStage(int current, int max) = 0;

  // Sets the overall recovery state ("background image").
  virtual void SetBackground(Icon icon) = 0;
  virtual void SetSystemUpdateText(bool security_update) = 0;

  // --- progress indicator ---
  virtual void SetProgressType(ProgressType determinate) = 0;

  // Shows a progress bar and define the scope of the next operation:
  //   portion - fraction of the progress bar the next operation will use
  //   seconds - expected time interval (progress bar moves at this minimum rate)
  virtual void ShowProgress(float portion, float seconds) = 0;

  // Sets progress bar position (0.0 - 1.0 within the scope defined by the last call to
  // ShowProgress).
  virtual void SetProgress(float fraction) = 0;

  // --- text log ---

  virtual void ShowText(bool visible) = 0;

  virtual bool IsTextVisible() = 0;

  virtual bool WasTextEverVisible() = 0;

  virtual void UpdateScreenOnPrint(bool update) = 0;

  // Writes a message to the on-screen log (shown if the user has toggled on the text display).
  // Print() will also dump the message to stdout / log file, while PrintOnScreenOnly() not.
  virtual void Print(const char* fmt, ...) __printflike(2, 3) = 0;
  virtual void PrintOnScreenOnly(const char* fmt, ...) __printflike(2, 3) = 0;

  virtual int ShowFile(const char* filename) = 0;

  virtual void Redraw() = 0;

  // --- event handling ---

  enum event_type_t { EVENT_TYPE_NONE, EVENT_TYPE_KEY, EVENT_TYPE_TOUCH };
  class InputEvent {
   public:
    InputEvent() : type_(EVENT_TYPE_NONE), evt_({ 0 }) {}
    explicit InputEvent(int key) : type_(EVENT_TYPE_KEY), evt_({ key }) {}
    explicit InputEvent(const Point& pos) : type_(EVENT_TYPE_TOUCH), evt_({ 0 }) {
      evt_.pos = pos;
    }

    event_type_t type() const {
      return type_;
    }
    int key() const {
      return evt_.key;
    }
    const Point& pos() const {
      return evt_.pos;
    }

   private:
    event_type_t type_;
    union {
      int key;
      Point pos;
    } evt_;
  };

  // Waits for a key and return it. May return -1 after timeout.
  virtual InputEvent WaitInputEvent();

  // Cancel a WaitKey()
  virtual void CancelWaitKey();

  virtual bool IsKeyPressed(int key);
  virtual bool IsLongPress();

  // Returns true if you have the volume up/down and power trio typical of phones and tablets, false
  // otherwise.
  virtual bool HasThreeButtons();

  // Returns true if it has a power key.
  virtual bool HasPowerKey() const;

  // Returns true if it supports touch inputs.
  virtual bool HasTouchScreen() const;

  // Erases any queued-up keys.
  virtual void FlushKeys();

  // Called on each key press, even while operations are in progress. Return value indicates whether
  // an immediate operation should be triggered (toggling the display, rebooting the device), or if
  // the key should be enqueued for use by the main thread.
  virtual KeyAction CheckKey(int key, bool is_long_press);

  // Called when a key is held down long enough to have been a long-press (but before the key is
  // released). This means that if the key is eventually registered (released without any other keys
  // being pressed in the meantime), CheckKey will be called with 'is_long_press' true.
  virtual void KeyLongPress(int key);

  // Normally in recovery there's a key sequence that triggers immediate reboot of the device,
  // regardless of what recovery is doing (with the default CheckKey implementation, it's pressing
  // the power button 7 times in row). Call this to enable or disable that feature. It is enabled by
  // default.
  virtual void SetEnableReboot(bool enabled);

  // --- menu display ---

  // Display some header text followed by a menu of items, which appears at the top of the screen
  // (in place of any scrolling ui_print() output, if necessary).
  virtual void StartMenu(bool is_main, menu_type_t type, const char* const* headers,
                         const MenuItemVector& items, int initial_selection) = 0;

  // Sets the menu highlight to the given index, wrapping if necessary. Returns the actual item
  // selected.
  virtual int SelectMenu(int sel) = 0;
  virtual int SelectMenu(const Point& point) = 0;

  // Scroll the view by increasing or lowering the first shown item
  // If updown < 0, scroll up by |updown| items. If updown > 0, scroll down by |updown| items
  // Returns the selected item, since scrolling past a selected item will change the selection to
  // the closest on screen item
  virtual int ScrollMenu(int updown) = 0;

  // Ends menu mode, resetting the text overlay so that ui_print() statements will be displayed.
  virtual void EndMenu() = 0;

  // Notify of volume state change
  void onVolumeChanged() {
    volumes_changed_ = 1;
  }
  bool VolumesChanged();

  virtual bool MenuShowing() const = 0;
  virtual bool MenuScrollable() const = 0;
  virtual int MenuItemStart() const = 0;
  virtual int MenuItemHeight() const = 0;

 protected:
  void EnqueueKey(int key_code);
  void EnqueueTouch(const Point& pos);

  std::string android_version_;
  std::string lineage_version_;

  // The normal and dimmed brightness percentages (default: 50 and 25, which means 50% and 25% of
  // the max_brightness). Because the absolute values may vary across devices. These two values can
  // be configured via subclassing. Setting brightness_normal_ to 0 to disable screensaver.
  unsigned int brightness_normal_;
  unsigned int brightness_dimmed_;
  std::string brightness_file_;
  std::string max_brightness_file_;

  // Whether we should listen for touch inputs (default: true).
  bool touch_screen_allowed_;

 private:
  enum class ScreensaverState {
    DISABLED,
    NORMAL,
    DIMMED,
    OFF
  };

  struct key_timer_t {
    RecoveryUI* ui;
    int key_code;
    int count;
  };

  // The sensitivity when detecting a swipe.
  const int kTouchLowThreshold;
  const int kTouchHighThreshold;

  void OnTouchDeviceDetected(int fd);
  void OnKeyDetected(int key_code);
  void OnTouchPress();
  void OnTouchTrack();
  void OnTouchRelease();
  int OnInputEvent(int fd, uint32_t epevents);
  void ProcessKey(int key_code, int updown);

  bool IsUsbConnected();

  static void* time_key_helper(void* cookie);
  void time_key(int key_code, int count);

  bool InitScreensaver();

  // Key event input queue
  pthread_mutex_t event_queue_mutex;
  pthread_cond_t event_queue_cond;
  InputEvent event_queue[256];
  int event_queue_len;
  char key_pressed[KEY_MAX + 1];  // under event_queue_mutex
  int key_last_down;              // under event_queue_mutex
  bool key_long_press;            // under event_queue_mutex
  int key_down_count;             // under event_queue_mutex
  bool enable_reboot;             // under event_queue_mutex
  int rel_sum;

  int consecutive_power_keys;
  int last_key;

  bool has_power_key;
  bool has_up_key;
  bool has_down_key;
  bool has_touch_screen;

  struct vkey_t {
    bool inside(const Point& p) const {
      return (p.x() >= min_.x() && p.x() < max_.x() && p.y() >= min_.y() && p.y() < max_.y());
    }

    int keycode;
    Point min_;
    Point max_;
  };

  // Touch event related variables. See the comments in RecoveryUI::OnInputEvent().
  int touch_slot_;
  bool touch_finger_down_;
  bool touch_saw_x_;
  bool touch_saw_y_;
  bool touch_reported_;
  Point touch_pos_;
  Point touch_start_;
  Point touch_track_;
  bool has_swiped_;
  std::vector<vkey_t> virtual_keys_;
  bool is_bootreason_recovery_ui_;

  pthread_t input_thread_;

  bool volumes_changed_;

  ScreensaverState screensaver_state_;

  // The following two contain the absolute values computed from brightness_normal_ and
  // brightness_dimmed_ respectively.
  unsigned int brightness_normal_value_;
  unsigned int brightness_dimmed_value_;
};

#endif  // RECOVERY_UI_H
