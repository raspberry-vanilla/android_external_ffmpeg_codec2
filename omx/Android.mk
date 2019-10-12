LOCAL_PATH := $(call my-dir)

include $(SF_COMMON_MK)

LOCAL_SRC_FILES := \
	FFmpegOMXPlugin.cpp \
	SoftFFmpegAudio.cpp \
	SoftFFmpegVideo.cpp \
	ffmpeg_hwaccel.c \

LOCAL_C_INCLUDES += \
	$(TOP)/frameworks/native/include/media/hardware

LOCAL_SHARED_LIBRARIES += \
	libdl             \
	libffmpeg_utils   \
	android.hidl.memory@1.0	\
	$(if $(filter true,$(BOARD_USE_LIBAV)),libavresample,libswresample) \
	liblog            \
	libnativewindow   \
	libswscale        \
	libstagefright_softomx

LOCAL_MODULE:= libffmpeg_omx

include $(BUILD_SHARED_LIBRARY)
