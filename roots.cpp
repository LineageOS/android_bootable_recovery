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

#include "roots.h"

#include <ctype.h>
#include <fcntl.h>
#include <dirent.h>
#include <stdlib.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android-base/stringprintf.h>
#include <android-base/unique_fd.h>
#include <cryptfs.h>
#include <ext4_utils/wipe.h>
#include <fs_mgr.h>

#include "common.h"
#include "mounts.h"

#include "voldclient.h"

#ifdef __bitwise
#undef __bitwise
#endif
#include <blkid/blkid.h>

static struct fstab* fstab = nullptr;

extern struct selabel_handle* sehandle;

static int mkdir_p(const char* path, mode_t mode)
{
  char dir[PATH_MAX];
  char* p;
  strcpy(dir, path);
  for (p = strchr(&dir[1], '/'); p != NULL; p = strchr(p+1, '/')) {
    *p = '\0';
    if (mkdir(dir, mode) != 0 && errno != EEXIST) {
      return -1;
    }
    *p = '/';
  }
  if (mkdir(dir, mode) != 0 && errno != EEXIST) {
    return -1;
  }
  return 0;
}

static void write_fstab_entry(const Volume *v, FILE *file)
{
  if (v &&
      strcmp(v->fs_type, "mtd") != 0 && strcmp(v->fs_type, "emmc") != 0 &&
      strcmp(v->fs_type, "bml") != 0 && !fs_mgr_is_voldmanaged(v) &&
      strncmp(v->blk_device, "/", 1) == 0 &&
      strncmp(v->mount_point, "/", 1) == 0) {
    fprintf(file, "%s ", v->blk_device);
    fprintf(file, "%s ", v->mount_point);
    fprintf(file, "%s ", v->fs_type);
    fprintf(file, "%s 0 0\n", v->fs_options == NULL ? "defaults" : v->fs_options);
  }
}

int get_num_volumes() {
  return fstab->num_entries;
}

Volume* get_device_volumes() {
  return fstab->recs;
}

void load_volume_table() {
  fstab = fs_mgr_read_fstab_default();
  if (!fstab) {
    LOG(ERROR) << "Failed to read default fstab";
    return;
  }

  int ret = fs_mgr_add_entry(fstab, "/tmp", "ramdisk", "ramdisk");
  if (ret == -1) {
    LOG(ERROR) << "Failed to add /tmp entry to fstab";
    fs_mgr_free_fstab(fstab);
    fstab = nullptr;
    return;
  }

  // Create a boring /etc/fstab so tools like Busybox work
  FILE *file = fopen("/etc/fstab", "w");
  if (!file) {
    LOG(ERROR) << "Unable to create /etc/fstab";
  }

  printf("recovery filesystem table\n");
  printf("=========================\n");
  for (int i = 0; i < fstab->num_entries; ++i) {
    const Volume* v = &fstab->recs[i];
    printf("  %d %s %s %s %lld\n", i, v->mount_point, v->fs_type, v->blk_device, v->length);
    if (file) {
      write_fstab_entry(v, file);
    }
  }
  printf("\n");
  if (file) {
    fclose(file);
  }
}

Volume* volume_for_path(const char* path) {
  Volume *rec = fs_mgr_get_entry_for_mount_point(fstab, path);

  if (rec == nullptr) {
    return rec;
  }

  if (strcmp(rec->fs_type, "ext4") == 0 || strcmp(rec->fs_type, "f2fs") == 0 ||
      strcmp(rec->fs_type, "vfat") == 0) {
    char *detected_fs_type = blkid_get_tag_value(nullptr, "TYPE", rec->blk_device);

    if (detected_fs_type == nullptr) {
      return rec;
    }

    Volume *fetched_rec = rec;
    while (rec != nullptr && strcmp(rec->fs_type, detected_fs_type) != 0) {
      rec = fs_mgr_get_entry_for_mount_point_after(rec, fstab, path);
    }

    if (rec == nullptr) {
      return fetched_rec;
    }
  }

  return rec;
}

Volume* volume_for_label(const char* label) {
  int i;
  for (i = 0; i < get_num_volumes(); i++) {
    Volume* v = get_device_volumes() + i;
    if (v->label && !strcmp(v->label, label)) {
      return v;
    }
  }
  return nullptr;
}

