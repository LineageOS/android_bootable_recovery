# Copyright (C) 2007 The Android Open Source Project
# Copyright (C) 2015 The CyanogenMod Project
# Copyright (C) 2017 The Lineage Android Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

ifeq ($(call my-dir),$(call project-path-for,recovery))

LOCAL_PATH := $(call my-dir)

# Needed by build/make/core/Makefile.
RECOVERY_API_VERSION := 3
RECOVERY_FSTAB_VERSION := 2

# libfusesideload (static library)
# ===============================
include $(CLEAR_VARS)
LOCAL_SRC_FILES := fuse_sideload.cpp
LOCAL_CFLAGS := -Wall -Werror
LOCAL_CFLAGS += -D_XOPEN_SOURCE -D_GNU_SOURCE
LOCAL_MODULE := libfusesideload
LOCAL_STATIC_LIBRARIES := \
    libcrypto \
    libbase
include $(BUILD_STATIC_LIBRARY)

# libmounts (static library)
# ===============================
include $(CLEAR_VARS)
LOCAL_SRC_FILES := mounts.cpp
LOCAL_CFLAGS := \
    -Wall \
    -Werror
LOCAL_MODULE := libmounts
LOCAL_STATIC_LIBRARIES := libbase
include $(BUILD_STATIC_LIBRARY)

# librecovery (static library)
# ===============================
include $(CLEAR_VARS)
LOCAL_SRC_FILES := \
    install.cpp
LOCAL_CFLAGS := -Wall -Werror
LOCAL_CFLAGS += -DRECOVERY_API_VERSION=$(RECOVERY_API_VERSION)

ifeq ($(AB_OTA_UPDATER),true)
    LOCAL_CFLAGS += -DAB_OTA_UPDATER=1
endif

LOCAL_MODULE := librecovery
LOCAL_STATIC_LIBRARIES := \
    libminui \
    libvintf_recovery \
    libcrypto_utils \
    libcrypto \
    libbase \
    libziparchive \

include $(BUILD_STATIC_LIBRARY)

# recovery (static executable)
# ===============================
include $(CLEAR_VARS)

LOCAL_SRC_FILES := \
    adb_install.cpp \
    device.cpp \
    fuse_sdcard_provider.cpp \
    recovery.cpp \
    roots.cpp \
    rotate_logs.cpp \
    screen_ui.cpp \
    ui.cpp \
    vr_ui.cpp \
    wear_ui.cpp \
    voldclient.cpp

# External tools
LOCAL_SRC_FILES += \
    ../../system/vold/vdc.cpp

LOCAL_MODULE := recovery

LOCAL_FORCE_STATIC_EXECUTABLE := true

LOCAL_REQUIRED_MODULES := e2fsdroid_static mke2fs_static mke2fs.conf

ifeq ($(TARGET_USERIMAGES_USE_F2FS),true)
ifeq ($(HOST_OS),linux)
LOCAL_REQUIRED_MODULES += mkfs.f2fs
endif
endif

LOCAL_CFLAGS += -DRECOVERY_API_VERSION=$(RECOVERY_API_VERSION)
LOCAL_CFLAGS += -Wno-unused-parameter -Werror

ifneq ($(TARGET_RECOVERY_UI_MARGIN_HEIGHT),)
LOCAL_CFLAGS += -DRECOVERY_UI_MARGIN_HEIGHT=$(TARGET_RECOVERY_UI_MARGIN_HEIGHT)
else
LOCAL_CFLAGS += -DRECOVERY_UI_MARGIN_HEIGHT=0
endif

ifneq ($(TARGET_RECOVERY_UI_MARGIN_WIDTH),)
LOCAL_CFLAGS += -DRECOVERY_UI_MARGIN_WIDTH=$(TARGET_RECOVERY_UI_MARGIN_WIDTH)
else
LOCAL_CFLAGS += -DRECOVERY_UI_MARGIN_WIDTH=0
endif

ifneq ($(TARGET_RECOVERY_UI_TOUCH_LOW_THRESHOLD),)
LOCAL_CFLAGS += -DRECOVERY_UI_TOUCH_LOW_THRESHOLD=$(TARGET_RECOVERY_UI_TOUCH_LOW_THRESHOLD)
else
LOCAL_CFLAGS += -DRECOVERY_UI_TOUCH_LOW_THRESHOLD=50
endif

