/*
 * Copyright (C) 2008 The Android Open Source Project
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

#include <stdio.h>
#include <sys/mount.h>
#include <utils/Errors.h>
#include <android-base/stringprintf.h>
#include "Iso9660.h"
#include "Utils.h"

namespace android {
namespace volmgr {
namespace iso9660 {

bool IsIso9660Supported() {
    return IsFilesystemSupported("iso9660");
}

bool IsUdfSupported() {
    return IsFilesystemSupported("udf");
}

status_t Mount(const std::string& source, const std::string& target,
        int ownerUid, int ownerGid ) {
    int mountFlags = MS_NODEV | MS_NOEXEC | MS_NOSUID | MS_RDONLY;
    auto mountData = android::base::StringPrintf("uid=%d,gid=%d", ownerUid, ownerGid);
    if (mount(source.c_str(), target.c_str(), "iso9660", mountFlags, mountData.c_str()) == 0) {
        return 0;
    }
    if (mount(source.c_str(), target.c_str(), "udf", mountFlags, mountData.c_str()) == 0) {
        return 0;
    }
    return -1;
}

}  // namespace iso9660
}  // namespace vold
}  // namespace android
