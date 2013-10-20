/*
 * Copyright (C) 2013 The CyanogenMod Project
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

#ifndef _RECOVERY_CMDS_H
#define _RECOVERY_CMDS_H

#ifdef __cplusplus
extern "C" {
#endif

int reboot_main(int argc, char** argv);
int poweroff_main(int argc, char** argv);
int start_main(int argc, char** argv);
int stop_main(int argc, char** argv);
int mksh_main(int argc, char** argv);

#ifdef __cplusplus
}
#endif

#endif
