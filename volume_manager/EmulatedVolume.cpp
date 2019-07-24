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

#include "EmulatedVolume.h"
#include <volume_manager/ResponseCode.h>
#include <volume_manager/VolumeManager.h>
#include "Utils.h"

#include <android-base/logging.h>
#include <android-base/stringprintf.h>
#include <cutils/fs.h>
#include <private/android_filesystem_config.h>

#include <fcntl.h>
#include <stdlib.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

using android::base::StringPrintf;

namespace android {
namespace volmgr {

static const std::string kStagingPath = "/mnt/staging/emulated";
static const std::string kFbeKeyVersion = kStagingPath + "/unencrypted/key/version";

EmulatedVolume::EmulatedVolume(FstabEntry* rec, const std::string& subdir)
    : VolumeBase(Type::kEmulated),
      mSubdir(subdir),
      mDevPath(rec->blk_device),
      mFsType(rec->fs_type),
      mFlags(rec->flags),
      mFsOptions(rec->fs_options) {
    setId("emulated");
    setPartLabel("internal storage");
    setPath("/storage/emulated");
}

EmulatedVolume::~EmulatedVolume() {}

status_t EmulatedVolume::doMount() {
    if (fs_prepare_dir(kStagingPath.c_str(), 0700, AID_ROOT, AID_ROOT)) {
        PLOG(ERROR) << getId() << " failed to create mount points";
        return -errno;
    }
    if (fs_prepare_dir(getPath().c_str(), 0700, AID_ROOT, AID_ROOT)) {
        PLOG(ERROR) << getId() << " failed to create mount points";
        return -errno;
    }

    std::string bindPath = kStagingPath + "/" + mSubdir;

    if (::mount(mDevPath.c_str(), kStagingPath.c_str(), mFsType.c_str(), mFlags,
                mFsOptions.c_str()) != 0) {
        PLOG(ERROR) << getId() << " failed to mount " << mDevPath << " on " << kStagingPath;
        return -EIO;
    }
    if (BindMount(bindPath, getPath()) != OK) {
        PLOG(ERROR) << getId() << " failed to bind mount " << bindPath << " on " << getPath();
        ForceUnmount(kStagingPath);
        return -EIO;
    }

    return OK;
}

status_t EmulatedVolume::doUnmount(bool detach /* = false */) {
    ForceUnmount(getPath(), detach);
    ForceUnmount(kStagingPath, detach);

    rmdir(getPath().c_str());
    rmdir(kStagingPath.c_str());

    return OK;
}

bool EmulatedVolume::detectMountable() {
    bool mountable = false;
    if (doMount() == OK) {
        // Check if FBE encrypted
        mountable = access(kFbeKeyVersion.c_str(), F_OK) != 0;
        doUnmount();
    }
    return mountable;
}

}  // namespace volmgr
}  // namespace android
