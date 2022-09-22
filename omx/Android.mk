LOCAL_PATH := $(call my-dir)

include $(SF_COMMON_MK)

LOCAL_SRC_FILES := \
	FFmpegOMXPlugin.cpp \
	SoftFFmpegAudio.cpp \
	SoftFFmpegVideo.cpp

LOCAL_SHARED_LIBRARIES += \
	libavcodec        \
	libavutil         \
	libcutils         \
	libffmpeg_utils   \
	$(if $(filter true,$(BOARD_USE_LIBAV)),libavresample,libswresample) \
	liblog            \
	libswscale        \
	libstagefright    \
	libstagefright_foundation \
	libstagefright_softomx \
	libutils

LOCAL_MODULE:= libffmpeg_omx

include $(BUILD_SHARED_LIBRARY)
