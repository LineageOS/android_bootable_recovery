/*
 * Copyright (C) 2015 The Android Open Source Project
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

#include "Exfat.h"
#include "Utils.h"

#define LOG_TAG "Vold"

#include <android-base/logging.h>
#include <android-base/stringprintf.h>

#include <cutils/log.h>

#include <logwrap/logwrap.h>

#include <string>
#include <vector>

#include <sys/mount.h>

using android::base::StringPrintf;

namespace android {
namespace volmgr {
namespace exfat {

status_t Mount(const std::string& source, const std::string& target, int ownerUid, int ownerGid,
               int permMask) {
    int mountFlags = MS_NODEV | MS_NOSUID | MS_DIRSYNC | MS_NOATIME | MS_NOEXEC;
    auto mountData = android::base::StringPrintf("uid=%d,gid=%d,fmask=%o,dmask=%o", ownerUid,
                                                 ownerGid, permMask, permMask);

    if (mount(source.c_str(), target.c_str(), "exfat", mountFlags, mountData.c_str()) == 0) {
        return 0;
    }
    PLOG(ERROR) << "Mount failed; attempting read-only";
    mountFlags |= MS_RDONLY;
    if (mount(source.c_str(), target.c_str(), "exfat", mountFlags, mountData.c_str()) == 0) {
        return 0;
    }

    return -1;
}

}  // namespace exfat
}  // namespace volmgr
}  // namespace android
