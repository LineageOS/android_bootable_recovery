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

#include "Disk.h"
#include "PublicVolume.h"
#include <volume_manager/ResponseCode.h>
#include <volume_manager/VolumeManager.h>
#include "Utils.h"
#include "VolumeBase.h"

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android-base/stringprintf.h>

#include <sgdisk.h>

#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <vector>

using android::base::ReadFileToString;
using android::base::WriteStringToFile;
using android::base::StringPrintf;

namespace android {
namespace volmgr {

static const char* kSysfsLoopMaxMinors = "/sys/module/loop/parameters/max_part";
static const char* kSysfsMmcMaxMinorsDeprecated = "/sys/module/mmcblk/parameters/perdev_minors";
static const char* kSysfsMmcMaxMinors = "/sys/module/mmc_block/parameters/perdev_minors";

static const unsigned int kMajorBlockLoop = 7;
static const unsigned int kMajorBlockScsiA = 8;
static const unsigned int kMajorBlockScsiB = 65;
static const unsigned int kMajorBlockScsiC = 66;
static const unsigned int kMajorBlockScsiD = 67;
static const unsigned int kMajorBlockScsiE = 68;
static const unsigned int kMajorBlockScsiF = 69;
static const unsigned int kMajorBlockScsiG = 70;
static const unsigned int kMajorBlockScsiH = 71;
static const unsigned int kMajorBlockScsiI = 128;
static const unsigned int kMajorBlockScsiJ = 129;
static const unsigned int kMajorBlockScsiK = 130;
static const unsigned int kMajorBlockScsiL = 131;
static const unsigned int kMajorBlockScsiM = 132;
static const unsigned int kMajorBlockScsiN = 133;
static const unsigned int kMajorBlockScsiO = 134;
static const unsigned int kMajorBlockScsiP = 135;
static const unsigned int kMajorBlockMmc = 179;
static const unsigned int kMajorBlockExperimentalMin = 240;
static const unsigned int kMajorBlockExperimentalMax = 254;

static const char* kGptBasicData = "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7";
static const char* kGptLinuxFilesystem = "0FC63DAF-8483-4772-8E79-3D69D8477DE4";

enum class Table {
    kUnknown,
    kMbr,
    kGpt,
};

static bool isVirtioBlkDevice(unsigned int major) {
    /*
     * The new emulator's "ranchu" virtual board no longer includes a goldfish
     * MMC-based SD card device; instead, it emulates SD cards with virtio-blk,
     * which has been supported by upstream kernel and QEMU for quite a while.
     * Unfortunately, the virtio-blk block device driver does not use a fixed
     * major number, but relies on the kernel to assign one from a specific
     * range of block majors, which are allocated for "LOCAL/EXPERIMENAL USE"
     * per Documentation/devices.txt. This is true even for the latest Linux
     * kernel (4.4; see init() in drivers/block/virtio_blk.c).
     *
     * This makes it difficult for vold to detect a virtio-blk based SD card.
     * The current solution checks two conditions (both must be met):
     *
     *  a) If the running environment is the emulator;
     *  b) If the major number is an experimental block device major number (for
     *     x86/x86_64 3.10 ranchu kernels, virtio-blk always gets major number
     *     253, but it is safer to match the range than just one value).
     *
     * Other conditions could be used, too, e.g. the hardware name should be
     * "ranchu", the device's sysfs path should end with "/block/vd[d-z]", etc.
     * But just having a) and b) is enough for now.
     */
    return IsRunningInEmulator() && major >= kMajorBlockExperimentalMin &&
           major <= kMajorBlockExperimentalMax;
}

Disk::Disk(const std::string& eventPath, dev_t device, const std::string& nickname, int flags)
    : mDevice(device),
      mSize(-1),
      mNickname(nickname),
      mFlags(flags),
      mCreated(false),
      mSkipChange(false) {
    mId = StringPrintf("disk:%u_%u", major(device), minor(device));
    mEventPath = eventPath;
    mSysPath = StringPrintf("/sys/%s", eventPath.c_str());
    mDevPath = StringPrintf("/dev/block/volmgr/%s", mId.c_str());
    CreateDeviceNode(mDevPath, mDevice);
}

Disk::~Disk() {
    CHECK(!mCreated);
    DestroyDeviceNode(mDevPath);
}

void Disk::getVolumeInfo(std::vector<VolumeInfo>& info) {
    for (auto vol : mVolumes) {
        info.push_back(VolumeInfo(vol.get()));
    }
}

std::shared_ptr<VolumeBase> Disk::findVolume(const std::string& id) {
    for (auto vol : mVolumes) {
        if (vol->getId() == id) {
            return vol;
        }
    }
    return nullptr;
}

void Disk::listVolumes(VolumeBase::Type type, std::list<std::string>& list) {
    for (const auto& vol : mVolumes) {
        if (vol->getType() == type) {
            list.push_back(vol->getId());
        }
        // TODO: consider looking at stacked volumes
    }
}

status_t Disk::create() {
    CHECK(!mCreated);
    mCreated = true;
    VolumeManager::Instance()->notifyEvent(ResponseCode::DiskCreated, StringPrintf("%d", mFlags));
    readMetadata();
    readPartitions();
    return OK;
}

status_t Disk::destroy() {
    CHECK(mCreated);
    destroyAllVolumes();
    mCreated = false;
    VolumeManager::Instance()->notifyEvent(ResponseCode::DiskDestroyed);
    return OK;
}

void Disk::createPublicVolume(dev_t device, const std::string& fstype /* = "" */,
                              const std::string& mntopts /* = "" */) {
    auto vol = std::shared_ptr<VolumeBase>(new PublicVolume(device, mNickname, fstype, mntopts));

    mVolumes.push_back(vol);
    vol->setDiskId(getId());
    vol->create();
}

void Disk::destroyAllVolumes() {
    for (const auto& vol : mVolumes) {
        vol->destroy();
    }
    mVolumes.clear();
}

status_t Disk::readMetadata() {
    if (mSkipChange) {
        return OK;
    }

    mSize = -1;
    mLabel.clear();

    int fd = open(mDevPath.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd != -1) {
        if (ioctl(fd, BLKGETSIZE64, &mSize)) {
            mSize = -1;
        }
        close(fd);
    }

    unsigned int majorId = major(mDevice);
    switch (majorId) {
        case kMajorBlockLoop: {
            mLabel = "Virtual";
            break;
        }
        case kMajorBlockScsiA:
        case kMajorBlockScsiB:
        case kMajorBlockScsiC:
        case kMajorBlockScsiD:
        case kMajorBlockScsiE:
        case kMajorBlockScsiF:
        case kMajorBlockScsiG:
        case kMajorBlockScsiH:
        case kMajorBlockScsiI:
        case kMajorBlockScsiJ:
        case kMajorBlockScsiK:
        case kMajorBlockScsiL:
        case kMajorBlockScsiM:
        case kMajorBlockScsiN:
        case kMajorBlockScsiO:
        case kMajorBlockScsiP: {
            std::string path(mSysPath + "/device/vendor");
            std::string tmp;
            if (!ReadFileToString(path, &tmp)) {
                PLOG(WARNING) << "Failed to read vendor from " << path;
                return -errno;
            }
            mLabel = tmp;
            break;
        }
        case kMajorBlockMmc: {
            std::string path(mSysPath + "/device/manfid");
            std::string tmp;
            if (!ReadFileToString(path, &tmp)) {
                PLOG(WARNING) << "Failed to read manufacturer from " << path;
                return -errno;
            }
            uint64_t manfid = strtoll(tmp.c_str(), nullptr, 16);
            // Our goal here is to give the user a meaningful label, ideally
            // matching whatever is silk-screened on the card.  To reduce
            // user confusion, this list doesn't contain white-label manfid.
            switch (manfid) {
                case 0x000003:
                    mLabel = "SanDisk";
                    break;
                case 0x00001b:
                    mLabel = "Samsung";
                    break;
                case 0x000028:
                    mLabel = "Lexar";
                    break;
                case 0x000074:
                    mLabel = "Transcend";
                    break;
            }
            break;
        }
        default: {
            if (isVirtioBlkDevice(majorId)) {
                LOG(DEBUG) << "Recognized experimental block major ID " << majorId
                           << " as virtio-blk (emulator's virtual SD card device)";
                mLabel = "Virtual";
                break;
            }
            LOG(WARNING) << "Unsupported block major type " << majorId;
            return -ENOTSUP;
        }
    }

    VolumeManager::Instance()->notifyEvent(ResponseCode::DiskSizeChanged,
                                           StringPrintf("%" PRIu64, mSize));
    VolumeManager::Instance()->notifyEvent(ResponseCode::DiskLabelChanged, mLabel);
    VolumeManager::Instance()->notifyEvent(ResponseCode::DiskSysPathChanged, mSysPath);
    return OK;
}

status_t Disk::readPartitions() {
    int8_t maxMinors = getMaxMinors();
    if (maxMinors < 0) {
        return -ENOTSUP;
    }

    if (mSkipChange) {
        mSkipChange = false;
        LOG(INFO) << "Skip first change";
        return OK;
    }

    destroyAllVolumes();

    // Parse partition table
    sgdisk_partition_table ptbl;
    std::vector<sgdisk_partition> partitions;
    int res = sgdisk_read(mDevPath.c_str(), ptbl, partitions);
    if (res != 0) {
        LOG(WARNING) << "sgdisk failed to scan " << mDevPath;
        VolumeManager::Instance()->notifyEvent(ResponseCode::DiskScanned);
        return res;
    }

    Table table = Table::kUnknown;
    bool foundParts = false;

    switch (ptbl.type) {
        case MBR:
            table = Table::kMbr;
            break;
        case GPT:
            table = Table::kGpt;
            break;
        default:
            table = Table::kUnknown;
    }

    foundParts = partitions.size() > 0;
    for (const auto& part : partitions) {
        if (part.num <= 0 || part.num > maxMinors) {
            LOG(WARNING) << mId << " is ignoring partition " << part.num
                         << " beyond max supported devices";
            continue;
        }
        dev_t partDevice = makedev(major(mDevice), minor(mDevice) + part.num);
        if (table == Table::kMbr) {
            switch (strtol(part.type.c_str(), nullptr, 16)) {
                case 0x06:  // FAT16
                case 0x07:  // NTFS/exFAT
                case 0x0b:  // W95 FAT32 (LBA)
                case 0x0c:  // W95 FAT32 (LBA)
                case 0x0e:  // W95 FAT16 (LBA)
                case 0x83:  // Linux EXT4/F2FS/...
                    createPublicVolume(partDevice);
                    break;
            }
        } else if (table == Table::kGpt) {
            if (!strcasecmp(part.guid.c_str(), kGptBasicData) ||
                !strcasecmp(part.guid.c_str(), kGptLinuxFilesystem)) {
                createPublicVolume(partDevice);
            }
        }
    }

    // Ugly last ditch effort, treat entire disk as partition
    if (table == Table::kUnknown || !foundParts) {
        LOG(WARNING) << mId << " has unknown partition table; trying entire device";

        std::string fsType;
        std::string unused;
        if (ReadMetadataUntrusted(mDevPath, fsType, unused, unused) == OK) {
            createPublicVolume(mDevice);
        } else {
            LOG(WARNING) << mId << " failed to identify, giving up";
        }
    }

    VolumeManager::Instance()->notifyEvent(ResponseCode::DiskScanned);
    return OK;
}

status_t Disk::unmountAll() {
    for (const auto& vol : mVolumes) {
        vol->unmount();
    }
    return OK;
}

int Disk::getMaxMinors() {
    // Figure out maximum partition devices supported
    unsigned int majorId = major(mDevice);
    switch (majorId) {
        case kMajorBlockLoop: {
            std::string tmp;
            if (!ReadFileToString(kSysfsLoopMaxMinors, &tmp)) {
                LOG(ERROR) << "Failed to read max minors";
                return -errno;
            }
            return atoi(tmp.c_str());
        }
        case kMajorBlockScsiA:
        case kMajorBlockScsiB:
        case kMajorBlockScsiC:
        case kMajorBlockScsiD:
        case kMajorBlockScsiE:
        case kMajorBlockScsiF:
        case kMajorBlockScsiG:
        case kMajorBlockScsiH:
        case kMajorBlockScsiI:
        case kMajorBlockScsiJ:
        case kMajorBlockScsiK:
        case kMajorBlockScsiL:
        case kMajorBlockScsiM:
        case kMajorBlockScsiN:
        case kMajorBlockScsiO:
        case kMajorBlockScsiP: {
            // Per Documentation/devices.txt this is static
            return 15;
        }
        case kMajorBlockMmc: {
            // Per Documentation/devices.txt this is dynamic
            std::string tmp;
            if (!ReadFileToString(kSysfsMmcMaxMinors, &tmp) &&
                !ReadFileToString(kSysfsMmcMaxMinorsDeprecated, &tmp)) {
                LOG(ERROR) << "Failed to read max minors";
                return -errno;
            }
            return atoi(tmp.c_str());
        }
        default: {
            if (isVirtioBlkDevice(majorId)) {
                // drivers/block/virtio_blk.c has "#define PART_BITS 4", so max is
                // 2^4 - 1 = 15
                return 15;
            }
        }
    }

    LOG(ERROR) << "Unsupported block major type " << majorId;
    return -ENOTSUP;
}

}  // namespace volmgr
}  // namespace android
