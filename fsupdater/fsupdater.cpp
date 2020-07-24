/*
 * Copyright (C) 2009 The Android Open Source Project
 * Copyright (C) 2020 The Android Open Kang Project
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

#include "fsupdater/fsupdater.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/capability.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>
#include <dirent.h>
#include <libgen.h>

#include <memory>
#include <string>
#include <vector>

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/parsedouble.h>
#include <android-base/parseint.h>
#include <android-base/properties.h>
#include <android-base/stringprintf.h>
#include <android-base/strings.h>
#include <android-base/unique_fd.h>
#include <applypatch/applypatch.h>
#include <openssl/sha.h>
#include <selinux/label.h>
#include <selinux/selinux.h>
#include <ziparchive/zip_archive.h>

#include "edify/expr.h"
#include "otautil/DirUtil.h"
#include "otautil/error_code.h"
#include "otautil/print_sha1.h"

static std::string updater_src_root = "/source";
static std::string updater_tgt_root = "/target";
static FsUpdaterMode updater_mode = kFsUpdaterModeStrict;

static bool mkdir_p(const std::string& name, mode_t mode) {
  size_t pos = 0;
  while (pos < name.size()) {
    size_t next = name.find('/', pos + 1);
    std::string path = name.substr(0, next);
    int rc = mkdir(path.c_str(), mode);
    if (rc != 0 && errno != EEXIST) {
      return false;
    }
    pos = next;
  }
  return true;
}

static std::string BootSlotSuffix() {
  return android::base::GetProperty("ro.boot.slot_suffix", "");
}

static std::string OtherSuffix(const std::string& suffix) {
  return (suffix == "_a") ? "_b" : "_a";
}

// Send over the buffer to recovery though the command pipe.
static void uiPrint(State* state, const std::string& buffer) {
  FsUpdaterInfo* ui = static_cast<FsUpdaterInfo*>(state->cookie);

  // "line1\nline2\n" will be split into 3 tokens: "line1", "line2" and "".
  // So skip sending empty strings to UI.
  std::vector<std::string> lines = android::base::Split(buffer, "\n");
  for (auto& line : lines) {
    if (!line.empty()) {
      if (ui->cmd_pipe) {
        fprintf(ui->cmd_pipe, "ui_print %s\n", line.c_str());
      }
      else {
        LOG(INFO) << "fsupdater: " << line;
      }
    }
  }

  // On the updater side, we need to dump the contents to stderr (which has
  // been redirected to the log file). Because the recovery will only print
  // the contents to screen when processing pipe command ui_print.
  LOG(INFO) << buffer;
}

static void uiPrintf(State* _Nonnull state, const char* _Nonnull format, ...) {
  std::string error_msg;

  va_list ap;
  va_start(ap, format);
  android::base::StringAppendV(&error_msg, format, ap);
  va_end(ap);

  uiPrint(state, error_msg);
}

/*
 * Parse a possibly symbolic slot name and set the suffix.
 * Acceptable names include:
 *  - Actual name.  "a", "b", etc.
 *  - Active status. "active", "inactive".
 *  - Install status. "source", "target".
 * The suffix will always be either "_a" or "_b".
 */
static bool ParseSlotName(const std::string& name, std::string* suffix) {
  if (name.size() == 2 && name[0] == '_') {
    *suffix = name;
    return true;
  }
  if (name.size() == 1) {
    char buf[2+1];
    sprintf(buf, "_%s", name.c_str());
    suffix->assign(buf);
    return true;
  }
  if (name == "active" || name == "source") {
    *suffix = BootSlotSuffix();
    return true;
  }
  if (name == "inactive" || name == "target") {
    *suffix = OtherSuffix(BootSlotSuffix());
    return true;
  }

  return false;
}

struct perm_parsed_args {
  uid_t uid;
  gid_t gid;
  mode_t mode;
  bool has_selabel;
  const char* selabel;
  bool has_capabilities;
  uint64_t capabilities;
};

static bool ParsePermArgs(const std::vector<std::string>& args, size_t argidx,
                          struct perm_parsed_args* parsed) {
  memset(parsed, 0, sizeof(*parsed));

  if ((args.size() - argidx) % 2 != 1) {
    return false;
  }
  int64_t i64;
  int32_t i32;
  if (sscanf(args[argidx + 0].c_str(), "%" SCNd64, &i64) != 1) {
    return false;
  }
  parsed->uid = i64;
  if (sscanf(args[argidx + 1].c_str(), "%" SCNd64, &i64) != 1) {
    return false;
  }
  parsed->gid = i64;
  if (sscanf(args[argidx + 2].c_str(), "%" SCNi32, &i32) != 1) {
    return false;
  }
  parsed->mode = i32;
  for (size_t i = argidx + 3; i < args.size(); ++i) {
    if (args[i] == "capabilities") {
      if (sscanf(args[i + 1].c_str(), "%" SCNi64, &parsed->capabilities) != 1) {
        return false;
      }
      parsed->has_capabilities = true;
      continue;
    }
    if (args[i] == "selabel") {
      if (args[i + 1].empty()) {
        return false;
      }
      parsed->selabel = args[i + 1].c_str();
      parsed->has_selabel = true;
      continue;
    }
  }
  return true;
}

