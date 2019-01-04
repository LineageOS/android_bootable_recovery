/*
 * Copyright (C) 2015 Cyanogen, Inc.
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

#include "DiskPartition.h"
#include "PublicVolume.h"
#include <volume_manager/ResponseCode.h>
#include <volume_manager/VolumeManager.h>
#include "Utils.h"
#include "VolumeBase.h"

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/stringprintf.h>
#include <diskconfig/diskconfig.h>

#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <vector>

using android::base::ReadFileToString;
using android::base::WriteStringToFile;
using android::base::StringPrintf;

namespace android {
namespace volmgr {

DiskPartition::DiskPartition(const std::string& eventPath, dev_t device, const std::string& nickname,
                             int flags, int partnum, const std::string& fstype /* = "" */,
                             const std::string& mntopts /* = "" */)
    : Disk(eventPath, device, nickname, flags),
      mPartNum(partnum),
      mFsType(fstype),
      mMntOpts(mntopts) {
    // Empty
}

DiskPartition::~DiskPartition() {
    // Empty
}

status_t DiskPartition::create() {
    CHECK(!mCreated);
    mCreated = true;
    VolumeManager::Instance()->notifyEvent(ResponseCode::DiskCreated, StringPrintf("%d", mFlags));
    dev_t partDevice = makedev(major(mDevice), minor(mDevice) + mPartNum);
    createPublicVolume(partDevice, mFsType, mMntOpts);
    return OK;
}

status_t DiskPartition::destroy() {
    CHECK(mCreated);
    destroyAllVolumes();
    mCreated = false;
    VolumeManager::Instance()->notifyEvent(ResponseCode::DiskDestroyed);
    return OK;
}

}  // namespace volmgr
}  // namespace android
