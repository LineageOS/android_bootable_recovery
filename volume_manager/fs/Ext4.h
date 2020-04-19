/*
 * Copyright (C) 2012 The Android Open Source Project
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

#ifndef VOLMGR_EXT4_H
#define VOLMGR_EXT4_H

#include <utils/Errors.h>

#include <string>

namespace android {
namespace volmgr {
namespace ext4 {

status_t Check(const std::string& source, const std::string& target, bool trusted);
status_t Mount(const std::string& source, const std::string& target, bool ro, bool remount,
               bool executable, const std::string& opts = "", bool trusted = false,
               bool portable = false);

}  // namespace ext4
}  // namespace volmgr
}  // namespace android

#endif