static int ApplyParsedPerms(State* state, const char* filename, const struct stat* statptr,
                            struct perm_parsed_args parsed) {
  int bad = 0;

  if (parsed.has_selabel) {
    if (lsetfilecon(filename, parsed.selabel) != 0) {
      uiPrintf(state, "ApplyParsedPerms: lsetfilecon of %s to %s failed: %s\n", filename,
               parsed.selabel, strerror(errno));
      bad++;
    }
  }

  /* ignore symlinks */
  if (S_ISLNK(statptr->st_mode)) {
    return bad;
  }

  if (lchown(filename, parsed.uid, parsed.gid) < 0) {
    uiPrintf(state, "ApplyParsedPerms: chown of %s to %d.%d failed: %s\n", filename,
             parsed.uid, parsed.gid, strerror(errno));
    bad++;
  }

  if (chmod(filename, parsed.mode) < 0) {
    uiPrintf(state, "ApplyParsedPerms: chmod of %s to %d failed: %s\n", filename, parsed.mode,
             strerror(errno));
    bad++;
  }

  if (parsed.has_capabilities && S_ISREG(statptr->st_mode)) {
    if (parsed.capabilities == 0) {
      if ((removexattr(filename, XATTR_NAME_CAPS) != 0) && (errno != ENODATA)) {
        // Report failure unless it's ENODATA (attribute not set)
        uiPrintf(state, "ApplyParsedPerms: removexattr of %s to %" PRIx64 " failed: %s\n", filename,
                 parsed.capabilities, strerror(errno));
        bad++;
      }
    } else {
      struct vfs_cap_data cap_data;
      memset(&cap_data, 0, sizeof(cap_data));
      cap_data.magic_etc = VFS_CAP_REVISION | VFS_CAP_FLAGS_EFFECTIVE;
      cap_data.data[0].permitted = (uint32_t)(parsed.capabilities & 0xffffffff);
      cap_data.data[0].inheritable = 0;
      cap_data.data[1].permitted = (uint32_t)(parsed.capabilities >> 32);
      cap_data.data[1].inheritable = 0;
      if (setxattr(filename, XATTR_NAME_CAPS, &cap_data, sizeof(cap_data), 0) < 0) {
        uiPrintf(state, "ApplyParsedPerms: setcap of %s to %" PRIx64 " failed: %s\n", filename,
                 parsed.capabilities, strerror(errno));
        bad++;
      }
    }
  }

  return bad;
}

static void CopyDevice(const std::string& src_dev, const std::string& dst_dev) {
  int src_fd = open(src_dev.c_str(), O_RDONLY);
  int dst_fd = open(dst_dev.c_str(), O_WRONLY);
  unsigned char buf[1024*1024];
  ssize_t nread;
  while ((nread = read(src_fd, buf, sizeof(buf))) > 0) {
    size_t nwritten = 0;
    while (nwritten < (size_t)nread) {
      size_t n = write(dst_fd, buf + nwritten, nread - nwritten);
      //XXX: handle error
      nwritten += n;
    }
  }
  //XXX: handle error (nread < 0)
  close(dst_fd);
  close(src_fd);
}

