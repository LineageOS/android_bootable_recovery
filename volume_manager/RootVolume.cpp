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

#include "RootVolume.h"
#include <volume_manager/VolumeManager.h>
#include "ResponseCode.h"

namespace android {
namespace volmgr {

RootVolume::RootVolume()
    : VolumeBase(Type::kEmulated) {
    setId("root");
    setPartLabel("root");
    setPath("/");
}

RootVolume::~RootVolume() {}

status_t RootVolume::doMount() {
    return OK;
}

status_t RootVolume::doUnmount(__unused bool detach /* = false */) {
    return OK;
}

bool RootVolume::detectMountable() {
    return true;
}

}  // namespace volmgr
}  // namespace android
