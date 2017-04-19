LOCAL_PATH := $(call my-dir)

include $(SF_COMMON_MK)

LOCAL_SRC_FILES := \
	FFmpegExtractor.cpp

LOCAL_SHARED_LIBRARIES += \
	libbinder         \
	libmedia          \
	libffmpeg_utils   \

LOCAL_MODULE:= libffmpeg_extractor

include $(BUILD_SHARED_LIBRARY)