ifneq ($(TARGET_RECOVERY_UI_TOUCH_HIGH_THRESHOLD),)
LOCAL_CFLAGS += -DRECOVERY_UI_TOUCH_HIGH_THRESHOLD=$(TARGET_RECOVERY_UI_TOUCH_HIGH_THRESHOLD)
else
LOCAL_CFLAGS += -DRECOVERY_UI_TOUCH_HIGH_THRESHOLD=90
endif

ifneq ($(TARGET_RECOVERY_UI_PROGRESS_BAR_BASELINE),)
LOCAL_CFLAGS += -DRECOVERY_UI_PROGRESS_BAR_BASELINE=$(TARGET_RECOVERY_UI_PROGRESS_BAR_BASELINE)
else
LOCAL_CFLAGS += -DRECOVERY_UI_PROGRESS_BAR_BASELINE=259
endif

ifneq ($(TARGET_RECOVERY_UI_ANIMATION_FPS),)
LOCAL_CFLAGS += -DRECOVERY_UI_ANIMATION_FPS=$(TARGET_RECOVERY_UI_ANIMATION_FPS)
else
LOCAL_CFLAGS += -DRECOVERY_UI_ANIMATION_FPS=30
endif

ifneq ($(TARGET_RECOVERY_UI_MENU_UNUSABLE_ROWS),)
LOCAL_CFLAGS += -DRECOVERY_UI_MENU_UNUSABLE_ROWS=$(TARGET_RECOVERY_UI_MENU_UNUSABLE_ROWS)
else
LOCAL_CFLAGS += -DRECOVERY_UI_MENU_UNUSABLE_ROWS=9
endif

ifneq ($(TARGET_RECOVERY_UI_VR_STEREO_OFFSET),)
LOCAL_CFLAGS += -DRECOVERY_UI_VR_STEREO_OFFSET=$(TARGET_RECOVERY_UI_VR_STEREO_OFFSET)
else
LOCAL_CFLAGS += -DRECOVERY_UI_VR_STEREO_OFFSET=0
endif

ifeq ($(TARGET_RECOVERY_USE_LCD_POWER_BLANK),true)
LOCAL_CFLAGS += -DUSE_LCD_POWER_BLANK
endif

LOCAL_C_INCLUDES += \
    system/vold \
    external/e2fsprogs/lib

LOCAL_STATIC_LIBRARIES := \
    libmksh_driver \
    librecovery \
    libverifier \
    libbatterymonitor \
    libbootloader_message \
    libfs_mgr \
    libext4_utils \
    libext2_blkid \
    libext2_uuid \
    libsparse \
    libreboot \
    libziparchive \
    libminipigz_static \
    libzopfli_static \
    libminizip_static \
    libminiunz_static \
    libsdcard \
    libotautil \
    libmounts \
    libz \
    libminadbd \
    libfusesideload \
    libminui \
    libpng \
    libcrypto_utils \
    libcrypto \
    libvintf_recovery \
    libvintf \
    libtinyxml2 \
    libbase \
    libcutils \
    libutils \
    liblog \
    libselinux \
    libm \
    libc

# Libraries for FS tools
LOCAL_WHOLE_STATIC_LIBRARIES += \
    libfuse_static

ifneq ($(TARGET_EXFAT_DRIVER),)
LOCAL_CFLAGS += -DWITH_EXFAT
LOCAL_WHOLE_STATIC_LIBRARIES += \
    libexfat_static \
    libexfat_fsck_static \
    libexfat_mkfs_static
endif

LOCAL_WHOLE_STATIC_LIBRARIES += \
    libntfs-3g_static \
    libntfs3g_fsck_static \
    libntfs3g_mkfs_main \
    libntfs3g_mount_static

LOCAL_WHOLE_STATIC_LIBRARIES += \
    libext2fs \
    libe2fsck \
    libmke2fs \
    libtune2fs \
    libsparse

LOCAL_WHOLE_STATIC_LIBRARIES += \
    libf2fs \
    libf2fs_fsck \
    libf2fs_mkfs

LOCAL_WHOLE_STATIC_LIBRARIES += \
    libsgdisk_static

LOCAL_STATIC_LIBRARIES += \
    libext2_blkid \
    libext2_uuid \
    libext2_profile \
    libext2_quota \
    libext2_com_err \
    libext2_e2p \
    libc++_static \
    libz

