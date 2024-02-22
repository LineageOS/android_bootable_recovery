/*
 * Copyright (C) 2024 The Android Open Source Project
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
#include <android-base/logging.h>
#include <android-base/properties.h>
#include <bootloader_message/bootloader_message.h>
#include <log/log.h>

#include <string>

#include <cstdio>

static int check_control_message() {
  misc_control_message m;
  std::string err;
  if (!ReadMiscControlMessage(&m, &err)) {
    LOG(ERROR) << "Could not read misctrl message: " << err.c_str();
    return 1;
  }

  if (m.magic != MISC_CONTROL_MAGIC_HEADER || m.version != MISC_CONTROL_MESSAGE_VERSION) {
    LOG(WARNING) << "misctrl message invalid, resetting it";
    m = { .version = MISC_CONTROL_MESSAGE_VERSION,
          .magic = MISC_CONTROL_MAGIC_HEADER,
          .misctrl_flags = 0 };
  }

  int res = 0;

  const size_t ps = getpagesize();

  if (ps != 4096 && ps != 16384) {
    LOG(ERROR) << "Unrecognized page size: " << ps;
    res = 1;
  }

  if (ps == 16384) {
    m.misctrl_flags |= MISC_CONTROL_16KB_BEFORE;
  }

  bool before_16kb = m.misctrl_flags & MISC_CONTROL_16KB_BEFORE;
  res |= android::base::SetProperty("ro.misctrl.16kb_before", before_16kb ? "1" : "0");

  if (!WriteMiscControlMessage(m, &err)) {
    LOG(ERROR) << "Could not write misctrl message: " << err.c_str();
    res |= 1;
  }

  return res;
}

static int check_reserved_space() {
  bool empty;
  std::string err;
  bool success = CheckReservedSystemSpaceEmpty(&empty, &err);
  if (!success) {
    LOG(ERROR) << "Could not read reserved space: " << err.c_str();
    return 1;
  }
  LOG(INFO) << "System reserved space empty? " << empty;

  if (!err.empty()) {
    LOG(ERROR) << "Reserved misc space being used: " << err;
  }

  return empty ? 0 : 1;
}

int main(int argc, char** argv) {
  {
    using namespace android::base;
    (void)argc;
    InitLogging(argv, TeeLogger(LogdLogger(), &StderrLogger));
  }
  int err = 0;
  err |= check_control_message();
  err |= check_reserved_space();
  return err;
}
