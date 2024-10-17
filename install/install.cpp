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

#include "install/install.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/parsedouble.h>
#include <android-base/parseint.h>
#include <android-base/properties.h>
#include <android-base/stringprintf.h>
#include <android-base/strings.h>
#include <android-base/unique_fd.h>

#include "bootloader_message/bootloader_message.h"
#include "install/snapshot_utils.h"
#include "install/spl_check.h"
#include "install/wipe_data.h"
#include "install/wipe_device.h"
#include "otautil/error_code.h"
#include "otautil/package.h"
#include "otautil/paths.h"
#include "otautil/sysutil.h"
#include "otautil/verifier.h"
#include "private/setup_commands.h"
#include "recovery_ui/device.h"
#include "recovery_ui/ui.h"
#include "recovery_utils/roots.h"
#include "recovery_utils/thermalutil.h"

using namespace std::chrono_literals;

bool ask_to_ab_reboot(Device* device);
bool ask_to_continue_unverified(Device* device);
bool ask_to_continue_downgrade(Device* device);

static constexpr int kRecoveryApiVersion = 3;
// We define RECOVERY_API_VERSION in Android.mk, which will be picked up by build system and packed
// into target_files.zip. Assert the version defined in code and in Android.mk are consistent.
static_assert(kRecoveryApiVersion == RECOVERY_API_VERSION, "Mismatching recovery API versions.");

// Default allocation of progress bar segments to operations
static constexpr int VERIFICATION_PROGRESS_TIME = 60;
static constexpr float VERIFICATION_PROGRESS_FRACTION = 0.25;
// The charater used to separate dynamic fingerprints. e.x. sargo|aosp-sargo
#define FINGERPRING_SEPARATOR "|"
static constexpr auto&& RELEASE_KEYS_TAG = "release-keys";
// If brick packages are smaller than |MEMORY_PACKAGE_LIMIT|, read the entire package into memory
static constexpr size_t MEMORY_PACKAGE_LIMIT = 1024 * 1024;

static std::condition_variable finish_log_temperature;
static bool isInStringList(const std::string& target_token, const std::string& str_list,
                           const std::string& deliminator);

bool ReadMetadataFromPackage(ZipArchiveHandle zip, std::map<std::string, std::string>* metadata) {
  CHECK(metadata != nullptr);

  static constexpr const char* METADATA_PATH = "META-INF/com/android/metadata";
  ZipEntry64 entry;
  if (FindEntry(zip, METADATA_PATH, &entry) != 0) {
    return false;
  }

  uint32_t length = entry.uncompressed_length;
  std::string metadata_string(length, '\0');
  int32_t err =
      ExtractToMemory(zip, &entry, reinterpret_cast<uint8_t*>(&metadata_string[0]), length);
  if (err != 0) {
    LOG(ERROR) << "Failed to extract " << METADATA_PATH << ": " << ErrorCodeString(err);
    return false;
  }

  for (const std::string& line : android::base::Split(metadata_string, "\n")) {
    size_t eq = line.find('=');
    if (eq != std::string::npos) {
      metadata->emplace(android::base::Trim(line.substr(0, eq)),
                        android::base::Trim(line.substr(eq + 1)));
    }
  }

  return true;
}

// Gets the value for the given key in |metadata|. Returns an emtpy string if the key isn't
// present.
static std::string get_value(const std::map<std::string, std::string>& metadata,
                             const std::string& key) {
  const auto& it = metadata.find(key);
  return (it == metadata.end()) ? "" : it->second;
}

static std::string OtaTypeToString(OtaType type) {
  switch (type) {
    case OtaType::AB:
      return "AB";
    case OtaType::BLOCK:
      return "BLOCK";
    case OtaType::BRICK:
      return "BRICK";
  }
}

// Read the build.version.incremental of src/tgt from the metadata and log it to last_install.
static void ReadSourceTargetBuild(const std::map<std::string, std::string>& metadata,
                                  std::vector<std::string>* log_buffer) {
  // Examples of the pre-build and post-build strings in metadata:
  //   pre-build-incremental=2943039
  //   post-build-incremental=2951741
  auto source_build = get_value(metadata, "pre-build-incremental");
  if (!source_build.empty()) {
    log_buffer->push_back("source_build: " + source_build);
  }

  auto target_build = get_value(metadata, "post-build-incremental");
  if (!target_build.empty()) {
    log_buffer->push_back("target_build: " + target_build);
  }
}

