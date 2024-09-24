/*
 * Copyright (C) 2007 The Android Open Source Project
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

#include "recovery_utils/roots.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <sys/mount.h>

#include <iostream>
#include <string>
#include <vector>

#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android-base/stringprintf.h>
#include <android-base/unique_fd.h>
#include <blkid/blkid.h>
#include <ext4_utils/ext4_utils.h>
#include <ext4_utils/wipe.h>
#include <fs_mgr.h>
#include <fs_mgr/roots.h>
#include <fs_mgr_dm_linear.h>

#include "otautil/sysutil.h"

using android::fs_mgr::Fstab;
using android::fs_mgr::FstabEntry;
using android::fs_mgr::ReadDefaultFstab;
using android::dm::DeviceMapper;
using android::dm::DmDeviceState;

static void write_fstab_entry(const FstabEntry& entry, FILE* file) {
  if (entry.fs_type != "emmc" && !entry.fs_mgr_flags.vold_managed && !entry.blk_device.empty() &&
      entry.blk_device[0] == '/' && !entry.mount_point.empty() && entry.mount_point[0] == '/') {
    fprintf(file, "%s ", entry.blk_device.c_str());
    fprintf(file, "%s ", entry.mount_point.c_str());
    fprintf(file, "%s ", entry.fs_type.c_str());
    fprintf(file, "%s 0 0\n", !entry.fs_options.empty() ? entry.fs_options.c_str() : "defaults");
  }
}

static Fstab fstab;

constexpr const char* CACHE_ROOT = "/cache";

FstabEntry* fstab_entry_for_mount_point_detect_fs(const std::string& path) {
  FstabEntry* found = android::fs_mgr::GetEntryForMountPoint(&fstab, path);
  if (found == nullptr) {
    return nullptr;
  }

  if (char* detected_fs_type = blkid_get_tag_value(nullptr, "TYPE", found->blk_device.c_str())) {
    for (auto& entry : fstab) {
      if (entry.mount_point == path && entry.fs_type == detected_fs_type) {
        found = &entry;
        break;
      }
    }
    free(detected_fs_type);
  }

  return found;
}

void load_volume_table() {
  if (!ReadDefaultFstab(&fstab)) {
    LOG(ERROR) << "Failed to read default fstab";
    return;
  }

  fstab.emplace_back(FstabEntry{
      .blk_device = "ramdisk",
      .mount_point = "/tmp",
      .fs_type = "ramdisk",
      .length = 0,
  });

  Fstab fake_fstab;
  std::cout << "recovery filesystem table" << std::endl << "=========================" << std::endl;
  for (size_t i = 0; i < fstab.size(); ++i) {
    const auto& entry = fstab[i];
    std::cout << "  " << i << " " << entry.mount_point << " "
              << " " << entry.fs_type << " " << entry.blk_device << " " << entry.length
              << std::endl;

    if (std::find_if(fake_fstab.begin(), fake_fstab.end(), [entry](const FstabEntry& e) {
          return entry.mount_point == e.mount_point;
        }) == fake_fstab.end()) {
      FstabEntry* entry_detectfs = fstab_entry_for_mount_point_detect_fs(entry.mount_point);
      if (entry_detectfs == &entry) {
        fake_fstab.emplace_back(entry);
      }
    }
  }
  std::cout << std::endl;

  // Create a boring /etc/fstab so tools like Busybox work
  FILE* file = fopen("/etc/fstab", "w");
  if (file) {
    for (auto& entry : fake_fstab) {
      write_fstab_entry(entry, file);
    }
    fclose(file);
  } else {
    LOG(ERROR) << "Unable to create /etc/fstab";
  }
}

Volume* volume_for_mount_point(const std::string& mount_point) {
  return android::fs_mgr::GetEntryForMountPoint(&fstab, mount_point);
}

// Mount the volume specified by path at the given mount_point.
int ensure_path_mounted_at(const std::string& path, const std::string& mount_point) {
  return android::fs_mgr::EnsurePathMounted(&fstab, path, mount_point) ? 0 : -1;
}

int ensure_path_mounted(const std::string& path) {
  // Mount at the default mount point.
  return android::fs_mgr::EnsurePathMounted(&fstab, path) ? 0 : -1;
}

int ensure_path_unmounted(const std::string& path) {
  return android::fs_mgr::EnsurePathUnmounted(&fstab, path) ? 0 : -1;
}

int ensure_volume_unmounted(const std::string& blk_device) {
  android::fs_mgr::Fstab mounted_fstab;
  if (!android::fs_mgr::ReadFstabFromFile("/proc/mounts", &mounted_fstab)) {
    LOG(ERROR) << "Failed to read /proc/mounts";
    return -1;
  }

  /* find any entries with the volume */
  for (auto& entry : mounted_fstab) {
    if (entry.blk_device == blk_device) {
      int result = umount(entry.mount_point.c_str());
      if (result == -1) {
        LOG(ERROR) << "Failed to unmount " << blk_device << " from " << entry.mount_point << ": "
                   << errno;
        return -1;
      }
    }
  }
  return 0;
}