// Mount the volume specified by path at the given mount_point.
int ensure_path_mounted_at(const char* path, const char* mount_point) {
  Volume* v = volume_for_path(path);
  if (v == nullptr) {
    LOG(ERROR) << "unknown volume for path [" << path << "]";
    return -1;
  }
  if (strcmp(v->fs_type, "ramdisk") == 0) {
    // The ramdisk is always mounted.
    return 0;
  }

  if (!scan_mounted_volumes()) {
    LOG(ERROR) << "Failed to scan mounted volumes";
    return -1;
  }

  if (!mount_point) {
    mount_point = v->mount_point;
  }

  if (!fs_mgr_is_voldmanaged(v)) {
    const MountedVolume* mv = find_mounted_volume_by_mount_point(mount_point);
    if (mv) {
      // volume is already mounted
      return 0;
    }
  }

  mkdir_p(mount_point, 0755);  // in case it doesn't already exist

  if (strcmp(v->fs_type, "ext4") == 0 || strcmp(v->fs_type, "squashfs") == 0 ||
      strcmp(v->fs_type, "vfat") == 0 || strcmp(v->fs_type, "f2fs") == 0) {
    if (mount(v->blk_device, mount_point, v->fs_type, v->flags, v->fs_options) == -1) {
      PLOG(ERROR) << "Failed to mount " << mount_point;
      return -1;
    }
    return 0;
  }

  LOG(ERROR) << "unknown fs_type \"" << v->fs_type << "\" for " << mount_point;
  return -1;
}

int ensure_volume_mounted(Volume* v) {
  if (v == nullptr) {
    LOG(ERROR) << "cannot mount unknown volume";
    return -1;
  }
  return ensure_path_mounted_at(v->mount_point, nullptr);
}

int ensure_path_mounted(const char* path) {
  // Mount at the default mount point.
  return ensure_path_mounted_at(path, nullptr);
}

int ensure_path_unmounted(const char* path, bool detach /* = false */) {
  const Volume* v;
  if (memcmp(path, "/storage/", 9) == 0) {
    char label[PATH_MAX];
    const char* p = path+9;
    const char* q = strchr(p, '/');
    memset(label, 0, sizeof(label));
    if (q) {
      memcpy(label, p, q-p);
    }
    else {
      strcpy(label, p);
    }
    v = volume_for_label(label);
  }
  else {
      v = volume_for_path(path);
  }

  return ensure_volume_unmounted(v, detach);
}

int ensure_volume_unmounted(const Volume* v, bool detach /* = false */) {
  if (v == nullptr) {
    LOG(ERROR) << "cannot unmount unknown volume";
    return -1;
  }
  if (strcmp(v->fs_type, "ramdisk") == 0) {
    // The ramdisk is always mounted; you can't unmount it.
    return -1;
  }

  if (!scan_mounted_volumes()) {
    LOG(ERROR) << "Failed to scan mounted volumes";
    return -1;
  }

  MountedVolume* mv = find_mounted_volume_by_mount_point(v->mount_point);
  if (mv == nullptr) {
    // Volume is already unmounted.
    return 0;
  }

  return (detach ?
          unmount_mounted_volume_detach(mv) :
          unmount_mounted_volume(mv));
}

static int exec_cmd(const char* path, char* const argv[]) {
    int status;
    pid_t child;
    if ((child = vfork()) == 0) {
        execv(path, argv);
        _exit(EXIT_FAILURE);
    }
    waitpid(child, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        LOG(ERROR) << path << " failed with status " << WEXITSTATUS(status);
    }
    return WEXITSTATUS(status);
}

static ssize_t get_file_size(int fd, uint64_t reserve_len) {
  struct stat buf;
  int ret = fstat(fd, &buf);
  if (ret) return 0;

  ssize_t computed_size;
  if (S_ISREG(buf.st_mode)) {
    computed_size = buf.st_size - reserve_len;
  } else if (S_ISBLK(buf.st_mode)) {
    computed_size = get_block_device_size(fd) - reserve_len;
  } else {
    computed_size = 0;
  }

  return computed_size;
}

