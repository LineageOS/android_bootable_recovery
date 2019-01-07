/*
 * Copyright (C) 2013 The CyanogenMod Project
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

#ifndef _RECOVERY_CMDS_H
#define _RECOVERY_CMDS_H

#ifdef __cplusplus
extern "C" {
#endif

int pigz_main(int argc, char **argv);
int miniunz_main(int argc, char **argv);
int minizip_main(int argc, char **argv);
int reboot_main(int argc, char **argv);
int poweroff_main(int argc, char **argv);
int start_main(int argc, char **argv);
int stop_main(int argc, char **argv);
int mksh_main(int argc, char **argv);
int awk_main(int argc, char **argv);

/* Filesystem tools */
int e2fsdroid_main(int argc, char **argv);

int mke2fs_main(int argc, char **argv);
int e2fsck_main(int argc, char **argv);
int resize2fs_main(int argc, char **argv);
int tune2fs_main(int argc, char **argv);

int mkfs_f2fs_main(int argc, char **argv);
int fsck_f2fs_main(int argc, char **argv);

int fsck_msdos_main(int argc, char **argv);

int mkfs_exfat_main(int argc, char **argv);
int fsck_exfat_main(int argc, char **argv);

int mkfs_ntfs_main(int argc, char **argv);
int fsck_ntfs_main(int argc, char **argv);
int mount_ntfs_main(int argc, char **argv);

int sgdisk_main(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif
