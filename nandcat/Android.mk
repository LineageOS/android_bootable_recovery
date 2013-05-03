LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

LOCAL_SRC_FILES:=\
        nandcat.c \
	atomicio.c


LOCAL_CFLAGS:=-O2 -g
#LOCAL_CFLAGS+=-DLINUX

LOCAL_MODULE_TAGS := eng
LOCAL_MODULE_PATH := $(TARGET_RECOVERY_ROOT_OUT)/sbin
LOCAL_MODULE:=nandcat
#LOCAL_MODULE_STEM:=nc
LOCAL_MODULE_CLASS := RECOVERY_EXECUTABLES
LOCAL_FORCE_STATIC_EXECUTABLE := true
LOCAL_STATIC_LIBRARIES := libcutils libc
include $(BUILD_EXECUTABLE)

