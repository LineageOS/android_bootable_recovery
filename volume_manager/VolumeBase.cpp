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

#include "VolumeBase.h"
#include <volume_manager/ResponseCode.h>
#include <volume_manager/VolumeManager.h>
#include "Utils.h"

#include <android-base/logging.h>
#include <android-base/stringprintf.h>

#include <fcntl.h>
#include <stdlib.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>

using android::base::StringPrintf;

namespace android {
namespace volmgr {

VolumeBase::VolumeBase(Type type)
    : mType(type), mMountFlags(0), mCreated(false), mState(State::kUnmounted), mSilent(false) {}

VolumeBase::~VolumeBase() {
    CHECK(!mCreated);
}

void VolumeBase::setState(State state) {
    mState = state;
    VolumeManager::Instance()->notifyEvent(ResponseCode::VolumeStateChanged,
                                           StringPrintf("%d", mState));
}

status_t VolumeBase::setDiskId(const std::string& diskId) {
    if (mCreated) {
        LOG(WARNING) << getId() << " diskId change requires destroyed";
        return -EBUSY;
    }

    mDiskId = diskId;
    return OK;
}

status_t VolumeBase::setPartGuid(const std::string& partGuid) {
    if (mCreated) {
        LOG(WARNING) << getId() << " partGuid change requires destroyed";
        return -EBUSY;
    }

    mPartGuid = partGuid;
    return OK;
}

status_t VolumeBase::setPartLabel(const std::string& partLabel) {
    if ((mState != State::kUnmounted) && (mState != State::kUnmountable)) {
        LOG(WARNING) << getId() << " partLabel change requires state unmounted or unmountable";
        LOG(WARNING) << getId() << " state=" << (int)mState;
    }

    mPartLabel = partLabel;
    return OK;
}

status_t VolumeBase::setMountFlags(int mountFlags) {
    if ((mState != State::kUnmounted) && (mState != State::kUnmountable)) {
        LOG(WARNING) << getId() << " flags change requires state unmounted or unmountable";
        return -EBUSY;
    }

    mMountFlags = mountFlags;
    return OK;
}

status_t VolumeBase::setSilent(bool silent) {
    if (mCreated) {
        LOG(WARNING) << getId() << " silence change requires destroyed";
        return -EBUSY;
    }

    mSilent = silent;
    return OK;
}

status_t VolumeBase::setId(const std::string& id) {
    if (mCreated) {
        LOG(WARNING) << getId() << " id change requires not created";
        return -EBUSY;
    }

    mId = id;
    return OK;
}

status_t VolumeBase::setPath(const std::string& path) {
    mPath = path;
    VolumeManager::Instance()->notifyEvent(ResponseCode::VolumePathChanged, mPath);
    return OK;
}

status_t VolumeBase::create() {
    if (mCreated) {
        return BAD_VALUE;
    }

    mCreated = true;
    std::vector<std::string> argv;
    argv.push_back(StringPrintf("%d", mType));
    argv.push_back(mDiskId);
    argv.push_back(mPartGuid);
    status_t res = doCreate();
    if (res == OK) {
        VolumeManager::Instance()->notifyEvent(ResponseCode::VolumeCreated, argv);
        if (mPartLabel.size() > 0) {
            VolumeManager::Instance()->notifyEvent(ResponseCode::VolumeFsLabelChanged, mPartLabel);
        }
    }
    setState(State::kUnmounted);
    return res;
}

status_t VolumeBase::doCreate() {
    return OK;
}

status_t VolumeBase::destroy() {
    if (!mCreated) {
        return NO_INIT;
    }

    if (mState == State::kMounted) {
        unmount(true /* detatch */);
        setState(State::kBadRemoval);
    } else {
        setState(State::kRemoved);
    }

    VolumeManager::Instance()->notifyEvent(ResponseCode::VolumeDestroyed);
    status_t res = doDestroy();
    mCreated = false;
    return res;
}

status_t VolumeBase::doDestroy() {
    return OK;
}

status_t VolumeBase::mount() {
    if ((mState != State::kUnmounted) && (mState != State::kUnmountable)) {
        LOG(WARNING) << getId() << " mount requires state unmounted or unmountable";
        return -EBUSY;
    }

    setState(State::kChecking);
    status_t res = doMount();
    if (res == OK) {
        setState(State::kMounted);
    } else {
        setState(State::kUnmountable);
    }

    return res;
}

status_t VolumeBase::unmount(bool detach /* = false */) {
    setState(State::kEjecting);

    status_t res = doUnmount(detach);
    setState(State::kUnmounted);
    return res;
}

}  // namespace volmgr
}  // namespace android
