/*
 * Copyright (C) 2025 The Android Open Source Project
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 * without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef _BOOTLOADER_MESSAGE_CORE_H
#define _BOOTLOADER_MESSAGE_CORE_H

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

// Spaces used by misc partition are as below:
// 0   - 2K     For bootloader_message
// 2K  - 16K    Used by Vendor's bootloader (the 2K - 4K range may be optionally used
//              as bootloader_message_ab struct)
// 16K - 32K    Used by uncrypt and recovery to store wipe_package for A/B devices
// 32K - 64K    System space, used for miscellanious AOSP features. See below.
// Note that these offsets are admitted by bootloader,recovery and uncrypt, so they
// are not configurable without changing all of them.
constexpr size_t BOOTLOADER_MESSAGE_OFFSET_IN_MISC = BOARD_RECOVERY_BLDRMSG_OFFSET;
constexpr size_t VENDOR_SPACE_OFFSET_IN_MISC = 2 * 1024 + BOARD_RECOVERY_BLDRMSG_OFFSET;
constexpr size_t WIPE_PACKAGE_OFFSET_IN_MISC = 16 * 1024 + BOARD_RECOVERY_BLDRMSG_OFFSET;
constexpr size_t SYSTEM_SPACE_OFFSET_IN_MISC = 32 * 1024 + BOARD_RECOVERY_BLDRMSG_OFFSET;
constexpr size_t SYSTEM_SPACE_SIZE_IN_MISC = 32 * 1024 + BOARD_RECOVERY_BLDRMSG_OFFSET;

/* Bootloader Message (2-KiB)
 *
 * This structure describes the content of a block in flash
 * that is used for recovery and the bootloader to talk to
 * each other.
 *
 * The command field is updated by linux when it wants to
 * reboot into recovery or to update radio or bootloader firmware.
 * It is also updated by the bootloader when firmware update
 * is complete (to boot into recovery for any final cleanup)
 *
 * The status field was used by the bootloader after the completion
 * of an "update-radio" or "update-hboot" command, which has been
 * deprecated since Froyo.
 *
 * The recovery field is only written by linux and used
 * for the system to send a message to recovery or the
 * other way around.
 *
 * The stage field is written by packages which restart themselves
 * multiple times, so that the UI can reflect which invocation of the
 * package it is.  If the value is of the format "#/#" (eg, "1/3"),
 * the UI will add a simple indicator of that status.
 *
 * We used to have slot_suffix field for A/B boot control metadata in
 * this struct, which gets unintentionally cleared by recovery or
 * uncrypt. Move it into struct bootloader_message_ab to avoid the
 * issue.
 */
struct bootloader_message {
  char command[32];
  char status[32];
  char recovery[768];

  // The 'recovery' field used to be 1024 bytes.  It has only ever
  // been used to store the recovery command line, so 768 bytes
  // should be plenty.  We carve off the last 256 bytes to store the
  // stage string (for multistage packages) and possible future
  // expansion.
  char stage[32];

  // The 'reserved' field used to be 224 bytes when it was initially
  // carved off from the 1024-byte recovery field. Bump it up to
  // 1184-byte so that the entire bootloader_message struct rounds up
  // to 2048-byte.
  char reserved[1184];
};

// Holds Virtual A/B merge status information. Current version is 1. New fields
// must be added to the end.
struct misc_virtual_ab_message {
  uint8_t version;
  uint32_t magic;
  uint8_t merge_status;  // IBootControl 1.1, MergeStatus enum.
  uint8_t source_slot;   // Slot number when merge_status was written.
  uint8_t reserved[57];
} __attribute__((packed));

struct misc_memtag_message {
  uint8_t version;
  uint32_t magic;  // magic string for treble compat
  uint32_t memtag_mode;
  uint8_t reserved[55];
} __attribute__((packed));

struct misc_kcmdline_message {
  uint8_t version;
  uint32_t magic;
  uint64_t kcmdline_flags;
  uint8_t reserved[51];
} __attribute__((packed));

// holds generic platform info, managed by misctrl
struct misc_control_message {
  uint8_t version;
  uint32_t magic;
  uint64_t misctrl_flags;
  uint8_t reserved[51];
} __attribute__((packed));

#define MISC_VIRTUAL_AB_MESSAGE_VERSION 2
#define MISC_VIRTUAL_AB_MAGIC_HEADER 0x56740AB0

#define MISC_MEMTAG_MESSAGE_VERSION 1
#define MISC_MEMTAG_MAGIC_HEADER 0x5afefe5a
#define MISC_MEMTAG_MODE_MEMTAG 0x1
#define MISC_MEMTAG_MODE_MEMTAG_ONCE 0x2
#define MISC_MEMTAG_MODE_MEMTAG_KERNEL 0x4
#define MISC_MEMTAG_MODE_MEMTAG_KERNEL_ONCE 0x8
#define MISC_MEMTAG_MODE_MEMTAG_OFF 0x10
// This is set when the state was overridden forcibly. This does not need to be
// interpreted by the bootloader but is only for bookkeeping purposes so
// userspace knows what to do when the override is undone.
// See system/extras/mtectrl in AOSP for more information.
#define MISC_MEMTAG_MODE_FORCED 0x20

#define MISC_KCMDLINE_MESSAGE_VERSION 1
#define MISC_KCMDLINE_MAGIC_HEADER 0x6ab5110c
#define MISC_KCMDLINE_BINDER_FORCE_RUST 0x1
#define MISC_KCMDLINE_BINDER_FORCE_C 0x2

#define MISC_CONTROL_MESSAGE_VERSION 1
#define MISC_CONTROL_MAGIC_HEADER 0x736d6f72
#define MISC_CONTROL_16KB_BEFORE 0x1

#if (__STDC_VERSION__ >= 201112L) || defined(__cplusplus)
static_assert(sizeof(struct misc_virtual_ab_message) == 64,
              "struct misc_virtual_ab_message has wrong size");
static_assert(sizeof(struct misc_memtag_message) == 64,
              "struct misc_memtag_message has wrong size");
static_assert(sizeof(struct misc_kcmdline_message) == 64,
              "struct misc_kcmdline_message has wrong size");
static_assert(sizeof(struct misc_control_message) == 64,
              "struct misc_control_message has wrong size");
#endif

// This struct is not meant to be used directly, rather, it is to make
// computation of offsets easier. New fields must be added to the end.
struct misc_system_space_layout {
  misc_virtual_ab_message virtual_ab_message;
  misc_memtag_message memtag_message;
  misc_kcmdline_message kcmdline_message;
  misc_control_message control_message;
} __attribute__((packed));

#if (__STDC_VERSION__ >= 201112L) || defined(__cplusplus)
static_assert(sizeof(struct misc_system_space_layout) % 64 == 0,
              "prefer to extend by 64 byte chunks, for consistency");
#endif

#endif  // _BOOTLOADER_MESSAGE_CORE_H
