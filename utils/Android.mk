LOCAL_PATH:= $(call my-dir)

include $(CLEAR_VARS)
include external/ffmpeg/android/ffmpeg.mk

LOCAL_SRC_FILES := \
	ffmpeg_source.cpp \
	ffmpeg_utils.cpp \
	ffmpeg_cmdutils.c \
	codec_utils.cpp

LOCAL_C_INCLUDES += \
	$(TOP)/frameworks/native/include/media/openmax \
	$(TOP)/frameworks/av/include \
	$(TOP)/frameworks/av/media/libstagefright

LOCAL_SHARED_LIBRARIES := \
	libavcodec \
	libavformat \
	libavutil \
	libutils \
	libcutils \
	libstagefright \
	libstagefright_foundation

LOCAL_MODULE := libffmpeg_utils

LOCAL_MODULE_TAGS := optional

ifdef TARGET_2ND_ARCH
LOCAL_MODULE_PATH_32 := $(TARGET_OUT_VENDOR)/lib
LOCAL_MODULE_PATH_64 := $(TARGET_OUT_VENDOR)/lib64
else
LOCAL_MODULE_PATH := $(TARGET_OUT_VENDOR_SHARED_LIBRARIES)
endif

LOCAL_CFLAGS += -D__STDC_CONSTANT_MACROS=1

# Workaround for inline assembly tricks in FFMPEG which don't play nice with
# Clang when included from C++
LOCAL_CLANG_CFLAGS += -DAVUTIL_ARM_INTREADWRITE_H

# Quiet some noise from FFMPEG
LOCAL_CLANG_CFLAGS += -Wno-unknown-attributes -Wno-deprecated-declarations

#ifneq ($(filter arm arm64,$(TARGET_ARCH)),)
#	LOCAL_CFLAGS += -fpermissive
#endif

include $(BUILD_SHARED_LIBRARY)