FILESYSTEM_TOOLS := \
    e2fsck mke2fs tune2fs fsck.ext4 mkfs.ext4 \
    fsck.ntfs mkfs.ntfs mount.ntfs \
    mkfs.f2fs fsck.f2fs \
    sgdisk

ifneq ($(TARGET_EXFAT_DRIVER),)
FILESYSTEM_TOOLS += \
    fsck.exfat mkfs.exfat
endif

LOCAL_HAL_STATIC_LIBRARIES := libhealthd

ifeq ($(AB_OTA_UPDATER),true)
    LOCAL_CFLAGS += -DAB_OTA_UPDATER=1
endif

ifeq ($(BOARD_HAS_DOWNLOAD_MODE), true)
    LOCAL_CFLAGS += -DDOWNLOAD_MODE
endif

LOCAL_MODULE_PATH := $(TARGET_RECOVERY_ROOT_OUT)/sbin

LOCAL_CFLAGS += -DMINIVOLD
LOCAL_C_INCLUDES += system/extras/ext4_utils system/core/fs_mgr/include external/fsck_msdos
LOCAL_C_INCLUDES += system/vold

ifeq ($(TARGET_RECOVERY_UI_LIB),)
  LOCAL_SRC_FILES += default_device.cpp
else
  LOCAL_STATIC_LIBRARIES += $(TARGET_RECOVERY_UI_LIB)
endif

ifeq ($(BOARD_CACHEIMAGE_PARTITION_SIZE),)
LOCAL_REQUIRED_MODULES += recovery-persist recovery-refresh
endif

LOCAL_REQUIRED_MODULES += \
    toybox_static \
    recovery_mkshrc \
    minivold \
    bu_recovery

# Symlinks
RECOVERY_TOOLS := \
    reboot \
    setup_adbd \
    sh \
    gunzip \
    gzip \
    unzip \
    zip \
    vdc \
    $(FILESYSTEM_TOOLS)
LOCAL_POST_INSTALL_CMD := $(hide) $(foreach t,$(RECOVERY_TOOLS),ln -sf ${LOCAL_MODULE} $(LOCAL_MODULE_PATH)/$(t);)

ifneq ($(TARGET_RECOVERY_DEVICE_MODULES),)
    LOCAL_ADDITIONAL_DEPENDENCIES += $(TARGET_RECOVERY_DEVICE_MODULES)
endif

include $(BUILD_EXECUTABLE)

# mkshrc
include $(CLEAR_VARS)
LOCAL_MODULE := recovery_mkshrc
LOCAL_MODULE_TAGS := optional
LOCAL_MODULE_CLASS := ETC
LOCAL_MODULE_PATH := $(TARGET_RECOVERY_ROOT_OUT)/etc
LOCAL_SRC_FILES := etc/mkshrc
LOCAL_MODULE_STEM := mkshrc
include $(BUILD_PREBUILT)

include $(CLEAR_VARS)
LOCAL_MODULE := bu_recovery
LOCAL_MODULE_STEM := bu
LOCAL_MODULE_TAGS := optional
LOCAL_MODULE_PATH := $(TARGET_RECOVERY_ROOT_OUT)/sbin
LOCAL_FORCE_STATIC_EXECUTABLE := true
LOCAL_SRC_FILES := \
    bu.cpp \
    backup.cpp \
    restore.cpp \
    roots.cpp \
    voldclient.cpp
LOCAL_CFLAGS += -DMINIVOLD
LOCAL_CFLAGS += -Wno-unused-parameter
LOCAL_STATIC_LIBRARIES += \
    libext4_utils \
    libext2_blkid \
    libext2_uuid \
    libsparse \
    libmounts \
    libz \
    libminadbd \
    libminui \
    libfs_mgr \
    libtar \
    libcrypto \
    libbase \
    libcutils \
    libutils \
    liblog \
    libselinux \
    libm \
    libc

LOCAL_C_INCLUDES += \
    system/core/fs_mgr/include \
    system/core/include \
    system/core/libcutils \
    system/extras/ext4_utils \
    system/vold \
    external/libtar \
    external/libtar/listhash \
    external/openssl/include \
    external/zlib \
    bionic/libc/bionic \
    external/e2fsprogs/lib

include $(BUILD_EXECUTABLE)

