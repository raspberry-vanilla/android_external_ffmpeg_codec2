LOCAL_PATH := $(call my-dir)

include $(SF_COMMON_MK)

LOCAL_SRC_FILES := \
	FFmpegOMXPlugin.cpp \
	SoftFFmpegAudio.cpp \
	SoftFFmpegVideo.cpp

LOCAL_C_INCLUDES += \
	$(TOP)/frameworks/native/include/media/hardware \
	$(TOP)/frameworks/av/media/libstagefright/include

LOCAL_SHARED_LIBRARIES += \
	libdl             \
	libffmpeg_utils   \
	libswresample     \
	libswscale        \
	libstagefright_omx

LOCAL_MODULE:= libffmpeg_omx

include $(BUILD_SHARED_LIBRARY)
