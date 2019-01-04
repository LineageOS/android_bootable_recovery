/*
 * Copyright (C) 2015 The Android Open Source Project
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

#include <vector>
#include <string>

#include <sys/mount.h>

//XXX
#define CONFIG_EXFAT_DRIVER "exfat"

using android::base::StringPrintf;

namespace android {
namespace volmgr {
namespace exfat {

static const char* kMkfsPath = "/sbin/mkfs.exfat";
static const char* kFsckPath = "/sbin/fsck.exfat";

bool IsSupported() {
    return access(kMkfsPath, X_OK) == 0
            && access(kFsckPath, X_OK) == 0
            && IsFilesystemSupported("exfat");
}

status_t Check(const std::string& source) {
    std::vector<std::string> cmd;
    cmd.push_back(kFsckPath);
    cmd.push_back(source);

    // Exfat devices are currently always untrusted
    return ForkExecvp(cmd, sFsckUntrustedContext);
}

status_t Mount(const std::string& source, const std::string& target, bool ro,
        bool remount, bool executable, int ownerUid, int ownerGid, int permMask) {
    int rc;
    unsigned long flags;
    char mountData[255];

    const char* c_source = source.c_str();
    const char* c_target = target.c_str();

    flags = MS_NODEV | MS_NOSUID | MS_DIRSYNC | MS_NOATIME;

    flags |= (executable ? 0 : MS_NOEXEC);
    flags |= (ro ? MS_RDONLY : 0);
    flags |= (remount ? MS_REMOUNT : 0);

    snprintf(mountData, sizeof(mountData),
            "uid=%d,gid=%d,fmask=%o,dmask=%o",
            ownerUid, ownerGid, permMask, permMask);

    rc = mount(c_source, c_target, CONFIG_EXFAT_DRIVER, flags, mountData);

    if (rc && errno == EROFS) {
        SLOGE("%s appears to be a read only filesystem - retrying mount RO", c_source);
        flags |= MS_RDONLY;
        rc = mount(c_source, c_target, CONFIG_EXFAT_DRIVER, flags, mountData);
    }

    return rc;
}

status_t Format(const std::string& source) {
    std::vector<std::string> cmd;
    cmd.push_back(kMkfsPath);
    cmd.push_back(source);

    return ForkExecvp(cmd);
}

}  // namespace exfat
}  // namespace volmgr
}  // namespace android