// slotcopy(srcslot, dstslot)
//   Copy all slot partitions from srcslot to dstslot.
//   Returns nothing.
static Value* SlotcopyFn(const char* name, State* state, const std::vector<std::unique_ptr<Expr>>& argv) {
  if (argv.size() != 2) {
    return ErrorAbort(state, kArgsParsingFailure, "%s() expects 2 arguments, got %zu",
                      name, argv.size());
  }

  std::vector<std::string> args;
  if (!ReadArgs(state, argv, &args)) {
    return ErrorAbort(state, kArgsParsingFailure, "%s() Failed to parse the argument(s)", name);
  }
  const std::string& src_name = args[0];
  const std::string& dst_name = args[1];

  std::string src_suffix;
  if (!ParseSlotName(src_name, &src_suffix)) {
    return ErrorAbort(state, kArgsParsingFailure, "%s() Unrecognized src slot name", name);
  }
  std::string dst_suffix;
  if (!ParseSlotName(dst_name, &dst_suffix)) {
    return ErrorAbort(state, kArgsParsingFailure, "%s() Unrecognized dst slot name", name);
  }

  //XXX: How do we get the path to the devices, eg. "/dev/block/by-name"?
  // Answer: Use the misc partition in fstab.
  // See BootControlAndroid::GetPartitionDevice()

  //XXX: How do we get the fstab in both android and recovery?

  std::vector<std::string> part_names;

  const std::string misc_path = "/dev/block/bootdevice/by-name/misc";
  std::string byname_dir = dirname(misc_path.c_str());
  DIR* dp = opendir(byname_dir.c_str());
  if (!dp) {
    return ErrorAbort(state, kFileOpenFailure, "%s() failed to open %s",
                      name, byname_dir.c_str());
  }
  struct dirent* de;
  while ((de = readdir(dp)) != nullptr) {
    if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) {
      continue;
    }
    const char* suffix = de->d_name + strlen(de->d_name) - 2;
    if (!strcmp(suffix, "_a")) {
      part_names.push_back(std::string(de->d_name, suffix - de->d_name));
    }
  }
  closedir(dp);

  for (const auto& name : part_names) {
    std::string src_dev = byname_dir + "/" + name + src_suffix;
    std::string dst_dev = byname_dir + "/" + name + dst_suffix;
    CopyDevice(src_dev, dst_dev);
  }

  return StringValue("");
}

// slotwriteimg(slot, partition, zipfile)
//   Write zipfile to named slot partition
//   Returns nothing.
static Value* SlotwriteimgFn(const char* name, State* state, const std::vector<std::unique_ptr<Expr>>& argv) {
  if (argv.size() != 3) {
    return ErrorAbort(state, kArgsParsingFailure, "%s() expects 3 arguments, got %zu",
                      name, argv.size());
  }

  std::vector<std::string> args;
  if (!ReadArgs(state, argv, &args)) {
    return ErrorAbort(state, kArgsParsingFailure, "%s() Failed to parse the argument(s)", name);
  }
  const std::string& slot_name = args[0];
  const std::string& partition = args[1];
  const std::string& zipfile = args[2];

  std::string slot_suffix;
  if (!ParseSlotName(slot_name, &slot_suffix)) {
    return ErrorAbort(state, kArgsParsingFailure, "%s() Unrecognized slot name", name);
  }

  //XXX: Hard coded path
  std::string devpath = "/dev/block/bootdevice/by-name/";
  std::string device = devpath + partition + slot_suffix;

  ZipArchiveHandle za = static_cast<FsUpdaterInfo*>(state->cookie)->package_zip;
  ZipString zip_string_path(zipfile.c_str());
  ZipEntry entry;
  if (FindEntry(za, zip_string_path, &entry) != 0) {
    return ErrorAbort(state, kFileOpenFailure, "%s() No %s in package", name, zipfile.c_str());
  }

  android::base::unique_fd fd(TEMP_FAILURE_RETRY(
      open(device.c_str(), O_WRONLY)));
  if (fd < 0) {
    return ErrorAbort(state, kFileOpenFailure, "%s() No %s in package", name, device.c_str());
  }

  ExtractEntryToFile(za, &entry, fd);

  return StringValue("");
}

