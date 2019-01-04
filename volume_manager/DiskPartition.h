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

#ifndef ANDROID_VOLMGR_DISKPARTITION_H
#define ANDROID_VOLMGR_DISKPARTITION_H

#include "Disk.h"

namespace android {
namespace volmgr {

/*
 * Representation of a single partition on physical media.  Useful for
 * single media partitions such as "internal" sdcard partitions.
 */

class DiskPartition : public Disk {
  public:
    DiskPartition(const std::string& eventPath, dev_t device, const std::string& nickname, int flags,
                  int partnum, const std::string& fstype = "", const std::string& mntopts = "");
    virtual ~DiskPartition();

    virtual status_t create();
    virtual status_t destroy();

  private:
    /* Partition number */
    int mPartNum;
    /* Filesystem type */
    std::string mFsType;
    /* Mount options */
    std::string mMntOpts;
};

}  // namespace volmgr
}  // namespace android

#endif
