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

#ifndef ANDROID_VOLMGR_PUBLIC_VOLUME_H
#define ANDROID_VOLMGR_PUBLIC_VOLUME_H

#include "VolumeBase.h"

#include <cutils/multiuser.h>

namespace android {
namespace volmgr {

/*
 * Shared storage provided by public (vfat) partition.
 *
 * Knows how to mount itself and then spawn a FUSE daemon to synthesize
 * permissions.
 *
 * This volume is not inherently multi-user aware, so it has two possible
 * modes of operation:
 * 1. If primary storage for the device, it only binds itself to the
 * owner user.
 * 2. If secondary storage, it binds itself for all users, but masks
 * away the Android directory for secondary users.
 */
class PublicVolume : public VolumeBase {
  public:
    PublicVolume(dev_t device, const std::string& nickname, const std::string& mntopts = "",
                 const std::string& fstype = "");
    virtual ~PublicVolume();

  protected:
    status_t doCreate() override;
    status_t doDestroy() override;
    status_t doMount() override;
    status_t doUnmount(bool detach = false) override;

    status_t readMetadata();

  private:
    /* Kernel device representing partition */
    dev_t mDevice;
    /* Block device path */
    std::string mDevPath;

    /* Filesystem type */
    std::string mFsType;
    /* Filesystem UUID */
    std::string mFsUuid;
    /* Mount options */
    std::string mMntOpts;

    DISALLOW_COPY_AND_ASSIGN(PublicVolume);
};

}  // namespace volmgr
}  // namespace android

#endif