// slotmount(slot, name, path, fstype, [, options])
//   Mount one slot partition.  Example:
//     slotmount("source", "system", "/system_root", "ext4", "ro")
//     slotmount("target", "vendor", "/vendor", "ext4", "rw")
//   Returns nothing.
static Value* SlotmountFn(const char* name, State* state, const std::vector<std::unique_ptr<Expr>>& argv) {
  if (argv.size() < 4 || argv.size() > 5) {
    return ErrorAbort(state, kArgsParsingFailure, "%s() expects 4 or 5 arguments, got %zu",
                      name, argv.size());
  }

  std::vector<std::string> args;
  if (!ReadArgs(state, argv, &args)) {
    return ErrorAbort(state, kArgsParsingFailure, "%s() Failed to parse the argument(s)", name);
  }
  const std::string& slot_name = args[0];
  std::string slot_suffix;
  std::string root;
  if (!ParseSlotName(slot_name, &slot_suffix)) {
    return ErrorAbort(state, kArgsParsingFailure, "%s() Unrecognized slot name", name);
  }
  root = (slot_suffix == BootSlotSuffix() ? updater_src_root : updater_tgt_root);

  const std::string& partname = args[1];
  if (partname.empty()) {
    return ErrorAbort(state, kArgsParsingFailure, "%s() name cannot be empty", name);
  }
  const std::string& path = args[2];
  if (path.empty()) {
    return ErrorAbort(state, kArgsParsingFailure, "%s() path cannot be empty", name);
  }
  const std::string& fstype = args[3];
  if (fstype.empty()) {
    return ErrorAbort(state, kArgsParsingFailure, "%s() fstype cannot be empty", name);
  }
  std::string options = "ro";
  if (args.size() > 4) {
    options = args[4];
  }

  std::string mntpoint = root + path;

  //XXX: Hard coded path
  std::string devpath = "/dev/block/bootdevice/by-name/";
  std::string device = devpath + partname + slot_suffix;

  //XXX: selinux stuff, see updater/install.cpp@MountFn
  if (!mkdir_p(mntpoint.c_str(), 0755)) {
    return ErrorAbort(state, kVendorFailure, "%s() failed to mkdir %s: %s",
        name, mntpoint.c_str(), strerror(errno));
  }

  //XXX: detect fstype (only if empty?)
  //XXX: check the ro thing
  unsigned long flags = MS_NOATIME | MS_NODEV | MS_NODIRATIME;
  if (options == "ro") {
    flags |= MS_RDONLY;
  }
  if (mount(device.c_str(), mntpoint.c_str(), fstype.c_str(),
            flags, nullptr) != 0) {
    return ErrorAbort(state, kVendorFailure, "%s() failed to mount %s at %s: %s",
        name, device.c_str(), mntpoint.c_str(), strerror(errno));
  }

  return StringValue("");
}

// XXX: do we need this?
// slotunmount(slot, path)
//   Unmount one slot partition.  Example:
//     slotunmount("source", "/system_root")
//   Returns nothing.
static Value* SlotunmountFn(const char* name, State* state, const std::vector<std::unique_ptr<Expr>>& argv) {
  if (argv.size() != 2) {
    return ErrorAbort(state, kArgsParsingFailure, "%s() expects 2 arguments, got %zu",
                      name, argv.size());
  }

  std::vector<std::string> args;
  if (!ReadArgs(state, argv, &args)) {
    return ErrorAbort(state, kArgsParsingFailure, "%s() Failed to parse the argument(s)", name);
  }
  const std::string& slot_name = args[0];
  std::string root;
  if (slot_name == "source") {
    root = updater_src_root;
  }
  else if (slot_name == "target") {
    root = updater_tgt_root;
  }
  else {
    return ErrorAbort(state, kArgsParsingFailure, "%s() Unrecognized slot name", name);
  }
  const std::string& path = args[1];
  if (path.empty()) {
    return ErrorAbort(state, kArgsParsingFailure, "%s() path cannot be empty", name);
  }

  std::string mntpoint = root + path;

  if (umount(mntpoint.c_str()) != 0) {
    return ErrorAbort(state, kVendorFailure, "%s() failed to unmount %s: %s",
        name, mntpoint.c_str(), strerror(errno));
  }

  return StringValue("");
}

// mkdir(path, uid, gid, mode, ...)
//   Create a directory.
//   Returns nothing.
static Value* MkdirFn(const char* name, State* state, const std::vector<std::unique_ptr<Expr>>& argv) {
  if (argv.size() < 4) {
    return ErrorAbort(state, kArgsParsingFailure, "%s() expects at least 4 arguments, got %zu",
                      name, argv.size());
  }

  std::vector<std::string> args;
  if (!ReadArgs(state, argv, &args)) {
    return ErrorAbort(state, kArgsParsingFailure, "%s() Failed to parse the argument(s)", name);
  }

  const auto path = updater_tgt_root + args[0];

  struct perm_parsed_args parsed;
  if (!ParsePermArgs(args, 1, &parsed)) {
    return ErrorAbort(state, kArgsParsingFailure, "%s() Failed to parse the argument(s)", name);
  }

  int bad = 0;

  if (mkdir(path.c_str(), parsed.mode) != 0) {
    if (errno != EEXIST || updater_mode == kFsUpdaterModeStrict) {
      return ErrorAbort(state, kFileOpenFailure, "%s: Error on mkdir of \"%s\": %s", name,
                        path.c_str(), strerror(errno));
    }
  }

  struct stat st;
  if (stat(path.c_str(), &st) != 0) {
    return ErrorAbort(state, kFileOpenFailure, "%s: Error on stat of \"%s\": %s", name,
                      path.c_str(), strerror(errno));
  }

  bad += ApplyParsedPerms(state, path.c_str(), &st, parsed);

  if (bad > 0) {
    return ErrorAbort(state, kSetMetadataFailure, "%s: some changes failed", name);
  }

  return StringValue("");
}

