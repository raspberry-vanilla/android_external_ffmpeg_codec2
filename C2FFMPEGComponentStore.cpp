/*
 * Copyright (C) 2022 Michael Goffioul <michael.goffioul@gmail.com>
 * Copyright (C) 2025 KonstaKANG
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "C2FFMPEGComponentStore"
#include <android-base/properties.h>

#include "C2FFMPEGCommon.h"
#include "C2FFMPEGAudioDecodeComponent.h"
#include "C2FFMPEGAudioDecodeInterface.h"
#include "C2FFMPEGVideoDecodeComponent.h"
#include "C2FFMPEGVideoDecodeInterface.h"

#include "C2FFMPEGComponentStore.h"

#define RANK_DISABLED 0xFFFFFFFF

namespace android {

static const C2FFMPEGComponentInfo kFFMPEGVideoComponents[] = {
    { "c2.ffmpeg.av1.decoder"   , MEDIA_MIMETYPE_VIDEO_AV1   , AV_CODEC_ID_AV1        },
    { "c2.ffmpeg.h263.decoder"  , MEDIA_MIMETYPE_VIDEO_H263  , AV_CODEC_ID_H263       },
    { "c2.ffmpeg.h264.decoder"  , MEDIA_MIMETYPE_VIDEO_AVC   , AV_CODEC_ID_H264       },
    { "c2.ffmpeg.hevc.decoder"  , MEDIA_MIMETYPE_VIDEO_HEVC  , AV_CODEC_ID_HEVC       },
    { "c2.ffmpeg.mpeg2.decoder" , MEDIA_MIMETYPE_VIDEO_MPEG2 , AV_CODEC_ID_MPEG2VIDEO },
    { "c2.ffmpeg.mpeg4.decoder" , MEDIA_MIMETYPE_VIDEO_MPEG4 , AV_CODEC_ID_MPEG4      },
    { "c2.ffmpeg.vp8.decoder"   , MEDIA_MIMETYPE_VIDEO_VP8   , AV_CODEC_ID_VP8        },
    { "c2.ffmpeg.vp9.decoder"   , MEDIA_MIMETYPE_VIDEO_VP9   , AV_CODEC_ID_VP9        },
};

static const size_t kNumVideoComponents =
    (sizeof(kFFMPEGVideoComponents) / sizeof(kFFMPEGVideoComponents[0]));

static const C2FFMPEGComponentInfo kFFMPEGAudioComponents[] = {
    { "c2.ffmpeg.aac.decoder"   , MEDIA_MIMETYPE_AUDIO_AAC          , AV_CODEC_ID_AAC    },
    { "c2.ffmpeg.ac3.decoder"   , MEDIA_MIMETYPE_AUDIO_AC3          , AV_CODEC_ID_AC3    },
    { "c2.ffmpeg.alac.decoder"  , MEDIA_MIMETYPE_AUDIO_ALAC         , AV_CODEC_ID_ALAC   },
    { "c2.ffmpeg.flac.decoder"  , MEDIA_MIMETYPE_AUDIO_FLAC         , AV_CODEC_ID_FLAC   },
    { "c2.ffmpeg.mp2.decoder"   , MEDIA_MIMETYPE_AUDIO_MPEG_LAYER_II, AV_CODEC_ID_MP2    },
    { "c2.ffmpeg.mp3.decoder"   , MEDIA_MIMETYPE_AUDIO_MPEG         , AV_CODEC_ID_MP3    },
    { "c2.ffmpeg.vorbis.decoder", MEDIA_MIMETYPE_AUDIO_VORBIS       , AV_CODEC_ID_VORBIS },
};

static const size_t kNumAudioComponents =
    (sizeof(kFFMPEGAudioComponents) / sizeof(kFFMPEGAudioComponents[0]));

C2FFMPEGComponentStore::C2FFMPEGComponentStore()
    : mReflectorHelper(std::make_shared<C2ReflectorHelper>()),
      mInterface(mReflectorHelper) {
}

C2FFMPEGComponentStore::~C2FFMPEGComponentStore() = default;

C2String C2FFMPEGComponentStore::getName() const {
    return "ffmpeg";
}

c2_status_t C2FFMPEGComponentStore::createComponent(
        C2String name,
        std::shared_ptr<C2Component>* const component) {
    ALOGD("createComponent: %s", name.c_str());
    for (int i = 0; i < kNumAudioComponents; i++) {
        auto info = &kFFMPEGAudioComponents[i];
        if (name == info->name) {
            component->reset();
            *component = std::shared_ptr<C2Component>(
                    new C2FFMPEGAudioDecodeComponent(
                            info, std::make_shared<C2FFMPEGAudioDecodeInterface>(info, mReflectorHelper)));
            return C2_OK;
        }
    }
    for (int i = 0; i < kNumVideoComponents; i++) {
        auto info = &kFFMPEGVideoComponents[i];
        if (name == info->name) {
            component->reset();
            *component = std::shared_ptr<C2Component>(
                    new C2FFMPEGVideoDecodeComponent(
                            info, std::make_shared<C2FFMPEGVideoDecodeInterface>(info, mReflectorHelper)));
            return C2_OK;
        }
    }
    return C2_NOT_FOUND;
}

c2_status_t C2FFMPEGComponentStore::createInterface(
        C2String name,
        std::shared_ptr<C2ComponentInterface>* const interface) {
    ALOGD("createInterface: %s", name.c_str());
    for (int i = 0; i < kNumAudioComponents; i++) {
        auto info = &kFFMPEGAudioComponents[i];
        if (name == info->name) {
            interface->reset();
            *interface = std::shared_ptr<C2ComponentInterface>(
                    new SimpleInterface<C2FFMPEGAudioDecodeInterface>(
                            info->name, 0, std::make_shared<C2FFMPEGAudioDecodeInterface>(info, mReflectorHelper)));
            return C2_OK;
        }
    }
    for (int i = 0; i < kNumVideoComponents; i++) {
        auto info = &kFFMPEGVideoComponents[i];
        if (name == info->name) {
            interface->reset();
            *interface = std::shared_ptr<C2ComponentInterface>(
                    new SimpleInterface<C2FFMPEGVideoDecodeInterface>(
                            info->name, 0, std::make_shared<C2FFMPEGVideoDecodeInterface>(info, mReflectorHelper)));
            return C2_OK;
        }
    }
    ALOGE("createInterface: unknown component = %s", name.c_str());
    return C2_NOT_FOUND;
}

std::vector<std::shared_ptr<const C2Component::Traits>>
        C2FFMPEGComponentStore::listComponents() {
    std::vector<std::shared_ptr<const C2Component::Traits>> ret;
    // FIXME: Prefer OMX codecs for the time being...
    uint32_t defaultRank = ::android::base::GetUintProperty("persist.vendor.ffmpeg_codec2.rank", 0x110u);
    uint32_t defaultRankAudio = ::android::base::GetUintProperty("persist.vendor.ffmpeg_codec2.rank.audio", defaultRank);
    uint32_t defaultRankVideo = ::android::base::GetUintProperty("persist.vendor.ffmpeg_codec2.rank.video", defaultRank);
    ALOGD("listComponents: defaultRank=%x, defaultRankAudio=%x, defaultRankVideo=%x",
          defaultRank, defaultRankAudio, defaultRankVideo);
    if (defaultRank != RANK_DISABLED) {
        if (defaultRankAudio != RANK_DISABLED) {
            for (int i = 0; i < kNumAudioComponents; i++) {
                auto traits = std::make_shared<C2Component::Traits>();
                traits->name = kFFMPEGAudioComponents[i].name;
                traits->domain = C2Component::DOMAIN_AUDIO;
                traits->kind = C2Component::KIND_DECODER;
                traits->mediaType = kFFMPEGAudioComponents[i].mediaType;
                traits->rank = defaultRankAudio;
                ret.push_back(traits);
            }
        }
        if (defaultRankVideo != RANK_DISABLED) {
            for (int i = 0; i < kNumVideoComponents; i++) {
                auto traits = std::make_shared<C2Component::Traits>();
                traits->name = kFFMPEGVideoComponents[i].name;
                traits->domain = C2Component::DOMAIN_VIDEO;
                traits->kind = C2Component::KIND_DECODER;
                traits->mediaType = kFFMPEGVideoComponents[i].mediaType;
                traits->rank = defaultRankVideo;
                ret.push_back(traits);
            }
        }
    }
    return ret;
}

c2_status_t C2FFMPEGComponentStore::copyBuffer(
        std::shared_ptr<C2GraphicBuffer> /* src */,
        std::shared_ptr<C2GraphicBuffer> /* dst */) {
    return C2_OMITTED;
}

c2_status_t C2FFMPEGComponentStore::query_sm(
        const std::vector<C2Param*>& stackParams,
        const std::vector<C2Param::Index>& heapParamIndices,
        std::vector<std::unique_ptr<C2Param>>* const heapParams) const {
    return mInterface.query(stackParams, heapParamIndices, C2_MAY_BLOCK, heapParams);
}

c2_status_t C2FFMPEGComponentStore::config_sm(
        const std::vector<C2Param*>& params,
        std::vector<std::unique_ptr<C2SettingResult>>* const failures) {
    return mInterface.config(params, C2_MAY_BLOCK, failures);
}

std::shared_ptr<C2ParamReflector> C2FFMPEGComponentStore::getParamReflector() const {
    return mReflectorHelper;
}

c2_status_t C2FFMPEGComponentStore::querySupportedParams_nb(
        std::vector<std::shared_ptr<C2ParamDescriptor>>* const params) const {
    return mInterface.querySupportedParams(params);
}

c2_status_t C2FFMPEGComponentStore::querySupportedValues_sm(
        std::vector<C2FieldSupportedValuesQuery>& fields) const {
    return mInterface.querySupportedValues(fields, C2_MAY_BLOCK);
}

} // namespace android