// Checks the build version, fingerprint and timestamp in the metadata of the A/B package.
// Downgrading is not allowed unless explicitly enabled in the package and only for
// incremental packages.
static bool CheckAbSpecificMetadata(const std::map<std::string, std::string>& metadata,
                                    RecoveryUI* ui) {
  // Incremental updates should match the current build.
  auto device_pre_build = android::base::GetProperty("ro.build.version.incremental", "");
  auto pkg_pre_build = get_value(metadata, "pre-build-incremental");
  if (!pkg_pre_build.empty() && pkg_pre_build != device_pre_build) {
    LOG(ERROR) << "Package is for source build " << pkg_pre_build << " but expected "
               << device_pre_build;
    return false;
  }

  auto device_fingerprint = android::base::GetProperty("ro.build.fingerprint", "");
  auto pkg_pre_build_fingerprint = get_value(metadata, "pre-build");
  if (!pkg_pre_build_fingerprint.empty() &&
      !isInStringList(device_fingerprint, pkg_pre_build_fingerprint, FINGERPRING_SEPARATOR)) {
    LOG(ERROR) << "Package is for source build " << pkg_pre_build_fingerprint << " but expected "
               << device_fingerprint;
    return false;
  }

  // Check for downgrade version.
  bool undeclared_downgrade = false;
  int64_t build_timestamp =
      android::base::GetIntProperty("ro.build.date.utc", std::numeric_limits<int64_t>::max());
  int64_t pkg_post_timestamp = 0;
  // We allow to full update to the same version we are running, in case there
  // is a problem with the current copy of that version.
  auto pkg_post_timestamp_string = get_value(metadata, "post-timestamp");
  if (pkg_post_timestamp_string.empty() ||
      !android::base::ParseInt(pkg_post_timestamp_string, &pkg_post_timestamp) ||
      pkg_post_timestamp < build_timestamp) {
    if (get_value(metadata, "ota-downgrade") != "yes") {
      LOG(ERROR) << "Update package is older than the current build, expected a build "
                    "newer than timestamp "
                 << build_timestamp << " but package has timestamp " << pkg_post_timestamp
                 << " and downgrade not allowed.";
      undeclared_downgrade = true;
    } else if (pkg_pre_build_fingerprint.empty()) {
      LOG(ERROR) << "Downgrade package must have a pre-build version set, not allowed.";
      undeclared_downgrade = true;
    }
  }
  const auto post_build = get_value(metadata, "post-build");
  const auto build_fingerprint = android::base::Tokenize(post_build, "/");
  if (!build_fingerprint.empty() && android::base::GetProperty("ro.build.type", "") == "user") {
    const auto& post_build_tag = build_fingerprint.back();
    const auto build_tag = android::base::GetProperty("ro.build.tags", "");
    if (build_tag != post_build_tag) {
      LOG(ERROR) << "Post build-tag " << post_build_tag << " does not match device build tag "
                 << build_tag;
      return false;
    }
  }

  if (undeclared_downgrade &&
      !(ui->IsTextVisible() && ask_to_continue_downgrade(ui->GetDevice()))) {
    return false;
  }

  return true;
}