// rmdir(path)
//   Delete a directory.
//   Returns nothing.
static Value* RmdirFn(const char* name, State* state, const std::vector<std::unique_ptr<Expr>>& argv) {
  if (argv.size() != 1) {
    return ErrorAbort(state, kArgsParsingFailure, "%s() expects 1 argument, got %zu",
                      name, argv.size());
  }

  std::vector<std::string> args;
  if (!ReadArgs(state, argv, &args)) {
    return ErrorAbort(state, kArgsParsingFailure, "%s() Failed to parse the argument(s)", name);
  }

  const auto path = updater_tgt_root + args[0];

  if (rmdir(path.c_str()) != 0) {
    return ErrorAbort(state, kFileOpenFailure, "%s: Error on rmdir of \"%s\": %s", name,
                      path.c_str(), strerror(errno));
  }

  return StringValue("");
}

// create(path, zipfile, uid, gid, mode, ...)
//   Create a file.
//   Returns nothing.
static Value* CreateFn(const char* name, State* state, const std::vector<std::unique_ptr<Expr>>& argv) {
  if (argv.size() < 5) {
    return ErrorAbort(state, kArgsParsingFailure, "%s() expects at least 5 arguments, got %zu",
                      name, argv.size());
  }

  std::vector<std::string> args;
  if (!ReadArgs(state, argv, &args)) {
    return ErrorAbort(state, kArgsParsingFailure, "%s() Failed to parse the argument(s)", name);
  }

  const auto path = updater_tgt_root + args[0];
  const auto& zipfile = args[1];

  struct perm_parsed_args parsed;
  if (!ParsePermArgs(args, 2, &parsed)) {
    return ErrorAbort(state, kArgsParsingFailure, "%s() Failed to parse the argument(s)", name);
  }

  int bad = 0;

  ZipArchiveHandle za = static_cast<FsUpdaterInfo*>(state->cookie)->package_zip;
  ZipString zip_string_path(zipfile.c_str());
  ZipEntry entry;
  if (FindEntry(za, zip_string_path, &entry) != 0) {
    return ErrorAbort(state, kFileOpenFailure, "%s() No %s in package", name, zipfile.c_str());
  }

  struct stat st;
  if (lstat(path.c_str(), F_OK) == 0) {
    if (updater_mode == kFsUpdaterModeStrict) {
      return ErrorAbort(state, kFileOpenFailure, "%s() File %s exists", name, path.c_str());
    }
    return StringValue("");
  }

  android::base::unique_fd fd(TEMP_FAILURE_RETRY(
      open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR)));
  if (fd < 0) {
    return ErrorAbort(state, kFileOpenFailure, "%s() No %s in package", name, path.c_str());
  }

  ExtractEntryToFile(za, &entry, fd);

  if (stat(path.c_str(), &st) != 0) {
    return ErrorAbort(state, kFileOpenFailure, "%s() Failed to create %s", name, path.c_str());
  }

  bad += ApplyParsedPerms(state, path.c_str(), &st, parsed);

  if (bad > 0) {
    return ErrorAbort(state, kSetMetadataFailure, "%s: some changes failed", name);
  }

  if (fsync(fd) != 0) {
    return ErrorAbort(state, kFsyncFailure, "%s() No %s in package", name, path.c_str());
  }
  close(fd.release());

  return StringValue("");
}

// symlink(target, path, uid, gid)
//   Create symbolic link.
//   Returns nothing.
static Value* SymlinkFn(const char* name, State* state, const std::vector<std::unique_ptr<Expr>>& argv) {
  if (argv.size() != 4) {
    return ErrorAbort(state, kArgsParsingFailure, "%s() expects 4 arguments, got %zu",
                      name, argv.size());
  }

  std::vector<std::string> args;
  if (!ReadArgs(state, argv, &args)) {
    return ErrorAbort(state, kArgsParsingFailure, "%s() Failed to parse the argument(s)", name);
  }

  const auto& target = args[0];
  const auto path = updater_tgt_root + args[1];

  int64_t uid, gid;
  if (sscanf(args[2].c_str(), "%" SCNd64, &uid) != 1 ||
      sscanf(args[3].c_str(), "%" SCNd64, &gid) != 1) {
    return ErrorAbort(state, kArgsParsingFailure, "%s() Failed to parse the argument(s)", name);
  }

  struct stat st;
  if (lstat(path.c_str(), &st) == 0) {
    if (updater_mode == kFsUpdaterModeStrict) {
      return ErrorAbort(state, kFileOpenFailure, "%s() File %s exists", name, path.c_str());
    }
    return StringValue("");
  }

  if (symlink(target.c_str(), path.c_str()) != 0) {
    return ErrorAbort(state, kSymlinkFailure, "%s: Error on symlink of \"%s\": %s", name,
                      path.c_str(), strerror(errno));
  }
  if (lchown(path.c_str(), uid, gid) != 0) {
    return ErrorAbort(state, kSymlinkFailure, "%s: Error on lchown of \"%s\": %s", name,
                      path.c_str(), strerror(errno));
  }

  return StringValue("");
}

