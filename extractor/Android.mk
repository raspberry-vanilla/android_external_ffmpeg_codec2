LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
include external/ffmpeg/android/ffmpeg.mk

LOCAL_SRC_FILES := \
	FFmpegExtractor.cpp

LOCAL_C_INCLUDES += \
	$(LOCAL_PATH)/.. \
	$(TOP)/frameworks/native/include/media/openmax \
	$(TOP)/frameworks/av/include \
	$(TOP)/frameworks/av/media/libstagefright

LOCAL_SHARED_LIBRARIES := \
	libutils          \
	libcutils         \
	libbinder         \
	libavcodec        \
	libavformat       \
	libavutil         \
	libmedia          \
	libffmpeg_utils   \
	libstagefright    \
	libstagefright_foundation

LOCAL_MODULE:= libffmpeg_extractor

LOCAL_MODULE_TAGS := optional

ifneq ($(filter arm arm64,$(TARGET_ARCH)),)
	LOCAL_CFLAGS += -Wno-psabi
endif

ifdef TARGET_2ND_ARCH
LOCAL_MODULE_PATH_32 := $(TARGET_OUT_VENDOR)/lib
LOCAL_MODULE_PATH_64 := $(TARGET_OUT_VENDOR)/lib64
else
LOCAL_MODULE_PATH := $(TARGET_OUT_VENDOR_SHARED_LIBRARIES)
endif

LOCAL_CFLAGS += -D__STDC_CONSTANT_MACROS=1 -D__STDINT_LIMITS=1

#ifneq ($(filter arm arm64,$(TARGET_ARCH)),)
#	LOCAL_CFLAGS += -fpermissive
#endif

include $(BUILD_SHARED_LIBRARY)
