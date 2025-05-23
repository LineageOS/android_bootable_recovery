#include <selinux/selinux.h>

#include <filesystem>

extern bool SetUsbConfig(const std::string& state);

static constexpr char kUmsDir[] = "/config/usb_gadget/g1/functions/mass_storage.0";

static std::string umsBrowseDirectory(const std::string& path, Device* device, RecoveryUI* ui, bool is_browsing_bdev) {
  if (access(path.c_str(), R_OK | X_OK)) {
    PLOG(ERROR) << "error opening " << path;
    return "";
  }

  std::vector<std::string> dirs;
  std::vector<std::string> files;
  std::vector<std::string> entries{ "../" };  // "../" is always the first entry.

  for (const auto& entry : std::filesystem::directory_iterator(path)) {
    std::string name = entry.path().filename().string();
    if (name == "." || name == "..") continue;

    bool is_dir = false;
    bool is_file = false;

    if (entry.is_symlink()) {
      auto resolved = std::filesystem::read_symlink(entry.path());
      is_dir = std::filesystem::is_directory(resolved);
      is_file = std::filesystem::is_block_file(resolved) ||
        std::filesystem::is_regular_file(resolved);
    } else {
      is_dir = entry.is_directory();
      is_file = entry.is_block_file() || entry.is_regular_file();
    }

    if (is_dir) {
      dirs.push_back(name + "/");
    } else if (is_file) {
      if (is_browsing_bdev) {
        if (path == "/dev/block") {
          if (android::base::StartsWith(name, "loop") ||
              android::base::StartsWith(name, "ram") ||
              android::base::StartsWith(name, "zram")) {
            continue;
          }
        }
      } else {
        if (!android::base::EndsWithIgnoreCase(name, ".img") &&
            !android::base::EndsWithIgnoreCase(name, ".iso")) {
          continue;
        }
      }
      files.push_back(name);
    }
  }

  std::sort(dirs.begin(), dirs.end());
  std::sort(files.begin(), files.end());

  if (is_browsing_bdev) {
    entries.insert(entries.end(), dirs.begin(), dirs.end());
    entries.insert(entries.end(), files.begin(), files.end());
  } else {
    entries.insert(entries.end(), files.begin(), files.end());
    entries.insert(entries.end(), dirs.begin(), dirs.end());
  }

  std::vector<std::string> headers{ "Choose a file to expose:", path };

  size_t chosen_item = 0;
  while (true) {
    chosen_item = ui->ShowMenu(
        headers, entries, chosen_item, true,
        std::bind(&Device::HandleMenuKey, device, std::placeholders::_1, std::placeholders::_2));

    if (chosen_item == static_cast<size_t>(RecoveryUI::KeyError::INTERRUPTED)) return "";
    if (chosen_item == Device::kGoHome) return "@";
    if (chosen_item == Device::kGoBack || chosen_item == 0) return "";

    const std::string& item = entries[chosen_item];
    std::string new_path = path + "/" + item;

    if (new_path.back() == '/') {
      // Recurse down into a subdirectory.
      new_path.pop_back();
      std::string result = umsBrowseDirectory(new_path, device, ui, is_browsing_bdev);
      if (!result.empty()) return result;
    } else {
      return new_path;
    }
  }

  // Unreachable.
}

static bool umsSetParameter(std::string key, std::string value) {
  if (!android::base::WriteStringToFile(value, kUmsDir + std::string("/lun.0/") + key)) {
    LOG(ERROR) << "Failed to set USB Mass Storage parameter "
               << key << " to " << value;
    return false;
  }
  return true;
}

static bool umsToggleOptions(Device* device, std::map<std::string, bool>& options) {
  RecoveryUI* ui = device->GetUI();
  std::vector<std::string> headers{ "Toggle options" };

  while (true) {
    std::vector<std::string> items;

    items.push_back("Start");
    items.push_back(options["cdrom"] ? "Device type: [CD-ROM] Disk" : "Device type: CD-ROM [Disk]");
    items.push_back(options["nofua"] ? "Ignore FUA flag: Yes" : "Ignore FUA flag: No");
    items.push_back(options["ro"] ? "Permission: read-only" : "Permission: read-write");
    items.push_back(options["removable"] ? "Removable: Yes" : "Removable: No");

    int chosen = ui->ShowMenu(
      headers, items, 0, false,
      std::bind(&Device::HandleMenuKey, device,
        std::placeholders::_1, std::placeholders::_2),
      true /* refreshable */);

    switch (chosen) {
      case Device::kGoBack:
        return false;
      case Device::kRefresh:
        continue;
      case 1:
        options["cdrom"] = !options["cdrom"];
        continue;
      case 2:
        options["nofua"] = !options["nofua"];
        continue;
      case 3:
        options["ro"] = !options["ro"];
        continue;
      case 4:
        options["removable"] = !options["removable"];
        continue;
      default:
        break;
    }
    break;
  }

  // The other parameters aren't writable if file is set
  umsSetParameter("file", "");
  for (const auto& [key, value] : options) {
    if (!umsSetParameter(key, value ? "1" : "0")) {
      return false;
    }
  }

  return true;
}