// patch(path, zipfile, old_digest)
//   Patch a file.
//   Returns nothing.
Value* PatchFn(const char* name, State* state,
                    const std::vector<std::unique_ptr<Expr>>& argv) {
  if (argv.size() != 3) {
    return ErrorAbort(state, kArgsParsingFailure,
                      "%s(): expected 4 args, got %zu", name, argv.size());
  }

  std::vector<std::string> args;
  if (!ReadArgs(state, argv, &args)) {
    return ErrorAbort(state, kArgsParsingFailure, "%s() Failed to parse the argument(s)", name);
  }
  const std::string  filename = updater_tgt_root + args[0];
  const std::string& zipfile = args[1];
  const std::string& old_digest = args[2];
  if (old_digest.size() != 2 * SHA_DIGEST_LENGTH) {
    return ErrorAbort(state, kArgsParsingFailure, "%s() Bad digest length", name);
  }

  int ret;
  int fd;

  ZipArchiveHandle za = static_cast<FsUpdaterInfo*>(state->cookie)->package_zip;
  ZipString zip_string_path(zipfile.c_str());
  ZipEntry entry;
  if (FindEntry(za, zip_string_path, &entry) != 0) {
    return ErrorAbort(state, kFileOpenFailure, "%s() No %s in package", name, zipfile.c_str());
  }

  std::vector<uint8_t> patch_data;
  patch_data.resize(entry.uncompressed_length);
  ret = ExtractToMemory(za, &entry, patch_data.data(), patch_data.size());
  if (ret != 0) {
    return ErrorAbort(state, kFileOpenFailure, "%s() Failed to read patch %s: %d", name, zipfile.c_str(), ret);
  }
  Value patch(VAL_BLOB, std::string(patch_data.cbegin(), patch_data.cend()));

  struct stat st;
  if (stat(filename.c_str(), &st) != 0) {
    return ErrorAbort(state, kFileOpenFailure, "%s() Failed to stat %s", name, filename.c_str());
  }

  std::vector<unsigned char> old_data(st.st_size);
  fd = open(filename.c_str(), O_RDONLY);
  if (fd == -1) {
    return ErrorAbort(state, kFileOpenFailure, "%s() Failed to open %s", name, filename.c_str());
  }
  ssize_t nread = read(fd, old_data.data(), old_data.size());
  close(fd);
  if (nread != (ssize_t)st.st_size) {
    return ErrorAbort(state, kFreadFailure, "%s() Failed to read %s", name, filename.c_str());
  }

  SHA_CTX sha_ctx;
  SHA1_Init(&sha_ctx);
  SHA1_Update(&sha_ctx, old_data.data(), old_data.size());
  uint8_t digest[SHA_DIGEST_LENGTH];
  SHA1_Final(digest, &sha_ctx);
  char hexdigest[2 * SHA_DIGEST_LENGTH + 1];
  for (size_t n = 0; n < SHA_DIGEST_LENGTH; ++n) {
      sprintf(&hexdigest[2 * n], "%02x", digest[n]);
  }

  uiPrintf(state, "File %s size %u digest %s", filename.c_str(), old_data.size(), hexdigest);

  if (memcmp(old_digest.c_str(), hexdigest, 2 * SHA_DIGEST_LENGTH) != 0) {
    if (updater_mode == kFsUpdaterModeStrict) {
      return ErrorAbort(state, kPatchApplicationFailure, "%s() Digest mismatch for %s", name, filename.c_str());
    }
    uiPrintf(state, "digest mismatch, not patching %s: expected %s, got %s",
             filename.c_str(), old_digest.c_str(), hexdigest);
    return StringValue("");
  }

  std::string new_data;
  SinkFn sink = [&new_data](const unsigned char* data, size_t len) {
    new_data.append(reinterpret_cast<const char*>(data), len);
    return len;
  };

  ret = ApplyBSDiffPatch(old_data.data(), old_data.size(), patch, 0, sink, nullptr);
  if (ret != 0) {
    //XXX: ErrorAbort
    uiPrintf(state, "Failed to patch %s: ApplyBSDiffPatch returned %d\n", filename.c_str(), ret);
  }

  fd = open(filename.c_str(), O_WRONLY);
  if (fd == -1) {
    return ErrorAbort(state, kFileOpenFailure, "%s() Failed to open %s: %s", name, filename.c_str(), strerror(errno));
  }
  size_t nwritten = write(fd, new_data.c_str(), new_data.size());
  close(fd);
  if (nwritten != new_data.size()) {
    return ErrorAbort(state, kFwriteFailure, "%s() Failed to write %s", name, filename.c_str());
  }

  return StringValue("");
}

