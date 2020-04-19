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

#include "Ntfs.h"
#include "Utils.h"

#include <android-base/logging.h>
#include <android-base/stringprintf.h>

#include <string>
#include <vector>

#include <sys/mount.h>

using android::base::StringPrintf;

namespace android {
namespace volmgr {
namespace ntfs {

static const char* kFsckPath = "/sbin/fsck.ntfs";
static const char* kMountPath = "/sbin/mount.ntfs";

status_t Check(const std::string& source) {
    std::vector<std::string> cmd;
    cmd.push_back(kFsckPath);
    cmd.push_back("-n");
    cmd.push_back(source);

    // Ntfs devices are currently always untrusted
    return ForkExecvp(cmd, sFsckUntrustedContext);
}

status_t Mount(const std::string& source, const std::string& target, bool ro, bool remount,
               bool executable, int ownerUid, int ownerGid, int permMask) {
    char mountData[255];

    const char* c_source = source.c_str();
    const char* c_target = target.c_str();

    sprintf(mountData,
            "utf8,uid=%d,gid=%d,fmask=%o,dmask=%o,"
            "shortname=mixed,nodev,nosuid,dirsync",
            ownerUid, ownerGid, permMask, permMask);

    if (!executable) strlcat(mountData, ",noexec", sizeof(mountData));
    if (ro) strlcat(mountData, ",ro", sizeof(mountData));
    if (remount) strlcat(mountData, ",remount", sizeof(mountData));

    std::vector<std::string> cmd;
    cmd.push_back(kMountPath);
    cmd.push_back("-o");
    cmd.push_back(mountData);
    cmd.push_back(c_source);
    cmd.push_back(c_target);

    return ForkExecvp(cmd);
}

}  // namespace ntfs
}  // namespace volmgr
}  // namespace android
