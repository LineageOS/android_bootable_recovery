/*
 * Copyright (C) 2020 The Android Open Source Project
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

#include "recovery_ui/ethernet_ui.h"

#include <android-base/logging.h>

void EthernetRecoveryUI::SetTitle(const std::vector<std::string>& lines) {
  ScreenRecoveryUI::SetTitle(lines);

  // Append IP address, if any
  for (auto it = ipv4_addresses_.begin(); it != ipv4_addresses_.end(); ++it) {
    title_lines_.push_back("IPv4 address - " + *it);
  }
  if (!ipv6_linklocal_address_.empty()) {
    title_lines_.push_back("IPv6 link-local address - " + ipv6_linklocal_address_);
  }
}

void EthernetRecoveryUI::AddIPv4Address(const std::string& address) {
  ipv4_addresses_.push_back(address);
}

void EthernetRecoveryUI::ClearIPAddresses() {
  ipv4_addresses_.clear();
  ipv6_linklocal_address_.clear();
}

void EthernetRecoveryUI::SetIPv6LinkLocalAddress(const std::string& address) {
  ipv6_linklocal_address_ = address;
}
