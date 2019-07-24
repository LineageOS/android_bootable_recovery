/*
 * Copyright (C) 2008 The Android Open Source Project
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

#ifndef ANDROID_VOLMGR_VOLUMEBASE_H
#define ANDROID_VOLMGR_VOLUMEBASE_H

#include "Utils.h"

#include <cutils/multiuser.h>
#include <utils/Errors.h>

#include <sys/types.h>
#include <list>
#include <string>

namespace android {
namespace volmgr {

/*
 * Representation of a mounted volume ready for presentation.
 *
 * Various subclasses handle the different mounting prerequisites, such as
 * encryption details, etc.  Volumes can also be "stacked" above other
 * volumes to help communicate dependencies.  For example, an ASEC volume
 * can be stacked on a vfat volume.
 *
 * Mounted volumes can be asked to manage bind mounts to present themselves
 * to specific users on the device.
 *
 * When an unmount is requested, the volume recursively unmounts any stacked
 * volumes and removes any bind mounts before finally unmounting itself.
 */
class VolumeBase {
  public:
    virtual ~VolumeBase();

    enum class Type {
        kPublic = 0,
        kPrivate,
        kEmulated,
        kAsec,
        kObb,
    };

    enum MountFlags {
        /* Flag that volume is primary external storage */
        kPrimary = 1 << 0,
        /* Flag that volume is visible to normal apps */
        kVisible = 1 << 1,
    };

    enum class State {
        kUnmounted = 0,
        kChecking,
        kMounted,
        kMountedReadOnly,
        kEjecting,
        kUnmountable,
        kRemoved,
        kBadRemoval,
    };

    const std::string& getId() const { return mId; }
    const std::string& getDiskId() const { return mDiskId; }
    const std::string& getPartGuid() const { return mPartGuid; }
    const std::string& getPartLabel() const { return mPartLabel; }
    Type getType() const { return mType; }
    int getMountFlags() const { return mMountFlags; }
    State getState() const { return mState; }
    const std::string& getPath() const { return mPath; }
    bool isMountable() const { return mMountable; }

    status_t setDiskId(const std::string& diskId);
    status_t setPartGuid(const std::string& partGuid);
    status_t setPartLabel(const std::string& partLabel);
    status_t setMountFlags(int mountFlags);
    status_t setSilent(bool silent);

    status_t create();
    status_t destroy();
    status_t mount();
    status_t unmount(bool detach = false);

  protected:
    explicit VolumeBase(Type type);

    virtual status_t doCreate();
    virtual status_t doDestroy();
    virtual status_t doMount() = 0;
    virtual status_t doUnmount(bool detach = false) = 0;

    status_t setId(const std::string& id);
    status_t setPath(const std::string& path);

  private:
    /* ID that uniquely references volume while alive */
    std::string mId;
    /* ID that uniquely references parent disk while alive */
    std::string mDiskId;
    /* Partition GUID of this volume */
    std::string mPartGuid;
    /* Partition label of this volume */
    std::string mPartLabel;
    /* Volume type */
    Type mType;
    /* Flags used when mounting this volume */
    int mMountFlags;
    /* Flag indicating object is created */
    bool mCreated;
    /* Current state of volume */
    State mState;
    /* Path to mounted volume */
    std::string mPath;
    /* Flag indicating that volume should emit no events */
    bool mSilent;

    bool mMountable;
    virtual bool detectMountable();

    void setState(State state);

    DISALLOW_COPY_AND_ASSIGN(VolumeBase);
};

}  // namespace volmgr
}  // namespace android

#endif