bool CheckPackageMetadata(const std::map<std::string, std::string>& metadata, OtaType ota_type,
                          RecoveryUI* ui) {
  auto package_ota_type = get_value(metadata, "ota-type");
  auto expected_ota_type = OtaTypeToString(ota_type);
  if (ota_type != OtaType::AB && ota_type != OtaType::BRICK) {
    LOG(INFO) << "Skip package metadata check for ota type " << expected_ota_type;
    return true;
  }

  if (package_ota_type != expected_ota_type) {
    LOG(ERROR) << "Unexpected ota package type, expects " << expected_ota_type << ", actual "
               << package_ota_type;
    return false;
  }

  auto device = android::base::GetProperty("ro.product.device", "");
  auto pkg_device = get_value(metadata, "pre-device");
  // device name can be a | separated list, so need to check
  if (pkg_device.empty() || !isInStringList(device, pkg_device, FINGERPRING_SEPARATOR ":" ",")) {
    LOG(ERROR) << "Package is for product " << pkg_device << " but expected " << device;
    return false;
  }

  // We allow the package to not have any serialno; and we also allow it to carry multiple serial
  // numbers split by "|"; e.g. serialno=serialno1|serialno2|serialno3 ... We will fail the
  // verification if the device's serialno doesn't match any of these carried numbers.

  auto pkg_serial_no = get_value(metadata, "serialno");
  if (!pkg_serial_no.empty()) {
    auto device_serial_no = android::base::GetProperty("ro.serialno", "");
    bool serial_number_match = false;
    for (const auto& number : android::base::Split(pkg_serial_no, "|")) {
      if (device_serial_no == android::base::Trim(number)) {
        serial_number_match = true;
      }
    }
    if (!serial_number_match) {
      LOG(ERROR) << "Package is for serial " << pkg_serial_no;
      return false;
    }
  } else if (ota_type == OtaType::BRICK) {
    const auto device_build_tag = android::base::GetProperty("ro.build.tags", "");
    if (device_build_tag.empty()) {
      LOG(ERROR) << "Unable to determine device build tags, serial number is missing from package. "
                    "Rejecting the brick OTA package.";
      return false;
    }
    if (device_build_tag == RELEASE_KEYS_TAG) {
      LOG(ERROR) << "Device is release key build, serial number is missing from package. "
                    "Rejecting the brick OTA package.";
      return false;
    }
    LOG(INFO)
        << "Serial number is missing from brick OTA package, permitting anyway because device is "
        << device_build_tag;
  }

  if (ota_type == OtaType::AB) {
    return CheckAbSpecificMetadata(metadata, ui);
  }

  return true;
}

static std::string ExtractPayloadProperties(ZipArchiveHandle zip) {
  // For A/B updates we extract the payload properties to a buffer and obtain the RAW payload offset
  // in the zip file.
  static constexpr const char* AB_OTA_PAYLOAD_PROPERTIES = "payload_properties.txt";
  ZipEntry64 properties_entry;
  if (FindEntry(zip, AB_OTA_PAYLOAD_PROPERTIES, &properties_entry) != 0) {
    return {};
  }
  auto properties_entry_length = properties_entry.uncompressed_length;
  if (properties_entry_length > std::numeric_limits<size_t>::max()) {
    LOG(ERROR) << "Failed to extract " << AB_OTA_PAYLOAD_PROPERTIES
               << " because's uncompressed size exceeds size of address space. "
               << properties_entry_length;
    return {};
  }
  std::string payload_properties(properties_entry_length, '\0');
  int32_t err =
      ExtractToMemory(zip, &properties_entry, reinterpret_cast<uint8_t*>(payload_properties.data()),
                      properties_entry_length);
  if (err != 0) {
    LOG(ERROR) << "Failed to extract " << AB_OTA_PAYLOAD_PROPERTIES << ": " << ErrorCodeString(err);
    return {};
  }
  return payload_properties;
}

bool SetUpAbUpdateCommands(const std::string& package, ZipArchiveHandle zip, int status_fd,
                           std::vector<std::string>* cmd) {
  CHECK(cmd != nullptr);

  // For A/B updates we extract the payload properties to a buffer and obtain the RAW payload offset
  // in the zip file.
  const auto payload_properties = ExtractPayloadProperties(zip);
  if (payload_properties.empty()) {
    return false;
  }

  static constexpr const char* AB_OTA_PAYLOAD = "payload.bin";
  ZipEntry64 payload_entry;
  if (FindEntry(zip, AB_OTA_PAYLOAD, &payload_entry) != 0) {
    LOG(ERROR) << "Failed to find " << AB_OTA_PAYLOAD;
    return false;
  }
  long payload_offset = payload_entry.offset;
  *cmd = {
    "/system/bin/update_engine_sideload",
    "--payload=file://" + package,
    android::base::StringPrintf("--offset=%ld", payload_offset),
    "--headers=" + std::string(payload_properties.begin(), payload_properties.end()),
    android::base::StringPrintf("--status_fd=%d", status_fd),
  };
  return true;
}

