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

#ifndef C2_FFMPEG_AUDIO_DECODE_INTERFACE_H
#define C2_FFMPEG_AUDIO_DECODE_INTERFACE_H

#include <SimpleC2Interface.h>
#include "C2FFMPEGCommon.h"
#include "codec_utils.h"

namespace android {

class C2FFMPEGAudioDecodeInterface : public SimpleInterface<void>::BaseParams {
public:
    explicit C2FFMPEGAudioDecodeInterface(
        const C2FFMPEGComponentInfo* componentInfo,
        const std::shared_ptr<C2ReflectorHelper>& helper);

    uint32_t getSampleRate() const { return mSampleRate->value; }
    uint32_t getChannelCount() const { return mChannelCount->value; }
    uint32_t getBitrate() const { return mBitrate->value; }
    C2Config::pcm_encoding_t getPcmEncodingInfo() const { return mPcmEncodingInfo->value; }
    const FFMPEGAudioCodecInfo* getCodecInfo() const;

private:
    static C2R CodecSetter(
        bool mayBlock, C2P<C2StreamRawCodecDataInfo::input>& me);

private:
    std::shared_ptr<C2StreamSampleRateInfo::output> mSampleRate;
    std::shared_ptr<C2StreamChannelCountInfo::output> mChannelCount;
    std::shared_ptr<C2StreamBitrateInfo::input> mBitrate;
    std::shared_ptr<C2StreamPcmEncodingInfo::output> mPcmEncodingInfo;
    std::shared_ptr<C2StreamMaxBufferSizeInfo::input> mInputMaxBufSize;
    std::shared_ptr<C2StreamRawCodecDataInfo::input> mRawCodecData;
};

} // namespace android

#endif // C2_FFMPEG_AUDIO_DECODE_INTERFACE_H
