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

#ifndef _FSUPDATER_H_
#define _FSUPDATER_H_

#include <stdio.h>

#include <string>

#include <ziparchive/zip_archive.h>


// This should match UpdaterInfo
typedef struct {
    FILE* cmd_pipe;
    ZipArchiveHandle package_zip;
    int version;

    uint8_t* package_zip_addr;
    size_t package_zip_len;
} FsUpdaterInfo;

/*
 * In strict mode, all operations must succeed:
 *  - Created files and directories must not exist.
 *  - Deleted files and directories must exist.
 *  - Patches must apply.
 *
 * In lenient mode:
 *  - Created files and directories may exist.
 *  - Deleted files and directories may not exist.
 *  - Patches may not apply.
 *
 * In both modes, genuine errors will always be reported.
 */
enum FsUpdaterMode : int {
  kFsUpdaterModeStrict,
  kFsUpdaterModeLenient,
};

void RegisterFsUpdaterFunctions();
void SetFsUpdaterRoot(const std::string& src_root, const std::string& tgt_root);
void SetFsUpdaterMountContext(const std::string& context);
void SetFsUpdaterMode(FsUpdaterMode mode);

#endif // _FSUPDATER_H_
