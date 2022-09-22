LOCAL_PATH := $(call my-dir)

include $(SF_COMMON_MK)

LOCAL_SRC_FILES := \
	ffmpeg_source.cpp \
	ffmpeg_utils.cpp \
	ffmpeg_cmdutils.c \
	ffmpeg_hwaccel.c \
	codec_utils.cpp

LOCAL_SHARED_LIBRARIES += \
	libavcodec        \
	libavformat       \
	libavutil         \
	libcutils         \
	liblog            \
	libstagefright    \
	libstagefright_foundation \
	$(if $(filter true,$(BOARD_USE_LIBAV)),libavresample,libswresample) \
	libswscale        \
	libutils

LOCAL_STATIC_LIBRARIES += libstagefright_metadatautils

LOCAL_HEADER_LIBRARIES += libaudio_system_headers media_ndk_headers

LOCAL_MODULE := libffmpeg_utils

LOCAL_EXPORT_C_INCLUDE_DIRS := $(LOCAL_PATH)
LOCAL_EXPORT_SHARED_LIBRARY_HEADERS += \
	libavcodec        \
	libavformat       \
	$(if $(filter true,$(BOARD_USE_LIBAV)),libavresample,libswresample) \
	libswscale
LOCAL_EXPORT_HEADER_LIBRARY_HEADERS += libaudio_system_headers media_ndk_headers

# Workaround for inline assembly tricks in FFMPEG which don't play nice with
# Clang when included from C++
LOCAL_CLANG_CFLAGS += -DAVUTIL_ARM_INTREADWRITE_H

include $(BUILD_SHARED_LIBRARY)
