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

namespace android {

class C2FFMPEGVideoDecodeInterface : public SimpleInterface<void>::BaseParams {
public:
    explicit C2FFMPEGVideoDecodeInterface(
        const C2FFMPEGComponentInfo* componentInfo,
        const std::shared_ptr<C2ReflectorHelper>& helper);

    uint32_t getWidth() const { return mSize->width; }
    uint32_t getHeight() const { return mSize->height; }
    uint64_t getConsumerUsage() const { return mConsumerUsage->value; }
    const std::shared_ptr<C2StreamPixelFormatInfo::output>&
        getPixelFormatInfo() const { return mPixelFormat; }
    uint32_t getOutputDelay() const { return mActualOutputDelay->value; }
    bool isHevc() const { return mIsHevc; }

    // Thread-safe accessors for parameters that the client may (re)configure
    // from a binder thread while the component thread reads them.
    std::shared_ptr<C2StreamPixelFormatInfo::output> getPixelFormatInfoLocked() const {
        Lock lock = this->lock();
        return mPixelFormat;
    }
    uint32_t getPixelFormatLocked() const {
        Lock lock = this->lock();
        return mPixelFormat->value;
    }
    // Container / client supplied colour aspects (C2_PARAMKEY_DEFAULT_COLOR_ASPECTS),
    // only declared for HEVC. Returns all-unspecified otherwise.
    C2ColorAspectsStruct getDefaultColorAspectsLocked() const {
        Lock lock = this->lock();
        if (!mDefaultColorAspects) {
            return C2ColorAspectsStruct();
        }
        return C2ColorAspectsStruct(mDefaultColorAspects->range, mDefaultColorAspects->primaries,
                                    mDefaultColorAspects->transfer, mDefaultColorAspects->matrix);
    }

private:
    static C2R PixelFormatSetter(
        bool mayBlock,
        const C2P<C2StreamPixelFormatInfo::output> &oldMe,
        C2P<C2StreamPixelFormatInfo::output> &me);
    static C2R DefaultColorAspectsSetter(
        bool mayBlock,
        C2P<C2StreamColorAspectsTuning::output> &me);
    static C2R ColorAspectsSetter(
        bool mayBlock,
        C2P<C2StreamColorAspectsInfo::output> &me);
    static C2R HdrStaticInfoSetter(
        bool mayBlock,
        C2P<C2StreamHdrStaticInfo::output> &me);
    static C2R SizeSetter(
        bool mayBlock,
        const C2P<C2StreamPictureSizeInfo::output> &oldMe,
        C2P<C2StreamPictureSizeInfo::output> &me);
    static C2R ProfileLevelSetter(
        bool mayBlock,
        C2P<C2StreamProfileLevelInfo::input> &me,
        const C2P<C2StreamPictureSizeInfo::output> &size);

private:
    std::shared_ptr<C2StreamPictureSizeInfo::output> mSize;
    std::shared_ptr<C2StreamProfileLevelInfo::input> mProfileLevel;
    std::shared_ptr<C2StreamColorInfo::output> mColorInfo;
    std::shared_ptr<C2StreamPixelFormatInfo::output> mPixelFormat;
    std::shared_ptr<C2StreamUsageTuning::output> mConsumerUsage;
    std::shared_ptr<C2StreamColorAspectsTuning::output> mDefaultColorAspects;
    std::shared_ptr<C2StreamColorAspectsInfo::output> mColorAspects;
    std::shared_ptr<C2StreamHdrStaticInfo::output> mHdrStaticInfo;
    bool mIsHevc;
};

} // namespace android

#endif // C2_FFMPEG_VIDEO_DECODE_INTERFACE_H