static int exec_cmd(const std::vector<std::string>& args) {
  CHECK(!args.empty());
  auto argv = StringVectorToNullTerminatedArray(args);

  pid_t child;
  if ((child = fork()) == 0) {
    execv(argv[0], argv.data());
    _exit(EXIT_FAILURE);
  }

  int status;
  waitpid(child, &status, 0);
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    LOG(ERROR) << args[0] << " failed with status " << WEXITSTATUS(status);
  }
  return WEXITSTATUS(status);
}

static int64_t get_file_size(int fd, uint64_t reserve_len) {
  struct stat buf;
  int ret = fstat(fd, &buf);
  if (ret) return 0;

  int64_t computed_size;
  if (S_ISREG(buf.st_mode)) {
    computed_size = buf.st_size - reserve_len;
  } else if (S_ISBLK(buf.st_mode)) {
    uint64_t block_device_size = get_block_device_size(fd);
    if (block_device_size < reserve_len ||
        block_device_size > std::numeric_limits<int64_t>::max()) {
      computed_size = 0;
    } else {
      computed_size = block_device_size - reserve_len;
    }
  } else {
    computed_size = 0;
  }

  return computed_size;
}

int format_volume(const std::string& volume, const std::string& directory,
                  std::string_view new_fstype) {
  const FstabEntry* v = android::fs_mgr::GetEntryForPath(&fstab, volume);
  if (v == nullptr) {
    LOG(ERROR) << "unknown volume \"" << volume << "\"";
    return -1;
  }
  if (v->fs_type == "ramdisk") {
    LOG(ERROR) << "can't format_volume \"" << volume << "\"";
    return -1;
  }
  if (v->mount_point != volume) {
    LOG(ERROR) << "can't give path \"" << volume << "\" to format_volume";
    return -1;
  }
  if (ensure_volume_unmounted(v->blk_device) != 0) {
    LOG(ERROR) << "format_volume: Failed to unmount \"" << v->mount_point << "\"";
    return -1;
  }
  if (v->fs_type != "ext4" && v->fs_type != "f2fs") {
    LOG(ERROR) << "format_volume: fs_type \"" << v->fs_type << "\" unsupported";
    return -1;
  }

  bool needs_casefold = false;

  if (volume == "/data") {
    needs_casefold = android::base::GetBoolProperty("external_storage.casefold.enabled", false);
  }

  int64_t length = 0;
  if (v->length > 0) {
    length = v->length;
  } else if (v->length < 0) {
    android::base::unique_fd fd(open(v->blk_device.c_str(), O_RDONLY));
    if (fd == -1) {
      PLOG(ERROR) << "format_volume: failed to open " << v->blk_device;
      return -1;
    }
    length = get_file_size(fd.get(), -v->length);
    if (length <= 0) {
      LOG(ERROR) << "get_file_size: invalid size " << length << " for " << v->blk_device;
      return -1;
    }
  }

  // If the raw disk will be used as a metadata encrypted device mapper target,
  // next boot will do encrypt_in_place the raw disk. While fs_mgr mounts /data
  // as RO to avoid write file operations before encrypt_inplace, this code path
  // is not well tested so we would like to avoid it if possible. For safety,
  // let vold do the formatting on boot for metadata encrypted devices, except
  // when user specified a new fstype. Because init formats /data according
  // to fstab, it's difficult to override the fstab in init.
  if (!v->metadata_key_dir.empty() && length == 0 && new_fstype.empty()) {
    android::base::unique_fd fd(open(v->blk_device.c_str(), O_RDWR));
    if (fd == -1) {
      PLOG(ERROR) << "format_volume: failed to open " << v->blk_device;
      return -1;
    }
    int64_t device_size = get_file_size(fd.get(), 0);
    if (device_size > 0 && !wipe_block_device(fd.get(), device_size)) {
      LOG(INFO) << "format_volume: wipe metadata encrypted " << v->blk_device << " with size "
                << device_size;
      return 0;
    }
  }

  if ((v->fs_type == "ext4" && new_fstype.empty()) || new_fstype == "ext4") {
    LOG(INFO) << "Formatting " << v->blk_device << " as ext4";
    static constexpr int kBlockSize = 4096;
    std::vector<std::string> mke2fs_args = {
      "/system/bin/mke2fs", "-F", "-t", "ext4", "-b", std::to_string(kBlockSize),
    };

    // Following is added for Project ID's quota as they require wider inodes.
    // The Quotas themselves are enabled by tune2fs on boot.
    mke2fs_args.push_back("-I");
    mke2fs_args.push_back("512");

    if (v->fs_mgr_flags.ext_meta_csum) {
      mke2fs_args.push_back("-O");
      mke2fs_args.push_back("metadata_csum");
      mke2fs_args.push_back("-O");
      mke2fs_args.push_back("64bit");
      mke2fs_args.push_back("-O");
      mke2fs_args.push_back("extent");
    }

    int raid_stride = v->logical_blk_size / kBlockSize;
    int raid_stripe_width = v->erase_blk_size / kBlockSize;
    // stride should be the max of 8KB and logical block size
    if (v->logical_blk_size != 0 && v->logical_blk_size < 8192) {
      raid_stride = 8192 / kBlockSize;
    }
    if (v->erase_blk_size != 0 && v->logical_blk_size != 0) {
      mke2fs_args.push_back("-E");
      mke2fs_args.push_back(
          android::base::StringPrintf("stride=%d,stripe-width=%d", raid_stride, raid_stripe_width));
    }
    mke2fs_args.push_back(v->blk_device);
    if (length != 0) {
      mke2fs_args.push_back(std::to_string(length / kBlockSize));
    }

    int result = exec_cmd(mke2fs_args);
    if (result == 0 && !directory.empty()) {
      std::vector<std::string> e2fsdroid_args = {
        "/system/bin/e2fsdroid", "-e", "-f", directory, "-a", volume, v->blk_device,
      };
      result = exec_cmd(e2fsdroid_args);
    }

    if (result != 0) {
      PLOG(ERROR) << "format_volume: Failed to make ext4 on " << v->blk_device;
      return -1;
    }
    return 0;
  }

  // Has to be f2fs because we checked earlier.
  LOG(INFO) << "Formatting " << v->blk_device << " as f2fs";
  static constexpr int kSectorSize = 4096;
  std::vector<std::string> make_f2fs_cmd = {
    "/system/bin/make_f2fs",
    "-g",
    "android",
  };

  make_f2fs_cmd.push_back("-O");
  make_f2fs_cmd.push_back("project_quota,extra_attr");

  if (needs_casefold) {
    make_f2fs_cmd.push_back("-O");
    make_f2fs_cmd.push_back("casefold");
    make_f2fs_cmd.push_back("-C");
    make_f2fs_cmd.push_back("utf8");
  }
  if (v->fs_mgr_flags.fs_compress) {
    make_f2fs_cmd.push_back("-O");
    make_f2fs_cmd.push_back("compression");
    make_f2fs_cmd.push_back("-O");
    make_f2fs_cmd.push_back("extra_attr");
  }
  make_f2fs_cmd.push_back(v->blk_device);
  if (length >= kSectorSize) {
    make_f2fs_cmd.push_back(std::to_string(length / kSectorSize));
  }

  if (exec_cmd(make_f2fs_cmd) != 0) {
    PLOG(ERROR) << "format_volume: Failed to make_f2fs on " << v->blk_device;
    return -1;
  }
  if (!directory.empty()) {
    std::vector<std::string> sload_f2fs_cmd = {
      "/system/bin/sload_f2fs", "-f", directory, "-t", volume, v->blk_device,
    };
    if (exec_cmd(sload_f2fs_cmd) != 0) {
      PLOG(ERROR) << "format_volume: Failed to sload_f2fs on " << v->blk_device;
      return -1;
    }
  }
  return 0;
}