int format_volume(const char* volume, const char* directory) {
    Volume* v = volume_for_path(volume);
    if (v == NULL) {
        LOG(ERROR) << "unknown volume \"" << volume << "\"";
        return -1;
    }
    if (strcmp(v->fs_type, "ramdisk") == 0) {
        // you can't format the ramdisk.
        LOG(ERROR) << "can't format_volume \"" << volume << "\"";
        return -1;
    }
    if (strcmp(v->mount_point, volume) != 0) {
        LOG(ERROR) << "can't give path \"" << volume << "\" to format_volume";
        return -1;
    }

    if (ensure_path_unmounted(volume) != 0) {
        LOG(ERROR) << "format_volume failed to unmount \"" << v->mount_point << "\"";
        return -1;
    }

    if (fs_mgr_is_voldmanaged(v)) {
        LOG(ERROR) << "can't format vold volume \"" << volume << "\"";
        return -1;
    }

    if (strcmp(v->fs_type, "ext4") == 0 || strcmp(v->fs_type, "f2fs") == 0) {
        // if there's a key_loc that looks like a path, it should be a
        // block device for storing encryption metadata.  wipe it too.
        if (v->key_loc != NULL && v->key_loc[0] == '/') {
            LOG(INFO) << "wiping " << v->key_loc;
            int fd = open(v->key_loc, O_WRONLY | O_CREAT, 0644);
            if (fd < 0) {
                LOG(ERROR) << "format_volume: failed to open " << v->key_loc;
                return -1;
            }
            wipe_block_device(fd, get_file_size(fd));
            close(fd);
        }

        ssize_t length = 0;
        if (v->length != 0) {
            length = v->length;
        } else if (v->key_loc != NULL && strcmp(v->key_loc, "footer") == 0) {
          android::base::unique_fd fd(open(v->blk_device, O_RDONLY));
          if (fd < 0) {
            PLOG(ERROR) << "get_file_size: failed to open " << v->blk_device;
            return -1;
          }
          length = get_file_size(fd.get(), CRYPT_FOOTER_OFFSET);
          if (length <= 0) {
            LOG(ERROR) << "get_file_size: invalid size " << length << " for " << v->blk_device;
            return -1;
          }
        }
        int result;
        if (strcmp(v->fs_type, "ext4") == 0) {
          static constexpr int block_size = 4096;
          int raid_stride = v->logical_blk_size / block_size;
          int raid_stripe_width = v->erase_blk_size / block_size;

          // stride should be the max of 8kb and logical block size
          if (v->logical_blk_size != 0 && v->logical_blk_size < 8192) {
            raid_stride = 8192 / block_size;
          }

          const char* mke2fs_argv[] = { "/sbin/mke2fs_static",
                                        "-F",
                                        "-t",
                                        "ext4",
                                        "-b",
                                        nullptr,
                                        nullptr,
                                        nullptr,
                                        nullptr,
                                        nullptr,
                                        nullptr };

          int i = 5;
          std::string block_size_str = std::to_string(block_size);
          mke2fs_argv[i++] = block_size_str.c_str();

          std::string ext_args;
          if (v->erase_blk_size != 0 && v->logical_blk_size != 0) {
            ext_args = android::base::StringPrintf("stride=%d,stripe-width=%d", raid_stride,
                                                   raid_stripe_width);
            mke2fs_argv[i++] = "-E";
            mke2fs_argv[i++] = ext_args.c_str();
          }

          mke2fs_argv[i++] = v->blk_device;

          std::string size_str = std::to_string(length / block_size);
          if (length != 0) {
            mke2fs_argv[i++] = size_str.c_str();
          }

          result = exec_cmd(mke2fs_argv[0], const_cast<char**>(mke2fs_argv));
          if (result == 0 && directory != nullptr) {
            const char* e2fsdroid_argv[] = { "/sbin/e2fsdroid_static",
                                             "-e",
                                             "-f",
                                             directory,
                                             "-a",
                                             volume,
                                             v->blk_device,
                                             nullptr };

            result = exec_cmd(e2fsdroid_argv[0], const_cast<char**>(e2fsdroid_argv));
          }
        } else {   /* Has to be f2fs because we checked earlier. */
            const char* f2fs_argv[] = { "/sbin/mkfs.f2fs",
                                        "-d1",
                                        "-f",
                                        "-O",
                                        "encrypt",
                                        "-O",
                                        "quota",
                                        v->blk_device,
                                        nullptr,
                                        nullptr };
            if (length != 0) {
                f2fs_argv[8] = (std::to_string(length / 512)).c_str();
            }
            result = exec_cmd(f2fs_argv[0], const_cast<char**>(f2fs_argv));
        }
        if (result != 0) {
            PLOG(ERROR) << "format_volume: make " << v->fs_type << " failed on " << v->blk_device;
            return -1;
        }
        return 0;
    }

    LOG(ERROR) << "format_volume: fs_type \"" << v->fs_type << "\" unsupported";
    return -1;
}

int format_volume(const char* volume) {
  return format_volume(volume, nullptr);
}

int setup_install_mounts() {
  if (fstab == nullptr) {
    LOG(ERROR) << "can't set up install mounts: no fstab loaded";
    return -1;
  }
  for (int i = 0; i < fstab->num_entries; ++i) {
    const Volume* v = fstab->recs + i;

    // We don't want to do anything with "/".
    if (strcmp(v->mount_point, "/") == 0) {
      continue;
    }

    if (strcmp(v->mount_point, "/tmp") == 0 || strcmp(v->mount_point, "/cache") == 0) {
      if (ensure_path_mounted(v->mount_point) != 0) {
        LOG(ERROR) << "Failed to mount " << v->mount_point;
        return -1;
      }
    } else {
      // datamedia and anything managed by vold must be unmounted
      // with the detach flag to ensure that FUSE works.
      bool detach = false;
      if (vdc->isEmulatedStorage() && strcmp(v->mount_point, "/data") == 0) {
        detach = true;
      }
      if (ensure_volume_unmounted(v, detach) != 0) {
        LOG(ERROR) << "Failed to unmount " << v->mount_point;
        return -1;
      }
    }
  }
  return 0;
}
