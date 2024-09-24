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

#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android-base/strings.h>
#include <android-base/unique_fd.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <linux/if.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>

#include "recovery_ui/device.h"
#include "recovery_ui/ethernet_device.h"
#include "recovery_ui/ethernet_ui.h"

// Android TV defaults to eth0 for it's interface
EthernetDevice::EthernetDevice(EthernetRecoveryUI* ui) : EthernetDevice(ui, "eth0") {}

// Allow future users to define the interface as they prefer
EthernetDevice::EthernetDevice(EthernetRecoveryUI* ui, std::string interface)
    : Device(ui), ctl_sock_(socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)), interface_(interface) {
  if (ctl_sock_ < 0) {
    PLOG(ERROR) << "Failed to open socket";
  }
}

void EthernetDevice::InitDevice() {
  BringupInterface();
  sleep(1);
}

void EthernetDevice::PreRecovery() {
  SetTitleIPAddress(BringupInterface());
}

void EthernetDevice::PreFastboot() {
  android::base::SetProperty("fastbootd.protocol", "tcp");
  SetTitleIPAddress(BringupInterface());
}

bool EthernetDevice::BringupInterface() {
  if (SetInterfaceFlags(IFF_UP, 0) < 0) {
    LOG(ERROR) << "Failed to bring up interface";
    return false;
  }
  return true;
}

int EthernetDevice::SetInterfaceFlags(const unsigned set, const unsigned clr) {
  struct ifreq ifr;

  if (ctl_sock_ < 0) {
    return -1;
  }

  memset(&ifr, 0, sizeof(struct ifreq));
  strncpy(ifr.ifr_name, interface_.c_str(), IFNAMSIZ);
  ifr.ifr_name[IFNAMSIZ - 1] = 0;

  if (ioctl(ctl_sock_, SIOCGIFFLAGS, &ifr) < 0) {
    PLOG(ERROR) << "Failed to get interface active flags";
    return -1;
  }
  ifr.ifr_flags = (ifr.ifr_flags & (~clr)) | set;

  if (ioctl(ctl_sock_, SIOCSIFFLAGS, &ifr) < 0) {
    PLOG(ERROR) << "Failed to set interface active flags";
    return -1;
  }

  return 0;
}

void EthernetDevice::SetTitleIPAddress(const bool interface_up) {
  auto recovery_ui = reinterpret_cast<EthernetRecoveryUI*>(GetUI());

  // Cached IP Addresses needs to be cleared anyways, no matter if errored or not
  recovery_ui->ClearIPAddresses();

  if (!interface_up) return;

  struct ifaddrs* ifaddr;
  if (getifaddrs(&ifaddr) == -1) {
    PLOG(ERROR) << "Failed to get interface addresses";
    return;
  }

  std::unique_ptr<struct ifaddrs, decltype(&freeifaddrs)> guard{ ifaddr, freeifaddrs };
  for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
    if (interface_ != ifa->ifa_name)
      continue;

    if (ifa->ifa_addr->sa_family == AF_INET) {
      auto current_addr = reinterpret_cast<struct sockaddr_in*>(ifa->ifa_addr);

      char addrstr[INET_ADDRSTRLEN];
      inet_ntop(AF_INET, reinterpret_cast<const void*>(&current_addr->sin_addr), addrstr,
                INET_ADDRSTRLEN);
      LOG(INFO) << "Our IPv4 address is " << addrstr;
      recovery_ui->AddIPv4Address(addrstr);
      continue;
    } else if (ifa->ifa_addr->sa_family == AF_INET6) {
      auto current_addr = reinterpret_cast<struct sockaddr_in6*>(ifa->ifa_addr);
      if (!IN6_IS_ADDR_LINKLOCAL(&(current_addr->sin6_addr))) {
        continue;
      }

      char addrstr[INET6_ADDRSTRLEN];
      inet_ntop(AF_INET6, reinterpret_cast<const void*>(&current_addr->sin6_addr), addrstr,
                INET6_ADDRSTRLEN);
      LOG(INFO) << "Our IPv6 link-local address is " << addrstr;
      recovery_ui->SetIPv6LinkLocalAddress(addrstr);
      continue;
    }
  }
}
