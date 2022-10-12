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

#ifndef C2_FFMPEG_VIDEO_DECODE_INTERFACE_H
#define C2_FFMPEG_VIDEO_DECODE_INTERFACE_H

#include <SimpleC2Interface.h>
#include "C2FFMPEGCommon.h"
#include "codec_utils.h"

namespace android {

class C2FFMPEGVideoDecodeInterface : public SimpleInterface<void>::BaseParams {
public:
    explicit C2FFMPEGVideoDecodeInterface(
        const C2FFMPEGComponentInfo* componentInfo,
        const std::shared_ptr<C2ReflectorHelper>& helper);

    uint32_t getWidth() const { return mSize->width; }
    uint32_t getHeight() const { return mSize->height; }
    const FFMPEGVideoCodecInfo* getCodecInfo() const;
    uint64_t getConsumerUsage() const { return mConsumerUsage->value; }
    const std::shared_ptr<C2StreamPixelFormatInfo::output>&
        getPixelFormatInfo() const { return mPixelFormat; }
    uint32_t getOutputDelay() const { return mActualOutputDelay->value; }

private:
    static C2R SizeSetter(
        bool mayBlock,
        const C2P<C2StreamPictureSizeInfo::output> &oldMe,
        C2P<C2StreamPictureSizeInfo::output> &me);
    static C2R ProfileLevelSetter(
        bool mayBlock,
        C2P<C2StreamProfileLevelInfo::input> &me,
        const C2P<C2StreamPictureSizeInfo::output> &size);
    static C2R CodecSetter(
        bool mayBlock, C2P<C2StreamRawCodecDataInfo::input>& me);

private:
    std::shared_ptr<C2StreamPictureSizeInfo::output> mSize;
    std::shared_ptr<C2StreamProfileLevelInfo::input> mProfileLevel;
    std::shared_ptr<C2StreamColorInfo::output> mColorInfo;
    std::shared_ptr<C2StreamPixelFormatInfo::output> mPixelFormat;
    std::shared_ptr<C2StreamRawCodecDataInfo::input> mRawCodecData;
    std::shared_ptr<C2StreamUsageTuning::output> mConsumerUsage;
};

} // namespace android

#endif // C2_FFMPEG_VIDEO_DECODE_INTERFACE_H