# Minizip static library
include $(CLEAR_VARS)
LOCAL_MODULE := libminizip_static
LOCAL_MODULE_TAGS := optional
LOCAL_CFLAGS := -Dmain=minizip_main -D__ANDROID__ -DIOAPI_NO_64
LOCAL_C_INCLUDES := external/zlib
LOCAL_SRC_FILES := \
    ../../external/zlib/src/contrib/minizip/ioapi.c \
    ../../external/zlib/src/contrib/minizip/minizip.c \
    ../../external/zlib/src/contrib/minizip/zip.c
include $(BUILD_STATIC_LIBRARY)

# Miniunz static library
include $(CLEAR_VARS)
LOCAL_MODULE := libminiunz_static
LOCAL_MODULE_TAGS := optional
LOCAL_CFLAGS := -Dmain=miniunz_main -D__ANDROID__ -DIOAPI_NO_64
LOCAL_C_INCLUDES := external/zlib
LOCAL_SRC_FILES := \
    ../../external/zlib/src/contrib/minizip/ioapi.c \
    ../../external/zlib/src/contrib/minizip/miniunz.c \
    ../../external/zlib/src/contrib/minizip/unzip.c
include $(BUILD_STATIC_LIBRARY)

# Reboot static library
include $(CLEAR_VARS)
LOCAL_MODULE := libreboot
LOCAL_MODULE_TAGS := optional
LOCAL_CFLAGS := -Dmain=reboot_main
LOCAL_SRC_FILES := ../../system/core/reboot/reboot.c
include $(BUILD_STATIC_LIBRARY)


# recovery-persist (system partition dynamic executable run after /data mounts)
# ===============================
include $(CLEAR_VARS)
LOCAL_SRC_FILES := \
    recovery-persist.cpp \
    rotate_logs.cpp
LOCAL_MODULE := recovery-persist
LOCAL_SHARED_LIBRARIES := liblog libbase
LOCAL_CFLAGS := -Werror
LOCAL_INIT_RC := recovery-persist.rc
include $(BUILD_EXECUTABLE)

# recovery-refresh (system partition dynamic executable run at init)
# ===============================
include $(CLEAR_VARS)
LOCAL_SRC_FILES := \
    recovery-refresh.cpp \
    rotate_logs.cpp
LOCAL_MODULE := recovery-refresh
LOCAL_SHARED_LIBRARIES := liblog libbase
LOCAL_CFLAGS := -Werror
LOCAL_INIT_RC := recovery-refresh.rc
include $(BUILD_EXECUTABLE)

# libverifier (static library)
# ===============================
include $(CLEAR_VARS)
LOCAL_MODULE := libverifier
LOCAL_SRC_FILES := \
    asn1_decoder.cpp \
    verifier.cpp
LOCAL_STATIC_LIBRARIES := \
    libcrypto_utils \
    libcrypto \
    libbase
LOCAL_CFLAGS := -Werror
include $(BUILD_STATIC_LIBRARY)

# Wear default device
# ===============================
include $(CLEAR_VARS)
LOCAL_SRC_FILES := wear_device.cpp

# Should match TARGET_RECOVERY_UI_LIB in BoardConfig.mk.
LOCAL_MODULE := librecovery_ui_wear

include $(BUILD_STATIC_LIBRARY)

# vr headset default device
# ===============================
include $(CLEAR_VARS)

LOCAL_SRC_FILES := vr_device.cpp

# should match TARGET_RECOVERY_UI_LIB set in BoardConfig.mk
LOCAL_MODULE := librecovery_ui_vr

include $(BUILD_STATIC_LIBRARY)

include \
    $(LOCAL_PATH)/applypatch/Android.mk \
    $(LOCAL_PATH)/boot_control/Android.mk \
    $(LOCAL_PATH)/bootloader_message/Android.mk \
    $(LOCAL_PATH)/edify/Android.mk \
    $(LOCAL_PATH)/minadbd/Android.mk \
    $(LOCAL_PATH)/minui/Android.mk \
    $(LOCAL_PATH)/otafault/Android.mk \
    $(LOCAL_PATH)/otautil/Android.mk \
    $(LOCAL_PATH)/tests/Android.mk \
    $(LOCAL_PATH)/tools/Android.mk \
    $(LOCAL_PATH)/uncrypt/Android.mk \
    $(LOCAL_PATH)/updater/Android.mk \
    $(LOCAL_PATH)/update_verifier/Android.mk \

endif
