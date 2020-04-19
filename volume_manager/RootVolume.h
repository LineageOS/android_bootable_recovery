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

#ifndef ANDROID_VOLMGR_ROOT_VOLUME_H
#define ANDROID_VOLMGR_ROOT_VOLUME_H

#include "VolumeBase.h"

namespace android {
namespace volmgr {

class RootVolume : public VolumeBase {
  public:
    explicit RootVolume();
    virtual ~RootVolume();

  protected:
    status_t doMount() override;
    status_t doUnmount(bool detach = false) override;

  private:

    bool detectMountable() override;

    DISALLOW_COPY_AND_ASSIGN(RootVolume);
};

}  // namespace volmgr
}  // namespace android

#endif
