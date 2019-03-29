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

#include "Utils.h"
#include "EmulatedVolume.h"
#include <volume_manager/VolumeManager.h>
#include "ResponseCode.h"

#include <android-base/stringprintf.h>
#include <android-base/logging.h>
#include <cutils/fs.h>
#include <private/android_filesystem_config.h>

#include <fcntl.h>
#include <stdlib.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>

#include <fs_mgr.h>

using android::base::StringPrintf;

namespace android {
namespace volmgr {

static const std::string kStagingPath = "/mnt/staging/emulated/0";

EmulatedVolume::EmulatedVolume(fstab_rec* rec, const std::string& subdir) :
        VolumeBase(Type::kEmulated),
        mSubdir(subdir),
        mDevPath(rec->blk_device),
        mFsType(rec->fs_type),
        mFlags(rec->flags),
        mFsOptions(rec->fs_options) {
    setId("emulated");
    setPartLabel("emulated");
    setPath("/storage/emulated/0");
}

EmulatedVolume::~EmulatedVolume() {
}

status_t EmulatedVolume::doMount() {
    if (createMountPointRecursive(kStagingPath, 0700, AID_ROOT, AID_ROOT)) {
        return -errno;
    }
    if (createMountPointRecursive(getPath(), 0700, AID_ROOT, AID_ROOT)) {
        return -errno;
    }

    std::string bindPath = kStagingPath + "/" + mSubdir;

    if (::mount(mDevPath.c_str(), kStagingPath.c_str(),
                mFsType.c_str(), mFlags, mFsOptions.c_str()) != 0) {
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

int EmulatedVolume::createMountPointRecursive(const std::string& path, mode_t mode, uid_t uid, gid_t gid) {
    auto pos = path.find("/", 1);
    while (pos != std::string::npos) {
        std::string tmp = path.substr(0, pos);
        mkdir(tmp.c_str(), mode);
        pos = path.find("/", pos + 1);
    }

    if (fs_prepare_dir(path.c_str(), mode, uid, gid)) {
        PLOG(ERROR) << getId() << " failed to create mount point " << path.c_str();
        return -1;
    }

    return 0;
}

}  // namespace volmgr
}  // namespace android
