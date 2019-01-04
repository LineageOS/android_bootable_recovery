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

#include "volclient.h"
#include <volume_manager/ResponseCode.h>

void VolumeClient::handleEvent(int code, const std::vector<std::string>& argv) {
  // This client is only interested in volume addition/deletion
  if (code != ResponseCode::VolumeDestroyed &&
      code != ResponseCode::DiskScanned)
    return;

  printf("VolumeClient::handleEvent: code=%d, argv=<", code);
  for (auto& arg : argv) {
    printf("%s,", arg.c_str());
  }
  printf(">\n");

  mDevice->handleVolumeChanged();
}