// unlink(path)
//   Deletes path.
//   Returns nothing.
static Value* UnlinkFn(const char* name, State* state, const std::vector<std::unique_ptr<Expr>>& argv) {
  if (argv.size() != 1) {
    return ErrorAbort(state, kArgsParsingFailure, "%s() expects 1 argument, got %zu",
                      name, argv.size());
  }

  std::vector<std::string> args;
  if (!ReadArgs(state, argv, &args)) {
    return ErrorAbort(state, kArgsParsingFailure, "%s() Failed to parse the argument(s)", name);
  }

  const auto path = updater_tgt_root + args[0];

  struct stat sb;
  if (lstat(path.c_str(), &sb) != 0) {
    if (updater_mode == kFsUpdaterModeStrict) {
      return ErrorAbort(state, kSetMetadataFailure, "%s: Error on lstat of \"%s\": %s", name,
                        path.c_str(), strerror(errno));
    }
    return StringValue("");
  }

  if (unlink(path.c_str()) != 0) {
    return ErrorAbort(state, kFwriteFailure, "%s: Error on unlink of \"%s\": %s", name,
                      path.c_str(), strerror(errno));
  }

  return StringValue("");
}

// chown(path, uid, gid)
//   Change ownership of existing path.
//   Returns nothing.
static Value* ChownFn(const char* name, State* state, const std::vector<std::unique_ptr<Expr>>& argv) {
  if (argv.size() != 3) {
    return ErrorAbort(state, kArgsParsingFailure, "%s() expects 3 arguments, got %zu",
                      name, argv.size());
  }

  std::vector<std::string> args;
  if (!ReadArgs(state, argv, &args)) {
    return ErrorAbort(state, kArgsParsingFailure, "%s() Failed to parse the argument(s)", name);
  }

  const auto path = updater_tgt_root + args[0];

  int64_t uid, gid;
  if (sscanf(args[2].c_str(), "%" SCNd64, &uid) != 1 ||
      sscanf(args[3].c_str(), "%" SCNd64, &gid) != 1) {
    return ErrorAbort(state, kArgsParsingFailure, "%s() Failed to parse the argument(s)", name);
  }

  if (lchown(path.c_str(), uid, gid) != 0) {
    return ErrorAbort(state, kSymlinkFailure, "%s: Error on lchown of \"%s\": %s", name,
                      path.c_str(), strerror(errno));
  }

  return StringValue("");
}

// chmeta(path, uid, gid, mode, ...)
//   Change metadata of existing path.
//   Returns nothing.
static Value* ChmetaFn(const char* name, State* state, const std::vector<std::unique_ptr<Expr>>& argv) {
  if (argv.size() < 4) {
    return ErrorAbort(state, kArgsParsingFailure, "%s() expects at least 4 arguments, got %zu",
                      name, argv.size());
  }

  std::vector<std::string> args;
  if (!ReadArgs(state, argv, &args)) {
    return ErrorAbort(state, kArgsParsingFailure, "%s() Failed to parse the argument(s)", name);
  }

  const auto path = updater_tgt_root + args[0];

  struct stat sb;
  if (lstat(path.c_str(), &sb) != 0) {
    if (updater_mode == kFsUpdaterModeStrict) {
      return ErrorAbort(state, kSetMetadataFailure, "%s: Error on lstat of \"%s\": %s", name,
                        path.c_str(), strerror(errno));
    }
    return StringValue("");
  }

  struct perm_parsed_args parsed;
  ParsePermArgs(args, 0, &parsed);
  int bad = 0;

  bad += ApplyParsedPerms(state, path.c_str(), &sb, parsed);

  if (bad > 0) {
    return ErrorAbort(state, kSetMetadataFailure, "%s: some changes failed", name);
  }

  return StringValue("");
}


/*** Legacy updater functions ***/

//XXX: fix this
#define UpdaterInfo FsUpdaterInfo

static Value* GetPropFn(const char* name, State* state, const std::vector<std::unique_ptr<Expr>>& argv) {
  if (argv.size() != 1) {
    return ErrorAbort(state, kArgsParsingFailure, "%s() expects 1 arg, got %zu", name, argv.size());
  }
  std::string key;
  if (!Evaluate(state, argv[0], &key)) {
    return nullptr;
  }
  std::string value = android::base::GetProperty(key, "");

  return StringValue(value);
}