bool SetUpNonAbUpdateCommands(const std::string& package, ZipArchiveHandle zip, int retry_count,
                              int status_fd, std::vector<std::string>* cmd) {
  CHECK(cmd != nullptr);

  // In non-A/B updates we extract the update binary from the package.
  static constexpr const char* UPDATE_BINARY_NAME = "META-INF/com/google/android/update-binary";
  ZipEntry64 binary_entry;
  if (FindEntry(zip, UPDATE_BINARY_NAME, &binary_entry) != 0) {
    LOG(ERROR) << "Failed to find update binary " << UPDATE_BINARY_NAME;
    return false;
  }

  const std::string binary_path = Paths::Get().temporary_update_binary();
  unlink(binary_path.c_str());
  android::base::unique_fd fd(
      open(binary_path.c_str(), O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0755));
  if (fd == -1) {
    PLOG(ERROR) << "Failed to create " << binary_path;
    return false;
  }

  if (auto error = ExtractEntryToFile(zip, &binary_entry, fd); error != 0) {
    LOG(ERROR) << "Failed to extract " << UPDATE_BINARY_NAME << ": " << ErrorCodeString(error);
    return false;
  }

  // When executing the update binary contained in the package, the arguments passed are:
  //   - the version number for this interface
  //   - an FD to which the program can write in order to update the progress bar.
  //   - the name of the package zip file.
  //   - an optional argument "retry" if this update is a retry of a failed update attempt.
  *cmd = {
    binary_path,
    std::to_string(kRecoveryApiVersion),
    std::to_string(status_fd),
    package,
  };
  if (retry_count > 0) {
    cmd->push_back("retry");
  }
  return true;
}

static void log_max_temperature(int* max_temperature, const std::atomic<bool>& logger_finished) {
  CHECK(max_temperature != nullptr);
  std::mutex mtx;
  std::unique_lock<std::mutex> lck(mtx);
  while (!logger_finished.load() &&
         finish_log_temperature.wait_for(lck, 20s) == std::cv_status::timeout) {
    *max_temperature = std::max(*max_temperature, GetMaxValueFromThermalZone());
  }
}

static bool PerformPowerwashIfRequired(ZipArchiveHandle zip, Device *device) {
  const auto payload_properties = ExtractPayloadProperties(zip);
  if (payload_properties.find("POWERWASH=1") != std::string::npos) {
    LOG(INFO) << "Payload properties has POWERWASH=1, wiping userdata...";
    return WipeData(device);
  }
  return true;
}

