#
# Copyright (C) 2022 Michael Goffioul <michael.goffioul@gmail.com>
# Copyright (C) 2023 KonstaKANG
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

LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE := android.hardware.media.c2@1.2-service-ffmpeg
LOCAL_VENDOR_MODULE := true
LOCAL_MODULE_RELATIVE_PATH := hw
LOCAL_VINTF_FRAGMENTS := android.hardware.media.c2@1.2-service-ffmpeg.xml
LOCAL_INIT_RC := android.hardware.media.c2@1.2-service-ffmpeg.rc

LOCAL_REQUIRED_MODULES := \
    android.hardware.media.c2@1.2-ffmpeg.policy \
    media_codecs_ffmpeg_c2.xml

LOCAL_SRC_FILES := \
    C2FFMPEGAudioDecodeComponent.cpp \
    C2FFMPEGAudioDecodeInterface.cpp \
    C2FFMPEGVideoDecodeComponent.cpp \
    C2FFMPEGVideoDecodeInterface.cpp \
    service.cpp

LOCAL_SHARED_LIBRARIES := \
    android.hardware.media.c2@1.2 \
    libavcodec \
    libavutil \
    libavservices_minijail \
    libbase \
    libbinder \
    libcodec2_hidl@1.2 \
    libcodec2_soft_common \
    libcodec2_vndk \
    libffmpeg_utils \
    libhidlbase \
    liblog \
    libstagefright_foundation \
    libswresample \
    libswscale \
    libutils

include $(BUILD_EXECUTABLE)

include $(CLEAR_VARS)
LOCAL_MODULE := android.hardware.media.c2@1.2-ffmpeg.policy
LOCAL_VENDOR_MODULE := true
LOCAL_MODULE_CLASS := ETC
LOCAL_MODULE_RELATIVE_PATH := seccomp_policy
LOCAL_SRC_FILES := android.hardware.media.c2@1.2-ffmpeg.policy
include $(BUILD_PREBUILT)

include $(CLEAR_VARS)
LOCAL_MODULE := media_codecs_ffmpeg_c2.xml
LOCAL_VENDOR_MODULE := true
LOCAL_MODULE_CLASS := ETC
LOCAL_SRC_FILES := media_codecs_ffmpeg_c2.xml
include $(BUILD_PREBUILT)

include $(call all-makefiles-under,$(LOCAL_PATH))
