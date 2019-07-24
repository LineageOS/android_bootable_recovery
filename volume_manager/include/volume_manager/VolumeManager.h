/*
 * Copyright (C) 2008 The Android Open Source Project
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

#ifndef _VOLMGR_VOLUME_MANAGER_H
#define _VOLMGR_VOLUME_MANAGER_H

#include <fnmatch.h>
#include <pthread.h>
#include <stdlib.h>

#include <list>
#include <mutex>
#include <string>

struct selabel_handle;
class NetlinkManager;
class NetlinkEvent;

class VolumeWatcher {
  public:
    virtual ~VolumeWatcher(void) {}
    virtual void handleEvent(int code, const std::vector<std::string>& argv) = 0;
};

namespace android {
namespace volmgr {

class Disk;
class VolumeBase;

class VolumeInfo {
  public:
    explicit VolumeInfo(const VolumeBase* vol);

    std::string mId;
    std::string mLabel;
    std::string mPath;
    bool        mMountable;
};

class VolumeManager {
  private:
    VolumeManager(const VolumeManager&);
    VolumeManager& operator=(const VolumeManager&);

  public:
    static VolumeManager* Instance(void);

  private:
    static VolumeManager* sInstance;

  public:
    class DiskSource {
      public:
        DiskSource(const std::string& sysPattern, const std::string& nickname, int partnum,
                   int flags, const std::string& fstype, const std::string mntopts)
            : mSysPattern(sysPattern),
              mNickname(nickname),
              mPartNum(partnum),
              mFlags(flags),
              mFsType(fstype),
              mMntOpts(mntopts) {}

        bool matches(const std::string& sysPath) {
            return !fnmatch(mSysPattern.c_str(), sysPath.c_str(), 0);
        }

        const std::string& getNickname() { return mNickname; }
        int getPartNum() { return mPartNum; }
        int getFlags() { return mFlags; }
        const std::string& getFsType() { return mFsType; }
        const std::string& getMntOpts() { return mMntOpts; }

      private:
        std::string mSysPattern;
        std::string mNickname;
        int mPartNum;
        int mFlags;
        std::string mFsType;
        std::string mMntOpts;
    };

  public:
    VolumeManager(void);
    ~VolumeManager(void);

    bool start(VolumeWatcher* watcher);
    void stop(void);

    bool reset(void);
    bool unmountAll(void);

    void getVolumeInfo(std::vector<VolumeInfo>& info);

    VolumeBase* findVolume(const std::string& id);

    bool volumeMount(const std::string& id);
    bool volumeUnmount(const std::string& id, bool detach = false);
    bool volumeFormat(const std::string& id, const std::string& fsType);

  public:
    void addDiskSource(DiskSource* source);
    void handleBlockEvent(NetlinkEvent* evt);

    void notifyEvent(int code);
    void notifyEvent(int code, const std::string& arg);
    void notifyEvent(int code, const std::vector<std::string>& argv);

  private:
    VolumeWatcher* mWatcher;
    NetlinkManager* mNetlinkManager;
    std::mutex mLock;
    VolumeBase* mInternalEmulated;
    std::list<DiskSource*> mDiskSources;
    std::list<Disk*> mDisks;
};

}  // namespace volmgr
}  // namespace android

#endif
