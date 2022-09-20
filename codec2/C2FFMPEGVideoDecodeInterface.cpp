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

#define LOG_TAG "C2FFMPEGVideoDecodeInterface"
#include <android-base/properties.h>
#include <log/log.h>
#include <thread>

#include <media/stagefright/foundation/MediaDefs.h>
#include "C2FFMPEGVideoDecodeInterface.h"

namespace android {

constexpr size_t kMaxDimension = 4080;

C2FFMPEGVideoDecodeInterface::C2FFMPEGVideoDecodeInterface(
        const C2FFMPEGComponentInfo* componentInfo,
        const std::shared_ptr<C2ReflectorHelper>& helper)
    : SimpleInterface<void>::BaseParams(
        helper,
        componentInfo->name,
        C2Component::KIND_DECODER,
        C2Component::DOMAIN_VIDEO,
        componentInfo->mediaType) {
    noPrivateBuffers();
    noInputReferences();
    noOutputReferences();
    noInputLatency();
    noTimeStretch();
    setDerivedInstance(this);

    addParameter(
            DefineParam(mAttrib, C2_PARAMKEY_COMPONENT_ATTRIBUTES)
            .withConstValue(new C2ComponentAttributesSetting(C2Component::ATTRIB_IS_TEMPORAL))
            .build());

    addParameter(
            DefineParam(mSize, C2_PARAMKEY_PICTURE_SIZE)
            .withDefault(new C2StreamPictureSizeInfo::output(0u, 320, 240))
            .withFields({
                C2F(mSize, width).inRange(16, kMaxDimension, 2),
                C2F(mSize, height).inRange(16, kMaxDimension, 2),
            })
            .withSetter(SizeSetter)
            .build());

    if (strcasecmp(componentInfo->mediaType, MEDIA_MIMETYPE_VIDEO_MPEG2) == 0) {
        addParameter(
                DefineParam(mActualOutputDelay, C2_PARAMKEY_OUTPUT_DELAY)
                .withConstValue(new C2PortActualDelayTuning::output(3u))
                .build());

        addParameter(
                DefineParam(mProfileLevel, C2_PARAMKEY_PROFILE_LEVEL)
                .withDefault(new C2StreamProfileLevelInfo::input(0u,
                        C2Config::PROFILE_MP2V_SIMPLE, C2Config::LEVEL_MP2V_HIGH))
                .withFields({
                    C2F(mProfileLevel, profile).oneOf({
                            C2Config::PROFILE_MP2V_SIMPLE,
                            C2Config::PROFILE_MP2V_MAIN}),
                    C2F(mProfileLevel, level).oneOf({
                            C2Config::LEVEL_MP2V_LOW,
                            C2Config::LEVEL_MP2V_MAIN,
                            C2Config::LEVEL_MP2V_HIGH_1440,
                            C2Config::LEVEL_MP2V_HIGH})
                })
                .withSetter(ProfileLevelSetter, mSize)
                .build());
    }

    else if (strcasecmp(componentInfo->mediaType, MEDIA_MIMETYPE_VIDEO_AVC) == 0) {
        addParameter(
                DefineParam(mActualOutputDelay, C2_PARAMKEY_OUTPUT_DELAY)
                .withDefault(new C2PortActualDelayTuning::output(8u))
                .withFields({C2F(mActualOutputDelay, value).inRange(0, 34u)})
                .withSetter(Setter<decltype(*mActualOutputDelay)>::StrictValueWithNoDeps)
                .build());

        addParameter(
                DefineParam(mProfileLevel, C2_PARAMKEY_PROFILE_LEVEL)
                .withDefault(new C2StreamProfileLevelInfo::input(0u,
                        C2Config::PROFILE_AVC_CONSTRAINED_BASELINE, C2Config::LEVEL_AVC_5_2))
                .withFields({
                    C2F(mProfileLevel, profile).oneOf({
                            C2Config::PROFILE_AVC_CONSTRAINED_BASELINE,
                            C2Config::PROFILE_AVC_BASELINE,
                            C2Config::PROFILE_AVC_MAIN,
                            C2Config::PROFILE_AVC_CONSTRAINED_HIGH,
                            C2Config::PROFILE_AVC_PROGRESSIVE_HIGH,
                            C2Config::PROFILE_AVC_HIGH}),
                    C2F(mProfileLevel, level).oneOf({
                            C2Config::LEVEL_AVC_1, C2Config::LEVEL_AVC_1B, C2Config::LEVEL_AVC_1_1,
                            C2Config::LEVEL_AVC_1_2, C2Config::LEVEL_AVC_1_3,
                            C2Config::LEVEL_AVC_2, C2Config::LEVEL_AVC_2_1, C2Config::LEVEL_AVC_2_2,
                            C2Config::LEVEL_AVC_3, C2Config::LEVEL_AVC_3_1, C2Config::LEVEL_AVC_3_2,
                            C2Config::LEVEL_AVC_4, C2Config::LEVEL_AVC_4_1, C2Config::LEVEL_AVC_4_2,
                            C2Config::LEVEL_AVC_5, C2Config::LEVEL_AVC_5_1, C2Config::LEVEL_AVC_5_2
                    })
                })
                .withSetter(ProfileLevelSetter, mSize)
                .build());
    }

    else if (strcasecmp(componentInfo->mediaType, MEDIA_MIMETYPE_VIDEO_HEVC) == 0) {
        addParameter(
                DefineParam(mActualOutputDelay, C2_PARAMKEY_OUTPUT_DELAY)
                .withDefault(new C2PortActualDelayTuning::output(8u))
                .withFields({C2F(mActualOutputDelay, value).inRange(0, 34u)})
                .withSetter(Setter<decltype(*mActualOutputDelay)>::StrictValueWithNoDeps)
                .build());

        addParameter(
                DefineParam(mProfileLevel, C2_PARAMKEY_PROFILE_LEVEL)
                .withDefault(new C2StreamProfileLevelInfo::input(0u,
                        C2Config::PROFILE_HEVC_MAIN, C2Config::LEVEL_HEVC_MAIN_5_1))
                .withFields({
                    C2F(mProfileLevel, profile).oneOf({
                            C2Config::PROFILE_HEVC_MAIN,
                            C2Config::PROFILE_HEVC_MAIN_STILL}),
                    C2F(mProfileLevel, level).oneOf({
                            C2Config::LEVEL_HEVC_MAIN_1,
                            C2Config::LEVEL_HEVC_MAIN_2, C2Config::LEVEL_HEVC_MAIN_2_1,
                            C2Config::LEVEL_HEVC_MAIN_3, C2Config::LEVEL_HEVC_MAIN_3_1,
                            C2Config::LEVEL_HEVC_MAIN_4, C2Config::LEVEL_HEVC_MAIN_4_1,
                            C2Config::LEVEL_HEVC_MAIN_5, C2Config::LEVEL_HEVC_MAIN_5_1,
                            C2Config::LEVEL_HEVC_MAIN_5_2, C2Config::LEVEL_HEVC_HIGH_4,
                            C2Config::LEVEL_HEVC_HIGH_4_1, C2Config::LEVEL_HEVC_HIGH_5,
                            C2Config::LEVEL_HEVC_HIGH_5_1, C2Config::LEVEL_HEVC_HIGH_5_2
                    })
                })
                .withSetter(ProfileLevelSetter, mSize)
                .build());
    }

    else {
        int nthreads = base::GetIntProperty("debug.ffmpeg-codec2.threads", 0);

        if (nthreads <= 0) {
            nthreads = std::thread::hardware_concurrency();
        }

        addParameter(
                DefineParam(mActualOutputDelay, C2_PARAMKEY_OUTPUT_DELAY)
                .withConstValue(new C2PortActualDelayTuning::output(2 * nthreads))
                .build());

        if (strcasecmp(componentInfo->mediaType, MEDIA_MIMETYPE_VIDEO_VP9) == 0) {
            addParameter(
                    DefineParam(mProfileLevel, C2_PARAMKEY_PROFILE_LEVEL)
                    .withDefault(new C2StreamProfileLevelInfo::input(0u,
                            C2Config::PROFILE_VP9_0, C2Config::LEVEL_VP9_5))
                    .withFields({
                        C2F(mProfileLevel, profile).oneOf({
                                C2Config::PROFILE_VP9_0,
                                C2Config::PROFILE_VP9_2}),
                        C2F(mProfileLevel, level).oneOf({
                                C2Config::LEVEL_VP9_1,
                                C2Config::LEVEL_VP9_1_1,
                                C2Config::LEVEL_VP9_2,
                                C2Config::LEVEL_VP9_2_1,
                                C2Config::LEVEL_VP9_3,
                                C2Config::LEVEL_VP9_3_1,
                                C2Config::LEVEL_VP9_4,
                                C2Config::LEVEL_VP9_4_1,
                                C2Config::LEVEL_VP9_5,
                        })
                    })
                    .withSetter(ProfileLevelSetter, mSize)
                    .build());
        }
    }

    C2ChromaOffsetStruct locations[1] = { C2ChromaOffsetStruct::ITU_YUV_420_0() };
    std::shared_ptr<C2StreamColorInfo::output> defaultColorInfo =
        C2StreamColorInfo::output::AllocShared(
                1u, 0u, 8u /* bitDepth */, C2Color::YUV_420);
    memcpy(defaultColorInfo->m.locations, locations, sizeof(locations));

    defaultColorInfo =
        C2StreamColorInfo::output::AllocShared(
                { C2ChromaOffsetStruct::ITU_YUV_420_0() },
                0u, 8u /* bitDepth */, C2Color::YUV_420);
    helper->addStructDescriptors<C2ChromaOffsetStruct>();

    addParameter(
            DefineParam(mColorInfo, C2_PARAMKEY_CODED_COLOR_INFO)
            .withConstValue(defaultColorInfo)
            .build());

    addParameter(
            DefineParam(mPixelFormat, C2_PARAMKEY_PIXEL_FORMAT)
            .withConstValue(new C2StreamPixelFormatInfo::output(
                                 0u, HAL_PIXEL_FORMAT_YV12))
            .build());

    addParameter(
            DefineParam(mRawCodecData, C2_PARAMKEY_RAW_CODEC_DATA)
            .withDefault(C2StreamRawCodecDataInfo::input::AllocShared(0, 0u))
            .withFields({C2F(mRawCodecData, m.value)})
            .withSetter(CodecSetter)
            .build());
}

C2R C2FFMPEGVideoDecodeInterface::SizeSetter(
        bool /* mayBlock */,
        const C2P<C2StreamPictureSizeInfo::output> &oldMe,
        C2P<C2StreamPictureSizeInfo::output> &me) {
    C2R res = C2R::Ok();

    if (!me.F(me.v.width).supportsAtAll(me.v.width)) {
        res = res.plus(C2SettingResultBuilder::BadValue(me.F(me.v.width)));
        me.set().width = oldMe.v.width;
    }
    if (!me.F(me.v.height).supportsAtAll(me.v.height)) {
        res = res.plus(C2SettingResultBuilder::BadValue(me.F(me.v.height)));
        me.set().height = oldMe.v.height;
    }

    return res;
}

C2R C2FFMPEGVideoDecodeInterface::ProfileLevelSetter(
        bool /* mayBlock */,
        C2P<C2StreamProfileLevelInfo::input>& /* me */,
        const C2P<C2StreamPictureSizeInfo::output>& /* size */) {
    return C2R::Ok();
}

C2R C2FFMPEGVideoDecodeInterface::CodecSetter(
        bool mayBlock __unused, C2P<C2StreamRawCodecDataInfo::input>& me __unused) {
    return C2R::Ok();
}

const FFMPEGVideoCodecInfo* C2FFMPEGVideoDecodeInterface::getCodecInfo() const {
    if (mRawCodecData->flexCount() == sizeof(FFMPEGVideoCodecInfo)) {
        return (const FFMPEGVideoCodecInfo*)mRawCodecData->m.value;
    }
    return nullptr;
}

} // namespace android
