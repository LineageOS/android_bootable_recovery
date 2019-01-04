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

#ifndef ANDROID_VOLMGR_UTILS_H
#define ANDROID_VOLMGR_UTILS_H

#include <cutils/multiuser.h>
#include <selinux/selinux.h>
#include <utils/Errors.h>

#include <chrono>
#include <string>
#include <vector>

// DISALLOW_COPY_AND_ASSIGN disallows the copy and operator= functions. It goes in the private:
// declarations in a class.
#if !defined(DISALLOW_COPY_AND_ASSIGN)
#define DISALLOW_COPY_AND_ASSIGN(TypeName) \
    TypeName(const TypeName&) = delete;    \
    void operator=(const TypeName&) = delete
#endif

struct DIR;

namespace android {
namespace volmgr {

/* SELinux contexts used depending on the block device type */
extern char* sBlkidContext;
extern char* sBlkidUntrustedContext;
extern char* sFsckContext;
extern char* sFsckUntrustedContext;

status_t CreateDeviceNode(const std::string& path, dev_t dev);
status_t DestroyDeviceNode(const std::string& path);

/* fs_prepare_dir wrapper that creates with SELinux context */
status_t PrepareDir(const std::string& path, mode_t mode, uid_t uid, gid_t gid);

/* Really unmounts the path, killing active processes along the way */
status_t ForceUnmount(const std::string& path, bool detach = false);

/* Kills any processes using given path */
status_t KillProcessesUsingPath(const std::string& path);

/* Creates bind mount from source to target */
status_t BindMount(const std::string& source, const std::string& target);

/* Reads filesystem metadata from device at path */
status_t ReadMetadata(const std::string& path, std::string& fsType, std::string& fsUuid,
                      std::string& fsLabel);

/* Reads filesystem metadata from untrusted device at path */
status_t ReadMetadataUntrusted(const std::string& path, std::string& fsType, std::string& fsUuid,
                               std::string& fsLabel);

/* Returns either WEXITSTATUS() status, or a negative errno */
status_t ForkExecvp(const std::vector<std::string>& args);
status_t ForkExecvp(const std::vector<std::string>& args, char* context);

bool IsFilesystemSupported(const std::string& fsType);

/* Wipes contents of block device at given path */
status_t WipeBlockDevice(const std::string& path);

dev_t GetDevice(const std::string& path);

/* Checks if Android is running in QEMU */
bool IsRunningInEmulator();

}  // namespace volmgr
}  // namespace android

#endif