// If the package contains an update binary, extract it and run it.
static InstallResult TryUpdateBinary(Package* package, bool* wipe_cache,
                                     std::vector<std::string>* log_buffer, int retry_count,
                                     int* max_temperature, Device* device) {
  auto ui = device->GetUI();
  std::map<std::string, std::string> metadata;
  auto zip = package->GetZipArchiveHandle();
  bool has_metadata = ReadMetadataFromPackage(zip, &metadata);

  const bool package_is_ab = has_metadata && get_value(metadata, "ota-type") == OtaTypeToString(OtaType::AB);
  const bool package_is_brick = get_value(metadata, "ota-type") == OtaTypeToString(OtaType::BRICK);
  if (package_is_brick) {
    LOG(INFO) << "Installing a brick package";
    if (package->GetType() == PackageType::kFile &&
        package->GetPackageSize() < MEMORY_PACKAGE_LIMIT) {
      std::vector<uint8_t> content(package->GetPackageSize());
      if (package->ReadFullyAtOffset(content.data(), content.size(), 0)) {
        auto memory_package = Package::CreateMemoryPackage(std::move(content), {});
        return WipeAbDevice(device, memory_package.get()) ? INSTALL_SUCCESS : INSTALL_ERROR;
      }
    }
    return WipeAbDevice(device, package) ? INSTALL_SUCCESS : INSTALL_ERROR;
  }
  bool device_supports_ab = android::base::GetBoolProperty("ro.build.ab_update", false);
  bool ab_device_supports_nonab = true;
  bool device_only_supports_ab = device_supports_ab && !ab_device_supports_nonab;
  bool device_supports_virtual_ab = android::base::GetBoolProperty("ro.virtual_ab.enabled", false);

  const auto current_spl = android::base::GetProperty("ro.build.version.security_patch", "");
  if (ViolatesSPLDowngrade(zip, current_spl)) {
    LOG(ERROR) << "Denying OTA because it's SPL downgrade";
    return INSTALL_ERROR;
  }

  const auto reboot_to_recovery = [] {
    if (std::string err; !clear_bootloader_message(&err)) {
      LOG(ERROR) << "Failed to clear BCB message: " << err;
    }
    Reboot("recovery");
  };

  static bool ab_package_installed = false;
  if (ab_package_installed) {
    if (ask_to_ab_reboot(device)) {
      reboot_to_recovery();
    }
    return INSTALL_ERROR;
  }

  if (package_is_ab) {
    CHECK(package->GetType() == PackageType::kFile);
  }

  // Verify against the metadata in the package first. Expects A/B metadata if:
  // Package declares itself as an A/B package
  // Package does not declare itself as an A/B package, but device only supports A/B;
  //   still calls CheckPackageMetadata to get a meaningful error message.
  if (package_is_ab || device_only_supports_ab) {
    if (!CheckPackageMetadata(metadata, OtaType::AB, ui)) {
      log_buffer->push_back(android::base::StringPrintf("error: %d", kUpdateBinaryCommandFailure));
      return INSTALL_ERROR;
    }
  }

  if (!package_is_ab && !logical_partitions_mapped()) {
    CreateSnapshotPartitions();
    map_logical_partitions();
  } else if (package_is_ab && device_supports_virtual_ab && logical_partitions_mapped()) {
    LOG(ERROR) << "Logical partitions are mapped. "
               << "Please reboot recovery before installing an OTA update.";
    return INSTALL_ERROR;
  }

  ReadSourceTargetBuild(metadata, log_buffer);

  // The updater in child process writes to the pipe to communicate with recovery.
  android::base::unique_fd pipe_read, pipe_write;
  // Explicitly disable O_CLOEXEC using 0 as the flags (last) parameter to Pipe
  // so that the child updater process will recieve a non-closed fd.
  if (!android::base::Pipe(&pipe_read, &pipe_write, 0)) {
    PLOG(ERROR) << "Failed to create pipe for updater-recovery communication";
    return INSTALL_CORRUPT;
  }

  // The updater-recovery communication protocol.
  //
  //   progress <frac> <secs>
  //       fill up the next <frac> part of of the progress bar over <secs> seconds. If <secs> is
  //       zero, use `set_progress` commands to manually control the progress of this segment of the
  //       bar.
  //
  //   set_progress <frac>
  //       <frac> should be between 0.0 and 1.0; sets the progress bar within the segment defined by
  //       the most recent progress command.
  //
  //   ui_print <string>
  //       display <string> on the screen.
  //
  //   wipe_cache
  //       a wipe of cache will be performed following a successful installation.
  //
  //   clear_display
  //       turn off the text display.
  //
  //   enable_reboot
  //       packages can explicitly request that they want the user to be able to reboot during
  //       installation (useful for debugging packages that don't exit).
  //
  //   retry_update
  //       updater encounters some issue during the update. It requests a reboot to retry the same
  //       package automatically.
  //
  //   log <string>
  //       updater requests logging the string (e.g. cause of the failure).
  //

  std::string package_path = package->GetPath();

  std::vector<std::string> args;
  if (auto setup_result =
          package_is_ab
              ? SetUpAbUpdateCommands(package_path, zip, pipe_write.get(), &args)
              : SetUpNonAbUpdateCommands(package_path, zip, retry_count, pipe_write.get(), &args);
      !setup_result) {
    log_buffer->push_back(android::base::StringPrintf("error: %d", kUpdateBinaryCommandFailure));
    return INSTALL_CORRUPT;
  }

  pid_t pid = fork();
  if (pid == -1) {
    PLOG(ERROR) << "Failed to fork update binary";
    log_buffer->push_back(android::base::StringPrintf("error: %d", kForkUpdateBinaryFailure));
    return INSTALL_ERROR;
  }

  if (pid == 0) {
    umask(022);
    pipe_read.reset();

    // Convert the std::string vector to a NULL-terminated char* vector suitable for execv.
    auto chr_args = StringVectorToNullTerminatedArray(args);
    execv(chr_args[0], chr_args.data());
    // We shouldn't use LOG/PLOG in the forked process, since they may cause the child process to
    // hang. This deadlock results from an improperly copied mutex in the ui functions.
    // (Bug: 34769056)
    fprintf(stdout, "E:Can't run %s (%s)\n", chr_args[0], strerror(errno));
    _exit(EXIT_FAILURE);
  }
  pipe_write.reset();

  std::atomic<bool> logger_finished(false);
  std::thread temperature_logger(log_max_temperature, max_temperature, std::ref(logger_finished));

  *wipe_cache = false;
  bool retry_update = false;

  char buffer[1024];
  FILE* from_child = android::base::Fdopen(std::move(pipe_read), "r");
  while (fgets(buffer, sizeof(buffer), from_child) != nullptr) {
    std::string line(buffer);
    size_t space = line.find_first_of(" \n");
    std::string command(line.substr(0, space));
    if (command.empty()) continue;

    // Get rid of the leading and trailing space and/or newline.
    std::string args = space == std::string::npos ? "" : android::base::Trim(line.substr(space));

    if (command == "progress") {
      std::vector<std::string> tokens = android::base::Split(args, " ");
      double fraction;
      int seconds;
      if (tokens.size() == 2 && android::base::ParseDouble(tokens[0].c_str(), &fraction) &&
          android::base::ParseInt(tokens[1], &seconds)) {
        ui->ShowProgress(fraction * (1 - VERIFICATION_PROGRESS_FRACTION), seconds);
      } else {
        LOG(ERROR) << "invalid \"progress\" parameters: " << line;
      }
    } else if (command == "set_progress") {
      std::vector<std::string> tokens = android::base::Split(args, " ");
      double fraction;
      if (tokens.size() == 1 && android::base::ParseDouble(tokens[0].c_str(), &fraction)) {
        ui->SetProgress(fraction);
      } else {
        LOG(ERROR) << "invalid \"set_progress\" parameters: " << line;
      }
    } else if (command == "ui_print") {
      ui->PrintOnScreenOnly("%s\n", args.c_str());
      fflush(stdout);
    } else if (command == "wipe_cache") {
      *wipe_cache = true;
    } else if (command == "clear_display") {
      ui->SetBackground(RecoveryUI::NONE);
    } else if (command == "enable_reboot") {
      // packages can explicitly request that they want the user
      // to be able to reboot during installation (useful for
      // debugging packages that don't exit).
      ui->SetEnableReboot(true);
    } else if (command == "retry_update") {
      retry_update = true;
    } else if (command == "log") {
      if (!args.empty()) {
        // Save the logging request from updater and write to last_install later.
        log_buffer->push_back(args);
      } else {
        LOG(ERROR) << "invalid \"log\" parameters: " << line;
      }
    } else {
      LOG(ERROR) << "unknown command [" << command << "]";
    }
  }
  fclose(from_child);

  int status;
  waitpid(pid, &status, 0);

  logger_finished.store(true);
  finish_log_temperature.notify_one();
  temperature_logger.join();

  if (retry_update) {
    return INSTALL_RETRY;
  }
  if (WIFEXITED(status)) {
    if (WEXITSTATUS(status) != EXIT_SUCCESS) {
      LOG(ERROR) << "Error in " << package_path << " (status " << WEXITSTATUS(status) << ")";
      return INSTALL_ERROR;
    }
  } else if (WIFSIGNALED(status)) {
    LOG(ERROR) << "Error in " << package_path << " (killed by signal " << WTERMSIG(status) << ")";
    return INSTALL_ERROR;
  } else {
    LOG(FATAL) << "Invalid status code " << status;
  }
  if (package_is_ab) {
    ab_package_installed = true;
    PerformPowerwashIfRequired(zip, device);
    if (!ui->IsSideloadAutoReboot() && ask_to_ab_reboot(device)) {
      reboot_to_recovery();
    }
  }

  return INSTALL_SUCCESS;
}

