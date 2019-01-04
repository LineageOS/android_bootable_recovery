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

#ifndef ANDROID_VOLMGR_DISK_H
#define ANDROID_VOLMGR_DISK_H

#include "Utils.h"
#include "VolumeBase.h"

#include <utils/Errors.h>

#include <vector>

#include <volume_manager/VolumeManager.h>

namespace android {
namespace volmgr {

class VolumeBase;

/*
 * Representation of detected physical media.
 *
 * Knows how to create volumes based on the partition tables found, and also
 * how to repartition itself.
 */
class Disk {
  public:
    Disk(const std::string& eventPath, dev_t device, const std::string& nickname, int flags);
    virtual ~Disk();

    enum Flags {
        /* Flag that disk is adoptable */
        kAdoptable = 1 << 0,
        /* Flag that disk is considered primary when the user hasn't
         * explicitly picked a primary storage location */
        kDefaultPrimary = 1 << 1,
        /* Flag that disk is SD card */
        kSd = 1 << 2,
        /* Flag that disk is USB disk */
        kUsb = 1 << 3,
        /* Flag that disk is EMMC internal */
        kEmmc = 1 << 4,
        /* Flag that disk is non-removable */
        kNonRemovable = 1 << 5,
    };

    const std::string& getId() { return mId; }
    const std::string& getEventPath() { return mEventPath; }
    const std::string& getSysPath() { return mSysPath; }
    const std::string& getDevPath() { return mDevPath; }
    dev_t getDevice() { return mDevice; }
    uint64_t getSize() { return mSize; }
    const std::string& getLabel() { return mLabel; }
    int getFlags() { return mFlags; }

    void getVolumeInfo(std::vector<VolumeInfo>& info);

    std::shared_ptr<VolumeBase> findVolume(const std::string& id);

    void listVolumes(VolumeBase::Type type, std::list<std::string>& list);

    virtual status_t create();
    virtual status_t destroy();

    virtual status_t readMetadata();
    virtual status_t readPartitions();

    status_t unmountAll();

  protected:
    /* ID that uniquely references this disk */
    std::string mId;
    /* Original event path */
    std::string mEventPath;
    /* Device path under sysfs */
    std::string mSysPath;
    /* Device path under dev */
    std::string mDevPath;
    /* Kernel device representing disk */
    dev_t mDevice;
    /* Size of disk, in bytes */
    uint64_t mSize;
    /* User-visible label, such as manufacturer */
    std::string mLabel;
    /* Current partitions on disk */
    std::vector<std::shared_ptr<VolumeBase>> mVolumes;
    /* Nickname for this disk */
    std::string mNickname;
    /* Flags applicable to this disk */
    int mFlags;
    /* Flag indicating object is created */
    bool mCreated;
    /* Flag that we need to skip first disk change events after partitioning*/
    bool mSkipChange;

    void createPublicVolume(dev_t device, const std::string& fstype = "",
                            const std::string& mntopts = "");

    void destroyAllVolumes();

    int getMaxMinors();

    DISALLOW_COPY_AND_ASSIGN(Disk);
};

}  // namespace volmgr
}  // namespace android

#endif
