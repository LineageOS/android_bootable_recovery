/*
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

#ifndef _RECOVERY_VOLCLIENT_H
#define _RECOVERY_VOLCLIENT_H

#include <volume_manager/VolumeManager.h>
#include "recovery_ui/device.h"

class VolumeClient : public VolumeWatcher {
 public:
  VolumeClient(Device* device) : mDevice(device) {}
  virtual ~VolumeClient(void) {}
  virtual void handleEvent(int code, const std::vector<std::string>& argv);

 private:
  Device* mDevice;
};

#endif