static InstallResult VerifyAndInstallPackage(Package* package, bool* wipe_cache,
                                             std::vector<std::string>* log_buffer, int retry_count,
                                             int* max_temperature, Device* device) {
  auto ui = device->GetUI();
  ui->SetBackground(RecoveryUI::INSTALLING_UPDATE);
  // Give verification half the progress bar...
  ui->SetProgressType(RecoveryUI::DETERMINATE);
  ui->ShowProgress(VERIFICATION_PROGRESS_FRACTION, VERIFICATION_PROGRESS_TIME);

  // Verify package.
  if (!verify_package(package, ui)) {
    log_buffer->push_back(android::base::StringPrintf("error: %d", kZipVerificationFailure));
    if (!ui->IsTextVisible() || !ask_to_continue_unverified(ui->GetDevice())) {
        return INSTALL_CORRUPT;
    }
  }

  // Verify and install the contents of the package.
  ui->Print("Installing update...\n");
  if (retry_count > 0) {
    ui->Print("Retry attempt: %d\n", retry_count);
  }
  ui->SetEnableReboot(false);
  auto result =
      TryUpdateBinary(package, wipe_cache, log_buffer, retry_count, max_temperature, device);
  ui->SetEnableReboot(true);
  ui->Print("\n");

  return result;
}

