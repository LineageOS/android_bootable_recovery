/*
 * Copyright (C) 2012 The Android Open Source Project
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

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <string>
#include <vector>

#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <linux/kdev_t.h>

#define LOG_TAG "Vold"
#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android-base/stringprintf.h>
#include <cutils/log.h>
#include <cutils/properties.h>
#include <logwrap/logwrap.h>
#include <private/android_filesystem_config.h>
#include <selinux/selinux.h>

#include "Ext4.h"
#include "Utils.h"

using android::base::StringPrintf;

namespace android {
namespace volmgr {
namespace ext4 {

status_t Mount(const std::string& source, const std::string& target, bool ro, bool remount,
               bool executable, const std::string& opts /* = "" */, bool trusted, bool portable) {
    int rc;
    unsigned long flags;

    std::string data(opts);

    if (portable) {
        if (!data.empty()) {
            data += ",";
        }
        data += "context=u:object_r:sdcard_posix:s0";
    }
    const char* c_source = source.c_str();
    const char* c_target = target.c_str();
    const char* c_data = data.c_str();

    flags = MS_NOATIME | MS_NODEV | MS_NOSUID;

    // Only use MS_DIRSYNC if we're not mounting adopted storage
    if (!trusted) {
        flags |= MS_DIRSYNC;
    }

    flags |= (executable ? 0 : MS_NOEXEC);
    flags |= (ro ? MS_RDONLY : 0);
    flags |= (remount ? MS_REMOUNT : 0);

    rc = mount(c_source, c_target, "ext4", flags, c_data);
    if (portable && rc == 0) {
        chown(c_target, AID_MEDIA_RW, AID_MEDIA_RW);
        chmod(c_target, 0775);
    }

    if (rc && errno == EROFS) {
        SLOGE("%s appears to be a read only filesystem - retrying mount RO", c_source);
        flags |= MS_RDONLY;
        rc = mount(c_source, c_target, "ext4", flags, c_data);
    }

    return rc;
}

}  // namespace ext4
}  // namespace volmgr
}  // namespace android
