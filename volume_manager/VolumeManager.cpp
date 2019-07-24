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

#include <blkid/blkid.h>
#include <dirent.h>
#include <fcntl.h>
#include <fs_mgr.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>

#define LOG_TAG "VolumeManager"

#include <android-base/logging.h>
#include <cutils/properties.h>

#include <sysutils/NetlinkEvent.h>

#include <volume_manager/VolumeManager.h>
#include <fstab/fstab.h>
#include "Disk.h"
#include "DiskPartition.h"
#include "EmulatedVolume.h"
#include "VolumeBase.h"
#include "NetlinkManager.h"

#include "sehandle.h"

struct selabel_handle* sehandle;

using android::fs_mgr::Fstab;
using android::fs_mgr::FstabEntry;

static const unsigned int kMajorBlockMmc = 179;
static const unsigned int kMajorBlockExperimentalMin = 240;
static const unsigned int kMajorBlockExperimentalMax = 254;

namespace android {
namespace volmgr {

static void do_coldboot(DIR* d, int lvl) {
    struct dirent* de;
    int dfd, fd;

    dfd = dirfd(d);

    fd = openat(dfd, "uevent", O_WRONLY | O_CLOEXEC);
    if (fd >= 0) {
        write(fd, "add\n", 4);
        close(fd);
    }

    while ((de = readdir(d))) {
        DIR* d2;

        if (de->d_name[0] == '.') continue;

        if (de->d_type != DT_DIR && lvl > 0) continue;

        fd = openat(dfd, de->d_name, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (fd < 0) continue;

        d2 = fdopendir(fd);
        if (d2 == 0)
            close(fd);
        else {
            do_coldboot(d2, lvl + 1);
            closedir(d2);
        }
    }
}

static void coldboot(const char* path) {
    DIR* d = opendir(path);
    if (d) {
        do_coldboot(d, 0);
        closedir(d);
    }
}

static int process_config(VolumeManager* vm, FstabEntry* data_recp) {
    Fstab fstab;
    if (!ReadDefaultFstab(&fstab)) {
        PLOG(ERROR) << "Failed to open default fstab";
        return -1;
    }

    /* Loop through entries looking for ones that vold manages */
    for (int i = 0; i < fstab.size(); i++) {
        if (fstab[i].fs_mgr_flags.vold_managed) {
            std::string sysPattern(fstab[i].blk_device);
            std::string fstype = fstab[i].fs_type;
            std::string mntopts = fstab[i].fs_options;
            std::string nickname = fstab[i].label;
            int partnum = fstab[i].partnum;
            int flags = 0;

            if (fstab[i].is_encryptable()) {
                flags |= android::volmgr::Disk::Flags::kAdoptable;
            }
            if (fstab[i].fs_mgr_flags.no_emulated_sd ||
                property_get_bool("vold.debug.default_primary", false)) {
                flags |= android::volmgr::Disk::Flags::kDefaultPrimary;
            }
            if (fstab[i].fs_mgr_flags.nonremovable) {
                flags |= android::volmgr::Disk::Flags::kNonRemovable;
            }

            vm->addDiskSource(new VolumeManager::DiskSource(sysPattern, nickname, partnum, flags,
                                                            fstype, mntopts));
        } else {
            if (data_recp->fs_type.empty() && fstab[i].mount_point == "/data") {
                char* detected_fs_type =
                    blkid_get_tag_value(nullptr, "TYPE", fstab[i].blk_device.c_str());
                if (!detected_fs_type || fstab[i].fs_type == detected_fs_type) {
                    *data_recp = fstab[i];
                }
            }
        }
    }
    return 0;
}

VolumeInfo::VolumeInfo(const VolumeBase* vol)
    : mId(vol->getId()), mLabel(vol->getPartLabel()), mPath(vol->getPath()), mMountable(vol->isMountable()) {
    // Empty
}

VolumeManager* VolumeManager::sInstance = nullptr;

VolumeManager* VolumeManager::Instance(void) {
    if (!sInstance) {
        sInstance = new VolumeManager();
    }
    return sInstance;
}

VolumeManager::VolumeManager(void)
    : mWatcher(nullptr), mNetlinkManager(NetlinkManager::Instance()), mInternalEmulated(nullptr) {
    // Empty
}

VolumeManager::~VolumeManager(void) {
    stop();
}

bool VolumeManager::start(VolumeWatcher* watcher) {
    setenv("BLKID_FILE", "/tmp/vold_blkid.tab", 1);

    sehandle = selinux_android_file_context_handle();
    if (sehandle) {
        selinux_android_set_sehandle(sehandle);
    }

    mkdir("/dev/block/volmgr", 0755);

    mWatcher = watcher;

    FstabEntry data_rec;
    if (process_config(this, &data_rec) != 0) {
        LOG(ERROR) << "Error reading configuration... continuing anyway";
    }

    if (!data_rec.fs_type.empty()) {
        mInternalEmulated = new EmulatedVolume(&data_rec, "media/0");
        mInternalEmulated->create();
    }

    if (!mNetlinkManager->start()) {
        LOG(ERROR) << "Unable to start NetlinkManager";
        return false;
    }

    coldboot("/sys/block");

    unmountAll();

    LOG(INFO) << "VolumeManager initialized";
    return true;
}

void VolumeManager::stop(void) {
    for (auto& disk : mDisks) {
        disk->destroy();
        delete disk;
    }
    mDisks.clear();
    for (auto& source : mDiskSources) {
        delete source;
    }
    mDiskSources.clear();

    mNetlinkManager->stop();
    mWatcher = nullptr;
}

bool VolumeManager::reset(void) {
    return false;
}

bool VolumeManager::unmountAll(void) {
    std::lock_guard<std::mutex> lock(mLock);

    if (mInternalEmulated) {
        mInternalEmulated->unmount();
    }

    for (auto& disk : mDisks) {
        disk->unmountAll();
    }

    return true;
}

void VolumeManager::getVolumeInfo(std::vector<VolumeInfo>& info) {
    std::lock_guard<std::mutex> lock(mLock);

    info.clear();
    if (mInternalEmulated) {
        info.push_back(VolumeInfo(mInternalEmulated));
    }
    for (const auto& disk : mDisks) {
        disk->getVolumeInfo(info);
    }
}

VolumeBase* VolumeManager::findVolume(const std::string& id) {
    if (mInternalEmulated && mInternalEmulated->getId() == id) {
        return mInternalEmulated;
    }
    for (const auto& disk : mDisks) {
        auto vol = disk->findVolume(id);
        if (vol != nullptr) {
            return vol.get();
        }
    }
    return nullptr;
}

bool VolumeManager::volumeMount(const std::string& id) {
    std::lock_guard<std::mutex> lock(mLock);
    auto vol = findVolume(id);
    if (!vol) {
        return false;
    }
    status_t res = vol->mount();
    return (res == OK);
}

bool VolumeManager::volumeUnmount(const std::string& id, bool detach /* = false */) {
    std::lock_guard<std::mutex> lock(mLock);
    auto vol = findVolume(id);
    if (!vol) {
        return false;
    }
    status_t res = vol->unmount(detach);
    return (res == OK);
}

void VolumeManager::addDiskSource(DiskSource* source) {
    std::lock_guard<std::mutex> lock(mLock);

    mDiskSources.push_back(source);
}

void VolumeManager::handleBlockEvent(NetlinkEvent* evt) {
    std::lock_guard<std::mutex> lock(mLock);

    const char* param;
    param = evt->findParam("DEVTYPE");
    std::string devType(param ? param : "");
    if (devType != "disk") {
        return;
    }
    param = evt->findParam("DEVPATH");
    std::string eventPath(param ? param : "");

    int major = atoi(evt->findParam("MAJOR"));
    int minor = atoi(evt->findParam("MINOR"));
    dev_t device = makedev(major, minor);

    switch (evt->getAction()) {
        case NetlinkEvent::Action::kAdd: {
            for (const auto& source : mDiskSources) {
                if (source->matches(eventPath)) {
                    // For now, assume that MMC, virtio-blk (the latter is
                    // emulator-specific; see Disk.cpp for details) and UFS card
                    // devices are SD, and that everything else is USB
                    int flags = source->getFlags();
                    if (major == kMajorBlockMmc || (eventPath.find("ufs") != std::string::npos) ||
                        (IsRunningInEmulator() && major >= (int)kMajorBlockExperimentalMin &&
                         major <= (int)kMajorBlockExperimentalMax)) {
                        flags |= Disk::Flags::kSd;
                    } else {
                        flags |= Disk::Flags::kUsb;
                    }

                    Disk* disk = (source->getPartNum() == -1)
                                     ? new Disk(eventPath, device, source->getNickname(), flags)
                                     : new DiskPartition(eventPath, device, source->getNickname(),
                                                         flags, source->getPartNum(),
                                                         source->getFsType(), source->getMntOpts());
                    disk->create();
                    mDisks.push_back(disk);
                    break;
                }
            }
            break;
        }
        case NetlinkEvent::Action::kChange: {
            LOG(DEBUG) << "Disk at " << major << ":" << minor << " changed";
            for (const auto& disk : mDisks) {
                if (disk->getDevice() == device) {
                    disk->readMetadata();
                    disk->readPartitions();
                }
            }
            break;
        }
        case NetlinkEvent::Action::kRemove: {
            auto i = mDisks.begin();
            while (i != mDisks.end()) {
                if ((*i)->getDevice() == device) {
                    (*i)->destroy();
                    i = mDisks.erase(i);
                } else {
                    ++i;
                }
            }
            break;
        }
        default: {
            LOG(WARNING) << "Unexpected block event action " << (int)evt->getAction();
            break;
        }
    }
}

void VolumeManager::notifyEvent(int code) {
    std::vector<std::string> argv;
    notifyEvent(code, argv);
}

void VolumeManager::notifyEvent(int code, const std::string& arg) {
    std::vector<std::string> argv;
    argv.push_back(arg);
    notifyEvent(code, argv);
}

void VolumeManager::notifyEvent(int code, const std::vector<std::string>& argv) {
    mWatcher->handleEvent(code, argv);
}

}  // namespace volmgr
}  // namespace android
