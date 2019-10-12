LOCAL_PATH := $(call my-dir)

include $(SF_COMMON_MK)

LOCAL_SRC_FILES := \
	FFmpegExtractor.cpp

LOCAL_SHARED_LIBRARIES += \
	libbinder         \
	libffmpeg_utils   \
	libmediandk       \
	liblog

LOCAL_MODULE:= libffmpeg_extractor
LOCAL_MODULE_RELATIVE_PATH := extractors

include $(BUILD_SHARED_LIBRARY)
