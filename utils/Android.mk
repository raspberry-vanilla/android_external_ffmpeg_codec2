LOCAL_PATH := $(call my-dir)

include $(SF_COMMON_MK)

LOCAL_SRC_FILES := \
	ffmpeg_hwaccel.c \
	ffmpeg_utils.cpp

LOCAL_SHARED_LIBRARIES += \
	libavcodec        \
	libavformat       \
	libavutil         \
	libcutils         \
	liblog            \
	libstagefright_foundation \
	libswresample     \
	libswscale        \
	libutils

LOCAL_MODULE := libffmpeg_utils

LOCAL_EXPORT_C_INCLUDE_DIRS := $(LOCAL_PATH)

# Workaround for inline assembly tricks in FFMPEG which don't play nice with
# Clang when included from C++
LOCAL_CLANG_CFLAGS += -DAVUTIL_ARM_INTREADWRITE_H

include $(BUILD_SHARED_LIBRARY)
