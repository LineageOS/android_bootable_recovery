/*
 * Copyright (C) 2015 The Android Open Source Project
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

#include "sehandle.h"
#include "Utils.h"
#include "Process.h"
#include <volume_manager/VolumeManager.h>

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android-base/stringprintf.h>

#include <cutils/fs.h>
#include <private/android_filesystem_config.h>

#include <mutex>
#include <dirent.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <stdlib.h>
#include <sys/mount.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <sys/statvfs.h>
#include <thread>

#ifndef UMOUNT_NOFOLLOW
#define UMOUNT_NOFOLLOW    0x00000008  /* Don't follow symlink on umount */
#endif

using android::base::ReadFileToString;
using android::base::StringPrintf;

using namespace std::chrono_literals;

namespace android {
namespace volmgr {

security_context_t sBlkidContext = nullptr;
security_context_t sBlkidUntrustedContext = nullptr;
security_context_t sFsckContext = nullptr;
security_context_t sFsckUntrustedContext = nullptr;

#include <blkid/blkid.h>

static const char* kProcFilesystems = "/proc/filesystems";

status_t CreateDeviceNode(const std::string& path, dev_t dev) {
    const char* cpath = path.c_str();
    status_t res = 0;

    char* secontext = nullptr;
    if (sehandle) {
        if (!selabel_lookup(sehandle, &secontext, cpath, S_IFBLK)) {
            setfscreatecon(secontext);
        }
    }

    mode_t mode = 0660 | S_IFBLK;
    if (mknod(cpath, mode, dev) < 0) {
        if (errno != EEXIST) {
            PLOG(ERROR) << "Failed to create device node for " << major(dev)
                    << ":" << minor(dev) << " at " << path;
            res = -errno;
        }
    }

    if (secontext) {
        setfscreatecon(nullptr);
        freecon(secontext);
    }

    return res;
}

status_t DestroyDeviceNode(const std::string& path) {
    const char* cpath = path.c_str();
    if (TEMP_FAILURE_RETRY(unlink(cpath))) {
        return -errno;
    } else {
        return OK;
    }
}

status_t PrepareDir(const std::string& path, mode_t mode, uid_t uid, gid_t gid) {
    const char* cpath = path.c_str();

    char* secontext = nullptr;
    if (sehandle) {
        if (!selabel_lookup(sehandle, &secontext, cpath, S_IFDIR)) {
            setfscreatecon(secontext);
        }
    }

    int res = fs_prepare_dir(cpath, mode, uid, gid);

    if (secontext) {
        setfscreatecon(nullptr);
        freecon(secontext);
    }

    if (res == 0) {
        return OK;
    } else {
        return -errno;
    }
}

status_t ForceUnmount(const std::string& path, bool detach /* = false */) {
    const char* cpath = path.c_str();
    if (detach) {
        if (!umount2(path.c_str(), UMOUNT_NOFOLLOW | MNT_DETACH) || errno == EINVAL || errno == ENOENT) {
            return OK;
        }
        PLOG(WARNING) << "Failed to unmount " << path;
        return -errno;
    }
    if (!umount2(cpath, UMOUNT_NOFOLLOW) || errno == EINVAL || errno == ENOENT) {
        return OK;
    }
    sleep(1);

    Process::killProcessesWithOpenFiles(cpath, SIGINT);
    sleep(1);
    if (!umount2(cpath, UMOUNT_NOFOLLOW) || errno == EINVAL || errno == ENOENT) {
        return OK;
    }

    Process::killProcessesWithOpenFiles(cpath, SIGTERM);
    sleep(1);
    if (!umount2(cpath, UMOUNT_NOFOLLOW) || errno == EINVAL || errno == ENOENT) {
        return OK;
    }

    Process::killProcessesWithOpenFiles(cpath, SIGKILL);
    sleep(1);
    if (!umount2(cpath, UMOUNT_NOFOLLOW) || errno == EINVAL || errno == ENOENT) {
        return OK;
    }

    return -errno;
}

status_t KillProcessesUsingPath(const std::string& path) {
    const char* cpath = path.c_str();
    if (Process::killProcessesWithOpenFiles(cpath, SIGINT) == 0) {
        return OK;
    }
    sleep(1);

    if (Process::killProcessesWithOpenFiles(cpath, SIGTERM) == 0) {
        return OK;
    }
    sleep(1);

    if (Process::killProcessesWithOpenFiles(cpath, SIGKILL) == 0) {
        return OK;
    }
    sleep(1);

    // Send SIGKILL a second time to determine if we've
    // actually killed everyone with open files
    if (Process::killProcessesWithOpenFiles(cpath, SIGKILL) == 0) {
        return OK;
    }
    PLOG(ERROR) << "Failed to kill processes using " << path;
    return -EBUSY;
}

status_t BindMount(const std::string& source, const std::string& target) {
    if (::mount(source.c_str(), target.c_str(), "", MS_BIND, NULL)) {
        PLOG(ERROR) << "Failed to bind mount " << source << " to " << target;
        return -errno;
    }
    return OK;
}

static status_t readMetadata(const std::string& path, std::string& fsType,
        std::string& fsUuid, std::string& fsLabel) {
    char *val = NULL;
    val = blkid_get_tag_value(NULL, "TYPE", path.c_str());
    if (val) {
        fsType = val;
    }
    val = blkid_get_tag_value(NULL, "UUID", path.c_str());
    if (val) {
        fsUuid = val;
    }
    val = blkid_get_tag_value(NULL, "LABEL", path.c_str());
    if (val) {
        fsLabel = val;
    }

    return OK;
}

status_t ReadMetadata(const std::string& path, std::string& fsType,
        std::string& fsUuid, std::string& fsLabel) {
    return readMetadata(path, fsType, fsUuid, fsLabel);
}

status_t ReadMetadataUntrusted(const std::string& path, std::string& fsType,
        std::string& fsUuid, std::string& fsLabel) {
    return readMetadata(path, fsType, fsUuid, fsLabel);
}

status_t ForkExecvp(const std::vector<std::string>& args) {
    return ForkExecvp(args, nullptr);
}

status_t ForkExecvp(const std::vector<std::string>& args, security_context_t context) {
    std::vector<std::string> output;
    return ForkExecvp(args, output, context);
}

status_t ForkExecvp(const std::vector<std::string>& args,
        std::vector<std::string>& output) {
    return ForkExecvp(args, output, nullptr);
}

status_t ForkExecvp(const std::vector<std::string>& args,
        std::vector<std::string>& output, security_context_t context) {
    std::string cmd;
    for (size_t i = 0; i < args.size(); i++) {
        cmd += args[i] + " ";
        if (i == 0) {
            LOG(VERBOSE) << args[i];
        } else {
            LOG(VERBOSE) << "    " << args[i];
        }
    }
    output.clear();

    if (setexeccon(context)) {
        LOG(ERROR) << "Failed to setexeccon";
        abort();
    }
    FILE* fp = popen(cmd.c_str(), "r"); // NOLINT
    if (setexeccon(nullptr)) {
        LOG(ERROR) << "Failed to setexeccon";
        abort();
    }

    if (!fp) {
        PLOG(ERROR) << "Failed to popen " << cmd;
        return -errno;
    }
    char line[1024];
    while (fgets(line, sizeof(line), fp) != nullptr) {
        LOG(VERBOSE) << line;
        output.push_back(std::string(line));
    }
    if (pclose(fp) != 0) {
        PLOG(ERROR) << "Failed to pclose " << cmd;
        return -errno;
    }

    return OK;
}

pid_t ForkExecvpAsync(const std::vector<std::string>& args) {
    size_t argc = args.size();
    char** argv = (char**) calloc(argc + 1, sizeof(char*));
    for (size_t i = 0; i < argc; i++) {
        argv[i] = (char*) args[i].c_str();
        if (i == 0) {
            LOG(VERBOSE) << args[i];
        } else {
            LOG(VERBOSE) << "    " << args[i];
        }
    }

    pid_t pid = fork();
    if (pid == 0) {
        close(STDIN_FILENO);
        close(STDOUT_FILENO);
        close(STDERR_FILENO);

        if (execvp(argv[0], argv)) {
            PLOG(ERROR) << "Failed to exec";
        }

        _exit(1);
    }

    if (pid == -1) {
        PLOG(ERROR) << "Failed to exec";
    }

    free(argv);
    return pid;
}

bool IsFilesystemSupported(const std::string& fsType) {
    std::string supported;
    if (!ReadFileToString(kProcFilesystems, &supported)) {
        PLOG(ERROR) << "Failed to read supported filesystems";
        return false;
    }

#ifdef CONFIG_EXFAT_DRIVER
    /* Add exfat if an exfat driver is present */
    if (supported.find(CONFIG_EXFAT_DRIVER "\n") != std::string::npos)
        supported.append("nodev\texfat\n");
#endif

    return supported.find(fsType + "\n") != std::string::npos;
}

status_t WipeBlockDevice(const std::string& path) {
    status_t res = -1;
    const char* c_path = path.c_str();
    unsigned long nr_sec = 0;
    unsigned long long range[2];

    int fd = TEMP_FAILURE_RETRY(open(c_path, O_RDWR | O_CLOEXEC));
    if (fd == -1) {
        PLOG(ERROR) << "Failed to open " << path;
        goto done;
    }

    if ((ioctl(fd, BLKGETSIZE, &nr_sec)) == -1) {
        PLOG(ERROR) << "Failed to determine size of " << path;
        goto done;
    }

    range[0] = 0;
    range[1] = (unsigned long long) nr_sec * 512;

    LOG(INFO) << "About to discard " << range[1] << " on " << path;
    if (ioctl(fd, BLKDISCARD, &range) == 0) {
        LOG(INFO) << "Discard success on " << path;
        res = 0;
    } else {
        PLOG(ERROR) << "Discard failure on " << path;
    }

done:
    close(fd);
    return res;
}

dev_t GetDevice(const std::string& path) {
    struct stat sb;
    if (stat(path.c_str(), &sb)) {
        PLOG(WARNING) << "Failed to stat " << path;
        return 0;
    } else {
        return sb.st_dev;
    }
}

status_t SaneReadLinkAt(int dirfd, const char* path, char* buf, size_t bufsiz) {
    ssize_t len = readlinkat(dirfd, path, buf, bufsiz);
    if (len < 0) {
        return -1;
    } else if (len == (ssize_t) bufsiz) {
        return -1;
    } else {
        buf[len] = '\0';
        return 0;
    }
}

bool IsRunningInEmulator() {
    return android::base::GetBoolProperty("ro.kernel.qemu", false);
}

}  // namespace volmgr
}  // namespace android
