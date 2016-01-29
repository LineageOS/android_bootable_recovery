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
#include <ext4_utils/make_ext4fs.h>
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
    return;
  }

  printf("recovery filesystem table\n");
  printf("=========================\n");
  for (int i = 0; i < fstab->num_entries; ++i) {
    const Volume* v = &fstab->recs[i];
    printf("  %d %s %s %s %lld\n", i, v->mount_point, v->fs_type, v->blk_device, v->length);
    write_fstab_entry(v, file);
  }
  printf("\n");
  fclose(file);
}

bool volume_is_mountable(Volume *v)
{
    return (fs_mgr_is_voldmanaged(v) ||
            !strcmp(v->fs_type, "yaffs2") ||
            !strcmp(v->fs_type, "ext4") ||
            !strcmp(v->fs_type, "f2fs") ||
            !strcmp(v->fs_type, "vfat"));
}

bool volume_is_readonly(Volume *v)
{
    return (v->flags & MS_RDONLY);
}

bool volume_is_verity(Volume *v)
{
    return fs_mgr_is_verified(v);
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
int ensure_path_mounted_at(const char* path, const char* mount_point, bool force_rw) {
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
    unsigned long mntflags = v->flags;
    if (!force_rw) {
      if ((v->flags & MS_RDONLY) || fs_mgr_is_verified(v)) {
        mntflags |= MS_RDONLY;
      }
    }
    if (mount(v->blk_device, mount_point, v->fs_type, mntflags, v->fs_options) == -1) {
      PLOG(ERROR) << "Failed to mount " << mount_point;
      return -1;
    }
    return 0;
  }

  LOG(ERROR) << "unknown fs_type \"" << v->fs_type << "\" for " << mount_point;
  return -1;
}

int ensure_volume_mounted(Volume* v, bool force_rw) {
  if (v == nullptr) {
    LOG(ERROR) << "cannot mount unknown volume";
    return -1;
  }
  return ensure_path_mounted_at(v->mount_point, nullptr, force_rw);
}

int remount_for_wipe(const char* path) {
    int ret;

    char *old_fs_options;
    char *new_fs_options;

    char se_context[] = ",context=u:object_r:app_data_file:s0";
    Volume *v;

    // Backup original mount options
    v = volume_for_path(path);
    old_fs_options = v->fs_options;

    // Add SELinux mount override
    asprintf(&new_fs_options, "%s%s", v->fs_options, se_context);
    v->fs_options = new_fs_options;

    ensure_path_unmounted(path);
    ret = ensure_path_mounted(path);

    // Restore original mount options
    v->fs_options = old_fs_options;
    free(new_fs_options);

    return ret;
}

int ensure_path_mounted(const char* path, bool force_rw) {
  // Mount at the default mount point.
  return ensure_path_mounted_at(path, nullptr, force_rw);
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

static int rmtree_except(const char* path, const char* except)
{
    char pathbuf[PATH_MAX];
    int rc = 0;
    DIR* dp = opendir(path);
    if (dp == NULL) {
        return -1;
    }
    struct dirent* de;
    while ((de = readdir(dp)) != NULL) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        if (except && !strcmp(de->d_name, except))
            continue;
        struct stat st;
        snprintf(pathbuf, sizeof(pathbuf), "%s/%s", path, de->d_name);
        rc = lstat(pathbuf, &st);
        if (rc != 0) {
            LOG(ERROR) << "Failed to stat " << pathbuf;
            break;
        }
        if (S_ISDIR(st.st_mode)) {
            rc = rmtree_except(pathbuf, NULL);
            if (rc != 0)
                break;
            rc = rmdir(pathbuf);
        }
        else {
            rc = unlink(pathbuf);
        }
        if (rc != 0) {
            LOG(INFO) << "Failed to remove " << pathbuf << ": " << strerror(errno);
            break;
        }
    }
    closedir(dp);
    return rc;
}

int format_volume(const char* volume, const char* directory, bool force) {
    if (strcmp(volume, "media") == 0) {
        if (!vdc->isEmulatedStorage()) {
            return 0;
        }
        if (ensure_path_mounted("/data") != 0) {
            LOG(ERROR) << "format_volume failed to mount /data";
            return -1;
        }
        remount_for_wipe("/data");
        int rc = 0;
        rc = rmtree_except("/data/media", NULL);
        ensure_path_unmounted("/data");
        return rc;
    }

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

    if (!force && strcmp(volume, "/data") == 0 && vdc->isEmulatedStorage()) {
        if (ensure_path_mounted("/data") == 0) {
            remount_for_wipe("/data");
            // Preserve .layout_version to avoid "nesting bug"
            LOG(INFO) << "Preserving layout version";
            unsigned char layout_buf[256];
            ssize_t layout_buflen = -1;
            int fd;
            fd = open("/data/.layout_version", O_RDONLY);
            if (fd != -1) {
                layout_buflen = read(fd, layout_buf, sizeof(layout_buf));
                close(fd);
            }

            int rc = rmtree_except("/data", "media");

            // Restore .layout_version
            if (layout_buflen > 0) {
                LOG(INFO) << "Restoring layout version";
                fd = open("/data/.layout_version", O_WRONLY | O_CREAT | O_EXCL, 0600);
                if (fd != -1) {
                    write(fd, layout_buf, layout_buflen);
                    close(fd);
                }
            }

            ensure_path_unmounted(volume);

            return rc;
        }
        else {
            LOG(ERROR) << "format_volume failed to mount /data";
            return -1;
        }
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
            length = -CRYPT_FOOTER_OFFSET;
        }
        int result;
        if (strcmp(v->fs_type, "ext4") == 0) {
            if (v->erase_blk_size != 0 && v->logical_blk_size != 0) {
                result = make_ext4fs_directory_align(v->blk_device, length, volume, sehandle,
                        directory, v->erase_blk_size, v->logical_blk_size);
            } else {
                result = make_ext4fs_directory(v->blk_device, length, volume, sehandle, directory);
            }
        } else {   /* Has to be f2fs because we checked earlier. */
            char bytes_reserved[20], num_sectors[20];
            const char* f2fs_argv[6] = {"mkfs.f2fs", "-t1"};
            if (length < 0) {
                snprintf(bytes_reserved, sizeof(bytes_reserved), "%zd", -length);
                f2fs_argv[2] = "-r";
                f2fs_argv[3] = bytes_reserved;
                f2fs_argv[4] = v->blk_device;
                f2fs_argv[5] = NULL;
            } else {
                /* num_sectors can be zero which mean whole device space */
                snprintf(num_sectors, sizeof(num_sectors), "%zd", length / 512);
                f2fs_argv[2] = v->blk_device;
                f2fs_argv[3] = num_sectors;
                f2fs_argv[4] = NULL;
            }
            const char *f2fs_path = "/sbin/mkfs.f2fs";

            result = exec_cmd(f2fs_path, (char* const*)f2fs_argv);
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

int format_volume(const char* volume, bool force) {
  return format_volume(volume, nullptr, force);
}

int setup_install_mounts() {
    if (fstab == NULL) {
        LOG(ERROR) << "can't set up install mounts: no fstab loaded";
        return -1;
    }
    for (int i = 0; i < fstab->num_entries; ++i) {
        Volume* v = fstab->recs + i;

        if (strcmp(v->mount_point, "/tmp") == 0 ||
            strcmp(v->mount_point, "/cache") == 0) {
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