InstallResult InstallPackage(Package* package, const std::string_view package_id,
                             bool should_wipe_cache, int retry_count, Device* device) {
  auto ui = device->GetUI();
  auto start = std::chrono::system_clock::now();

  int start_temperature = GetMaxValueFromThermalZone();
  int max_temperature = start_temperature;

  InstallResult result;
  std::vector<std::string> log_buffer;

  ui->Print("Supported API: %d\n", kRecoveryApiVersion);

  ui->Print("Finding update package...\n");
  LOG(INFO) << "Update package id: " << package_id;
  if (!package) {
    log_buffer.push_back(android::base::StringPrintf("error: %d", kMapFileFailure));
    result = INSTALL_CORRUPT;
  } else if (setup_install_mounts() != 0) {
    LOG(ERROR) << "failed to set up expected mounts for install; aborting";
    result = INSTALL_ERROR;
  } else {
    bool updater_wipe_cache = false;
    result = VerifyAndInstallPackage(package, &updater_wipe_cache, &log_buffer, retry_count,
                                     &max_temperature, device);
    should_wipe_cache = should_wipe_cache || updater_wipe_cache;
  }

  // Measure the time spent to apply OTA update in seconds.
  std::chrono::duration<double> duration = std::chrono::system_clock::now() - start;
  int time_total = static_cast<int>(duration.count());

  bool has_cache = volume_for_mount_point("/cache") != nullptr;
  // Skip logging the uncrypt_status on devices without /cache.
  if (has_cache) {
    static constexpr const char* UNCRYPT_STATUS = "/cache/recovery/uncrypt_status";
    if (ensure_path_mounted(UNCRYPT_STATUS) != 0) {
      LOG(WARNING) << "Can't mount " << UNCRYPT_STATUS;
    } else {
      std::string uncrypt_status;
      if (!android::base::ReadFileToString(UNCRYPT_STATUS, &uncrypt_status)) {
        PLOG(WARNING) << "failed to read uncrypt status";
      } else if (!android::base::StartsWith(uncrypt_status, "uncrypt_")) {
        LOG(WARNING) << "corrupted uncrypt_status: " << uncrypt_status;
      } else {
        log_buffer.push_back(android::base::Trim(uncrypt_status));
      }
    }
  }

  // The first two lines need to be the package name and install result.
  std::vector<std::string> log_header = {
    std::string(package_id),
    result == INSTALL_SUCCESS ? "1" : "0",
    "time_total: " + std::to_string(time_total),
    "retry: " + std::to_string(retry_count),
  };

  int end_temperature = GetMaxValueFromThermalZone();
  max_temperature = std::max(end_temperature, max_temperature);
  if (start_temperature > 0) {
    log_buffer.push_back("temperature_start: " + std::to_string(start_temperature));
  }
  if (end_temperature > 0) {
    log_buffer.push_back("temperature_end: " + std::to_string(end_temperature));
  }
  if (max_temperature > 0) {
    log_buffer.push_back("temperature_max: " + std::to_string(max_temperature));
  }

  std::string log_content =
      android::base::Join(log_header, "\n") + "\n" + android::base::Join(log_buffer, "\n") + "\n";
  const std::string& install_file = Paths::Get().temporary_install_file();
  if (!android::base::WriteStringToFile(log_content, install_file)) {
    PLOG(ERROR) << "failed to write " << install_file;
  }

  // Write a copy into last_log.
  LOG(INFO) << log_content;

  if (result == INSTALL_SUCCESS && should_wipe_cache) {
    if (!WipeCache(ui, nullptr)) {
      result = INSTALL_ERROR;
    }
  }

  return result;
}

