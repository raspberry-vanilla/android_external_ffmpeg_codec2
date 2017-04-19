#
# Copyright (C) 2017 The Android-x86 Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

include $(CLEAR_VARS)

include external/$(AV_CODEC_LIB)/android/$(AV_CODEC_LIB).mk

# put the libraries to /vendor
LOCAL_PROPRIETARY_MODULE := true

LOCAL_C_INCLUDES += \
	$(LOCAL_PATH)/../include \
	$(LOCAL_PATH)/.. \
	frameworks/native/include/media/openmax \
	frameworks/av/include \
	frameworks/av/media/libstagefright

LOCAL_SHARED_LIBRARIES := \
	libutils \
	libcutils \
	libavcodec \
	libavformat \
	libavutil \
	libstagefright \
	libstagefright_foundation

ifneq ($(filter arm arm64,$(TARGET_ARCH)),)
	LOCAL_CFLAGS += -Wno-psabi
endif

LOCAL_CFLAGS += -D__STDC_CONSTANT_MACROS=1 -D__STDINT_LIMITS=1

LOCAL_MODULE_TAGS := optional
