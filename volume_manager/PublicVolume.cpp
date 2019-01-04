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

#include "PublicVolume.h"
#include <volume_manager/VolumeManager.h>
#include "ResponseCode.h"
#include "Utils.h"
#include "fs/Exfat.h"
#include "fs/Ext4.h"
#include "fs/F2fs.h"
#include "fs/Ntfs.h"
#include "fs/Vfat.h"

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

using android::base::StringPrintf;

namespace android {
namespace volmgr {

PublicVolume::PublicVolume(dev_t device, const std::string& nickname,
                           const std::string& fstype /* = "" */,
                           const std::string& mntopts /* = "" */)
    : VolumeBase(Type::kPublic), mDevice(device), mFsType(fstype), mMntOpts(mntopts) {
    setId(StringPrintf("public:%u_%u", major(device), minor(device)));
    setPartLabel(nickname);
    mDevPath = StringPrintf("/dev/block/volmgr/%s", getId().c_str());
}

PublicVolume::~PublicVolume() {}

status_t PublicVolume::readMetadata() {
    std::string label;
    status_t res = ReadMetadataUntrusted(mDevPath, mFsType, mFsUuid, label);
    if (!label.empty()) {
        setPartLabel(label);
    }
    VolumeManager::Instance()->notifyEvent(ResponseCode::VolumeFsTypeChanged, mFsType);
    VolumeManager::Instance()->notifyEvent(ResponseCode::VolumeFsUuidChanged, mFsUuid);
    return res;
}

status_t PublicVolume::doCreate() {
    status_t res = CreateDeviceNode(mDevPath, mDevice);
    if (res != OK) {
        return res;
    }
    readMetadata();

    // Use UUID as stable name, if available
    std::string stableName = getId();
    if (!mFsUuid.empty()) {
        stableName = mFsUuid;
    }
    setPath(StringPrintf("/storage/%s", stableName.c_str()));

    return OK;
}

status_t PublicVolume::doDestroy() {
    return DestroyDeviceNode(mDevPath);
}

status_t PublicVolume::doMount() {
    if (!IsFilesystemSupported(mFsType)) {
        LOG(ERROR) << getId() << " unsupported filesystem " << mFsType;
        return -EIO;
    }

    if (fs_prepare_dir(getPath().c_str(), 0700, AID_ROOT, AID_ROOT)) {
        PLOG(ERROR) << getId() << " failed to create mount points";
        return -errno;
    }

    int ret = 0;
    if (mFsType == "exfat") {
        ret = exfat::Mount(mDevPath, getPath(), AID_MEDIA_RW, AID_MEDIA_RW, 0007);
    } else if (mFsType == "ext4") {
        ret = ext4::Mount(mDevPath, getPath(), false, false, true, mMntOpts, false, true);
    } else if (mFsType == "f2fs") {
        ret = f2fs::Mount(mDevPath, getPath(), mMntOpts, false, true);
    } else if (mFsType == "ntfs") {
        ret =
            ntfs::Mount(mDevPath, getPath(), false, false, false, AID_MEDIA_RW, AID_MEDIA_RW, 0007);
    } else if (mFsType == "vfat") {
        ret = vfat::Mount(mDevPath, getPath(), false, false, false, AID_MEDIA_RW, AID_MEDIA_RW,
                          0007, true);
    } else {
        ret = ::mount(mDevPath.c_str(), getPath().c_str(), mFsType.c_str(), 0, nullptr);
    }
    if (ret) {
        PLOG(ERROR) << getId() << " failed to mount " << mDevPath;
        return -EIO;
    }

    return OK;
}

status_t PublicVolume::doUnmount(bool detach /* = false */) {
    ForceUnmount(getPath(), detach);

    rmdir(getPath().c_str());

    return OK;
}

}  // namespace volmgr
}  // namespace android