int format_volume(const std::string& volume) {
  return format_volume(volume, "", "");
}

int setup_install_mounts() {
  if (fstab.empty()) {
    LOG(ERROR) << "can't set up install mounts: no fstab loaded";
    return -1;
  }
  for (const FstabEntry& entry : fstab) {
    // We don't want to do anything with "/".
    if (entry.mount_point == "/") {
      continue;
    }

    // We may load update package from virtiofs mount point.
    if (entry.mount_point == "/mnt/vendor/shared") {
      continue;
    }

    if (entry.mount_point == "/tmp" || entry.mount_point == "/cache") {
      if (ensure_path_mounted(entry.mount_point) != 0) {
        LOG(ERROR) << "Failed to mount " << entry.mount_point;
        return -1;
      }
    } else {
      if (ensure_path_unmounted(entry.mount_point) != 0) {
        LOG(ERROR) << "Failed to unmount " << entry.mount_point;
        return -1;
      }
    }
  }
  return 0;
}

bool HasCache() {
  CHECK(!fstab.empty());
  static bool has_cache = volume_for_mount_point(CACHE_ROOT) != nullptr;
  return has_cache;
}

static bool logical_partitions_auto_mapped = false;

void map_logical_partitions() {
  if (android::base::GetBoolProperty("ro.boot.dynamic_partitions", false) &&
      !logical_partitions_mapped()) {
    std::string super_name = fs_mgr_get_super_partition_name();
    if (!android::fs_mgr::CreateLogicalPartitions("/dev/block/by-name/" + super_name)) {
      LOG(ERROR) << "Failed to map logical partitions";
    } else {
      logical_partitions_auto_mapped = true;
    }
  }
}

bool dm_find_system() {
  auto rec = GetEntryForPath(&fstab, android::fs_mgr::GetSystemRoot());
  if (!rec->fs_mgr_flags.logical) {
    return false;
  }
  // If the fstab entry for system it's a path instead of a name, then it was already mapped
  if (rec->blk_device[0] != '/') {
    if (DeviceMapper::Instance().GetState(rec->blk_device) == DmDeviceState::INVALID) {
      return false;
    }
  }
  return true;
}

bool logical_partitions_mapped() {
  return android::fs_mgr::LogicalPartitionsMapped() || logical_partitions_auto_mapped ||
      dm_find_system();
}
