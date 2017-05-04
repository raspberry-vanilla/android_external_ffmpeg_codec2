LOCAL_PATH:= $(call my-dir)

include $(SF_COMMON_MK)

LOCAL_SRC_FILES := \
	ffmpeg_source.cpp \
	ffmpeg_utils.cpp \
	ffmpeg_cmdutils.c \
	codec_utils.cpp

LOCAL_MODULE := libffmpeg_utils

# Workaround for inline assembly tricks in FFMPEG which don't play nice with
# Clang when included from C++
LOCAL_CLANG_CFLAGS += -DAVUTIL_ARM_INTREADWRITE_H

include $(BUILD_SHARED_LIBRARY)