static void umsStart(Device* device, std::string path) {
  RecoveryUI* ui = device->GetUI();
  std::string original_usb_state = android::base::GetProperty("sys.usb.state", "");
  std::vector<std::string> headers;
  std::vector<std::string> items {
    "Stop",
    "Turn off display",
  };

  if (!SetUsbConfig("none")) {
    LOG(ERROR) << "Failed to set USB config to none";
    goto out;
  }
  if (!umsSetParameter("file", path)) {
    goto out;
  }
  if (!SetUsbConfig("mass_storage")) {
    LOG(ERROR) << "Failed to set USB config to mass_storage";
    goto out;
  }

  headers.push_back("Exposing " + path + " as USB Mass Storage");

  while (true) {
    int chosen = ui->ShowMenu(
      headers, items, 0, false,
      std::bind(&Device::HandleMenuKey, device, std::placeholders::_1, std::placeholders::_2),
      true /* refreshable */);

    switch (chosen) {
      case Device::kRefresh:
        continue;
      case 1:
        ui->SetScreensaverState(RecoveryUI::ScreensaverState::OFF);
        continue;
      default:
        break;
    }
    break;
  }

out:
  SetUsbConfig("none");
  SetUsbConfig(original_usb_state);
}

static void usb_mass_storage_menu(Device* device) {
  RecoveryUI* ui = device->GetUI();

  if (get_build_type() == "user") {
    ui->Print("Not supported on user builds\n");
    return;
  }

  int old_selinux = security_getenforce();
  security_setenforce(0); // Some block devices are read-only otherwise,
                          // even if we make kernel domain permissive

  std::vector<std::string> headers{ "USB Mass Storage" };
  std::vector<std::string> items;

  const int item_blockdev = 0;
  unsigned int non_storage_items;
  std::vector<VolumeInfo> volumes;
  std::string chosen_path;
  int chosen_volume_num;

  while (true) {
    chosen_volume_num = -1;
    non_storage_items = 1; // blockdev, at least

    items.clear();
    items.push_back("Choose block device");

    VolumeManager::Instance()->getVolumeInfo(volumes);
    for (auto vol = volumes.begin(); vol != volumes.end(); /* empty */) {
      if (!vol->mMountable) {
        vol = volumes.erase(vol);
        continue;
      }
      items.push_back("Choose image from " + vol->mLabel);
      ++vol;
    }

    int chosen = ui->ShowMenu(
      headers, items, 0, false,
      std::bind(&Device::HandleMenuKey, device, std::placeholders::_1, std::placeholders::_2),
      true /* refreshable */);
    if (chosen == Device::kRefresh) {
      continue;
    }
    if (chosen == Device::kGoBack) {
      break;
    }
    if (chosen == static_cast<size_t>(RecoveryUI::KeyError::INTERRUPTED)) {
      return;
    }

    std::map<std::string, bool> ums_options = {
      {"cdrom", false},
      {"nofua", true},
      {"removable", true},
      {"ro", false},
    };

    if (chosen == item_blockdev) {
      chosen_path = umsBrowseDirectory("/dev/block", device, ui, true);
      if (chosen_path.empty()) continue;
    } else {
      chosen_volume_num = chosen - non_storage_items;
      if (!VolumeManager::Instance()->volumeMount(volumes[chosen_volume_num].mId)) {
        continue;
      }
      chosen_path = umsBrowseDirectory(volumes[chosen_volume_num].mPath, device, ui, false);
      if (chosen_path.empty()) {
        VolumeManager::Instance()->volumeUnmount(volumes[chosen_volume_num].mId);
        continue;
      }

      if (android::base::EndsWithIgnoreCase(chosen_path, ".iso")) {
        ums_options["cdrom"] = true;
        ums_options["ro"] = true;
      }
    }

    if (!std::filesystem::create_directory(kUmsDir)) {
      LOG(ERROR) << "Failed to create directory " << kUmsDir;
    } else {
      if (umsToggleOptions(device, ums_options)) {
        umsStart(device, chosen_path);
      }
      if (!std::filesystem::remove(kUmsDir)) {
        LOG(ERROR) << "Failed to remove directory " << kUmsDir;
      }
    }

    if (chosen_volume_num >= 0)
      VolumeManager::Instance()->volumeUnmount(volumes[chosen_volume_num].mId);
  }

  if (old_selinux) {
    security_setenforce(old_selinux);
  }
}
