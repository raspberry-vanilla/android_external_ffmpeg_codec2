LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
include external/ffmpeg/android/ffmpeg.mk

LOCAL_SRC_FILES := \
	FFmpegOMXPlugin.cpp \
	SoftFFmpegAudio.cpp \
	SoftFFmpegVideo.cpp

LOCAL_C_INCLUDES += \
	$(LOCAL_PATH)/../include \
	$(LOCAL_PATH)/.. \
	$(TOP)/frameworks/native/include/media/hardware \
	$(TOP)/frameworks/native/include/media/openmax \
	$(TOP)/frameworks/av/include \
	$(TOP)/frameworks/av/media/libstagefright \
	$(TOP)/frameworks/av/media/libstagefright/include

LOCAL_SHARED_LIBRARIES := \
	libdl             \
	libutils          \
	libcutils         \
	libavcodec		  \
	libavformat		  \
	libavutil		  \
	libffmpeg_utils   \
	libswresample     \
	libswscale        \
	libstagefright    \
	libstagefright_foundation \
	libstagefright_omx

LOCAL_MODULE:= libffmpeg_omx

LOCAL_MODULE_TAGS := optional

ifneq ($(filter arm arm64,$(TARGET_ARCH)),)
	LOCAL_CFLAGS += -Wno-psabi
endif

LOCAL_CFLAGS += -D__STDC_CONSTANT_MACROS=1 -D__STDINT_LIMITS=1

#ifneq ($(filter arm arm64,$(TARGET_ARCH)),)
#	LOCAL_CFLAGS += -fpermissive
#endif

include $(BUILD_SHARED_LIBRARY)

include $(call first-makefiles-under,$(LOCAL_PATH))