bool verify_package(Package* package, RecoveryUI* ui) {
  static constexpr const char* CERTIFICATE_ZIP_FILE = "/system/etc/security/otacerts.zip";
  std::vector<Certificate> loaded_keys = LoadKeysFromZipfile(CERTIFICATE_ZIP_FILE);
  if (loaded_keys.empty()) {
    LOG(ERROR) << "Failed to load keys";
    return false;
  }
  LOG(INFO) << loaded_keys.size() << " key(s) loaded from " << CERTIFICATE_ZIP_FILE;

  // Verify package.
  ui->Print("Verifying update package...\n");
  auto t0 = std::chrono::system_clock::now();
  int err = verify_file(package, loaded_keys);
  std::chrono::duration<double> duration = std::chrono::system_clock::now() - t0;
  ui->Print("Update package verification took %.1f s (result %d).\n", duration.count(), err);
  if (err != VERIFY_SUCCESS) {
    LOG(ERROR) << "Signature verification failed";
    LOG(ERROR) << "error: " << kZipVerificationFailure;
    return false;
  }
  return true;
}

bool SetupPackageMount(const std::string& package_path, bool* should_use_fuse) {
  CHECK(should_use_fuse != nullptr);

  if (package_path.empty()) {
    return false;
  }

  *should_use_fuse = true;
  if (package_path[0] == '@') {
    auto block_map_path = package_path.substr(1);
    if (ensure_path_mounted(block_map_path) != 0) {
      LOG(ERROR) << "Failed to mount " << block_map_path;
      return false;
    }
    // uncrypt only produces block map only if the package stays on /data.
    *should_use_fuse = false;
    return true;
  }

  // Package is not a block map file.
  if (ensure_path_mounted(package_path) != 0) {
    LOG(ERROR) << "Failed to mount " << package_path;
    return false;
  }

  // Reject the package if the input path doesn't equal the canonicalized path.
  // e.g. /cache/../sdcard/update_package.
  std::error_code ec;
  auto canonical_path = std::filesystem::canonical(package_path, ec);
  if (ec) {
    LOG(ERROR) << "Failed to get canonical of " << package_path << ", " << ec.message();
    return false;
  }
  if (canonical_path.string() != package_path) {
    LOG(ERROR) << "Installation aborts. The canonical path " << canonical_path.string()
               << " doesn't equal the original path " << package_path;
    return false;
  }

  constexpr const char* CACHE_ROOT = "/cache";
  if (android::base::StartsWith(package_path, CACHE_ROOT)) {
    *should_use_fuse = false;
  }
  return true;
}

// Check if `target_token` is in string `str_list`, where `str_list` is expected to be a
// list delimited by `deliminator`
// E.X. isInStringList("a", "a|b|c|d", "|") => true
// E.X. isInStringList("abc", "abc", "|") => true
static bool isInStringList(const std::string& target_token, const std::string& str_list,
                           const std::string& deliminator) {
  if (target_token.length() > str_list.length()) {
    return false;
  } else if (target_token.length() == str_list.length() || deliminator.length() == 0) {
    return target_token == str_list;
  }
  auto&& list = android::base::Split(str_list, deliminator);
  return std::find(list.begin(), list.end(), target_token) != list.end();
}
