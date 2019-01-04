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

#ifndef VOLMGR_EXFAT_H
#define VOLMGR_EXFAT_H

#include <utils/Errors.h>

#include <string>

namespace android {
namespace volmgr {
namespace exfat {

status_t Mount(const std::string& source, const std::string& target, int ownerUid, int ownerGid,
               int permMask);

}  // namespace exfat
}  // namespace volmgr
}  // namespace android

#endif