static Value* UIPrintFn(const char* name, State* state, const std::vector<std::unique_ptr<Expr>>& argv) {
  std::vector<std::string> args;
  if (!ReadArgs(state, argv, &args)) {
    return ErrorAbort(state, kArgsParsingFailure, "%s(): Failed to parse the argument(s)", name);
  }

  std::string buffer = android::base::Join(args, "");
  uiPrint(state, buffer);
  return StringValue(buffer);
}

static Value* ShowProgressFn(const char* name, State* state,
                      const std::vector<std::unique_ptr<Expr>>& argv) {
  if (argv.size() != 2) {
    return ErrorAbort(state, kArgsParsingFailure, "%s() expects 2 arguments, got %zu", name,
                      argv.size());
  }

  std::vector<std::string> args;
  if (!ReadArgs(state, argv, &args)) {
    return ErrorAbort(state, kArgsParsingFailure, "%s() Failed to parse the argument(s)", name);
  }
  const std::string& frac_str = args[0];
  const std::string& sec_str = args[1];

  double frac;
  if (!android::base::ParseDouble(frac_str.c_str(), &frac)) {
    return ErrorAbort(state, kArgsParsingFailure, "%s: failed to parse double in %s", name,
                      frac_str.c_str());
  }
  int sec;
  if (!android::base::ParseInt(sec_str.c_str(), &sec)) {
    return ErrorAbort(state, kArgsParsingFailure, "%s: failed to parse int in %s", name,
                      sec_str.c_str());
  }

  UpdaterInfo* ui = static_cast<UpdaterInfo*>(state->cookie);
  if (ui->cmd_pipe) {
    fprintf(ui->cmd_pipe, "progress %f %d\n", frac, sec);
  }

  return StringValue(frac_str);
}

static Value* SetProgressFn(const char* name, State* state,
                     const std::vector<std::unique_ptr<Expr>>& argv) {
  if (argv.size() != 1) {
    return ErrorAbort(state, kArgsParsingFailure, "%s() expects 1 arg, got %zu", name, argv.size());
  }

  std::vector<std::string> args;
  if (!ReadArgs(state, argv, &args)) {
    return ErrorAbort(state, kArgsParsingFailure, "%s() Failed to parse the argument(s)", name);
  }
  const std::string& frac_str = args[0];

  double frac;
  if (!android::base::ParseDouble(frac_str.c_str(), &frac)) {
    return ErrorAbort(state, kArgsParsingFailure, "%s: failed to parse double in %s", name,
                      frac_str.c_str());
  }

  UpdaterInfo* ui = static_cast<UpdaterInfo*>(state->cookie);
  if (ui->cmd_pipe) {
    fprintf(ui->cmd_pipe, "set_progress %f\n", frac);
  }

  return StringValue(frac_str);
}



/*
 * These are the edify functions for filesystem updater.
 *
 * The OTA generator uses some legacy updater functions.  We re-implement
 * them here so that they work for A/B updates since the system version
 * of update-engine does not link the legacy updater library.  Note that
 * we want the legacy updater library to provide the implementations in
 * recovery.  The updater registers us before the legacy updater library
 * so that our functions are overridden.
 */
void RegisterFsUpdaterFunctions() {
  RegisterFunction("slotcopy", SlotcopyFn);
  RegisterFunction("slotwriteimg", SlotwriteimgFn);
  RegisterFunction("slotmount", SlotmountFn);
  RegisterFunction("slotunmount", SlotunmountFn);
  RegisterFunction("mkdir", MkdirFn);
  RegisterFunction("rmdir", RmdirFn);
//XXX  RegisterFunction("mktree", MktreeFn);
//XXX  RegisterFunction("rmtree", RmtreeFn);
  RegisterFunction("create", CreateFn);
  RegisterFunction("symlink", SymlinkFn);
  RegisterFunction("patch", PatchFn);
  RegisterFunction("unlink", UnlinkFn);
  RegisterFunction("chown", ChownFn);
  RegisterFunction("chmeta", ChmetaFn);

  // Legacy updater functions
  RegisterFunction("getprop", GetPropFn);
  RegisterFunction("ui_print", UIPrintFn);
  RegisterFunction("show_progress", ShowProgressFn);
  RegisterFunction("set_progress", SetProgressFn);
}

void SetFsUpdaterRoot(const std::string& src_root, const std::string& tgt_root) {
  updater_src_root = src_root;
  updater_tgt_root = tgt_root;
  mkdir_p(updater_tgt_root, 0700);
}

void SetFsUpdaterMode(FsUpdaterMode mode) {
  updater_mode = mode;
}
