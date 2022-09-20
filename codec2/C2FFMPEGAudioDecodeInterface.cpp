/*
 * Copyright 2022 Michael Goffioul <michael.goffioul@gmail.com>
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

#define LOG_TAG "C2FFMPEGAudioDecodeInterface"
#include <log/log.h>

#include <media/stagefright/foundation/MediaDefs.h>
#include "C2FFMPEGAudioDecodeInterface.h"

#define MAX_CHANNEL_COUNT 8

namespace android {

constexpr size_t kDefaultOutputPortDelay = 2;
constexpr size_t kMaxOutputPortDelay = 16;

C2FFMPEGAudioDecodeInterface::C2FFMPEGAudioDecodeInterface(
        const C2FFMPEGComponentInfo* componentInfo,
        const std::shared_ptr<C2ReflectorHelper>& helper)
    : SimpleInterface<void>::BaseParams(
        helper,
        componentInfo->name,
        C2Component::KIND_DECODER,
        C2Component::DOMAIN_AUDIO,
        componentInfo->mediaType) {
    noPrivateBuffers();
    noInputReferences();
    noOutputReferences();
    noInputLatency();
    noTimeStretch();
    setDerivedInstance(this);

    addParameter(
            DefineParam(mActualOutputDelay, C2_PARAMKEY_OUTPUT_DELAY)
            .withDefault(new C2PortActualDelayTuning::output(kDefaultOutputPortDelay))
            .withFields({C2F(mActualOutputDelay, value).inRange(0, kMaxOutputPortDelay)})
            .withSetter(Setter<decltype(*mActualOutputDelay)>::StrictValueWithNoDeps)
            .build());

    addParameter(
            DefineParam(mSampleRate, C2_PARAMKEY_SAMPLE_RATE)
            .withDefault(new C2StreamSampleRateInfo::output(0u, 44100))
            .withFields({C2F(mSampleRate, value).oneOf({
                7350, 8000, 11025, 12000, 16000, 22050, 24000, 32000,
                44100, 48000, 64000, 88200, 96000, 192000
            })})
            .withSetter(Setter<decltype(*mSampleRate)>::NonStrictValueWithNoDeps)
            .build());

    addParameter(
            DefineParam(mBitrate, C2_PARAMKEY_BITRATE)
            .withDefault(new C2StreamBitrateInfo::input(0u, 64000))
            .withFields({C2F(mBitrate, value).inRange(8000, 320000)})
            .withSetter(Setter<decltype(*mBitrate)>::NonStrictValueWithNoDeps)
            .build());

    addParameter(
            DefineParam(mChannelCount, C2_PARAMKEY_CHANNEL_COUNT)
            .withDefault(new C2StreamChannelCountInfo::output(0u, 2))
            .withFields({C2F(mChannelCount, value).inRange(1, MAX_CHANNEL_COUNT)})
            .withSetter(Setter<decltype(*mChannelCount)>::StrictValueWithNoDeps)
            .build());

    addParameter(
            DefineParam(mPcmEncodingInfo, C2_PARAMKEY_PCM_ENCODING)
            .withDefault(new C2StreamPcmEncodingInfo::output(0u, C2Config::PCM_16))
            .withFields({C2F(mPcmEncodingInfo, value).oneOf({
                 C2Config::PCM_16,
                 C2Config::PCM_8,
                 C2Config::PCM_FLOAT,
                 C2Config::PCM_32})
            })
            .withSetter((Setter<decltype(*mPcmEncodingInfo)>::StrictValueWithNoDeps))
            .build());

    if (strcasecmp(componentInfo->mediaType, MEDIA_MIMETYPE_AUDIO_WMA) == 0) {
        addParameter(
                DefineParam(mInputMaxBufSize, C2_PARAMKEY_INPUT_MAX_BUFFER_SIZE)
                .withConstValue(new C2StreamMaxBufferSizeInfo::input(0u, 32768))
                .build());
    }

    addParameter(
            DefineParam(mRawCodecData, C2_PARAMKEY_RAW_CODEC_DATA)
            .withDefault(C2StreamRawCodecDataInfo::input::AllocShared(0, 0u))
            .withFields({C2F(mRawCodecData, m.value)})
            .withSetter(CodecSetter)
            .build());
}

C2R C2FFMPEGAudioDecodeInterface::CodecSetter(
        bool mayBlock __unused, C2P<C2StreamRawCodecDataInfo::input>& me __unused) {
    return C2R::Ok();
}

const FFMPEGAudioCodecInfo* C2FFMPEGAudioDecodeInterface::getCodecInfo() const {
    if (mRawCodecData->flexCount() == sizeof(FFMPEGAudioCodecInfo)) {
        return (const FFMPEGAudioCodecInfo*)mRawCodecData->m.value;
    }
    return nullptr;
}

} // namespace android
