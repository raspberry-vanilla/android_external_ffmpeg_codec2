LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
include external/ffmpeg/android/ffmpeg.mk

LOCAL_SRC_FILES := \
	FFmpegExtractor.cpp

LOCAL_C_INCLUDES += \
	$(LOCAL_PATH)/../.. \
	$(TOP)/frameworks/native/include/media/openmax \
	$(TOP)/frameworks/av/include \
	$(TOP)/frameworks/av/media/libstagefright

LOCAL_SHARED_LIBRARIES := \
	libutils          \
	libcutils         \
	libavcodec        \
	libavformat       \
	libavutil         \
	libffmpeg_utils   \
	libstagefright    \
	libstagefright_foundation

LOCAL_MODULE:= libFFmpegExtractor

LOCAL_MODULE_TAGS := optional

ifneq ($(filter arm arm64,$(TARGET_ARCH)),)
	LOCAL_CFLAGS += -Wno-psabi
endif

LOCAL_CFLAGS += -D__STDC_CONSTANT_MACROS=1 -D__STDINT_LIMITS=1

#ifneq ($(filter arm arm64,$(TARGET_ARCH)),)
#	LOCAL_CFLAGS += -fpermissive
#endif

include $(BUILD_SHARED_LIBRARY)
