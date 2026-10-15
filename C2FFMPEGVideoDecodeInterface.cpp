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
    mIsHevc = strcasecmp(componentInfo->mediaType, MEDIA_MIMETYPE_VIDEO_HEVC) == 0;
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
                            C2Config::PROFILE_HEVC_MAIN_10,
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

    else if (strcasecmp(componentInfo->mediaType, MEDIA_MIMETYPE_VIDEO_AV1) == 0) {
        addParameter(
                DefineParam(mActualOutputDelay, C2_PARAMKEY_OUTPUT_DELAY)
                .withDefault(new C2PortActualDelayTuning::output(8u))
                .withFields({C2F(mActualOutputDelay, value).inRange(0, 34u)})
                .withSetter(Setter<decltype(*mActualOutputDelay)>::StrictValueWithNoDeps)
                .build());

        addParameter(
                DefineParam(mProfileLevel, C2_PARAMKEY_PROFILE_LEVEL)
                .withDefault(new C2StreamProfileLevelInfo::input(0u,
                        C2Config::PROFILE_AV1_0, C2Config::LEVEL_AV1_2_1))
                .withFields({
                    C2F(mProfileLevel, profile).oneOf({
                            C2Config::PROFILE_AV1_0,
                            C2Config::PROFILE_AV1_1}),
                    C2F(mProfileLevel, level).oneOf({
                            C2Config::LEVEL_AV1_2, C2Config::LEVEL_AV1_2_1,
                            C2Config::LEVEL_AV1_2_2, C2Config::LEVEL_AV1_2_3,
                            C2Config::LEVEL_AV1_3, C2Config::LEVEL_AV1_3_1,
                            C2Config::LEVEL_AV1_3_2, C2Config::LEVEL_AV1_3_3,
                            C2Config::LEVEL_AV1_4, C2Config::LEVEL_AV1_4_1,
                            C2Config::LEVEL_AV1_4_2, C2Config::LEVEL_AV1_4_3,
                            C2Config::LEVEL_AV1_5, C2Config::LEVEL_AV1_5_1,
                            C2Config::LEVEL_AV1_5_2, C2Config::LEVEL_AV1_5_3
                    })
                })
                .withSetter(ProfileLevelSetter, mSize)
                .build());
    }

    else {
        int nthreads = base::GetIntProperty("debug.ffmpeg_codec2.threads", 0);

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

    if (mIsHevc) {
        // HEVC Main 10 can be output as 10-bit P010 (needed for HDR10/HLG).
        // IMPLEMENTATION_DEFINED must be listed: the framework configures it
        // when the output is a Surface, and it is the only way for the
        // component to know that a 10-bit format may be chosen.
        // YCBCR_420_888 is what CCodec configures for ByteBuffer mode
        // (COLOR_FormatYUV420Flexible/Planar/SemiPlanar). The setter is lenient:
        // a strict setter would turn any other requested value into
        // C2SettingResult::BAD_VALUE, which makes CCodec::configure() fail
        // (the previous const parameter only produced a non-fatal READ_ONLY).
        addParameter(
                DefineParam(mPixelFormat, C2_PARAMKEY_PIXEL_FORMAT)
                .withDefault(new C2StreamPixelFormatInfo::output(
                                     0u, HAL_PIXEL_FORMAT_YV12))
                .withFields({C2F(mPixelFormat, value).oneOf({
                        HAL_PIXEL_FORMAT_YV12,
                        HAL_PIXEL_FORMAT_YCBCR_420_888,
                        HAL_PIXEL_FORMAT_YCBCR_P010,
                        HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED})})
                .withSetter(PixelFormatSetter)
                .build());

        // Colour aspects coming from the container / client (KEY_COLOR_*),
        // used for every field the bitstream (VUI) leaves unspecified.
        addParameter(
                DefineParam(mDefaultColorAspects, C2_PARAMKEY_DEFAULT_COLOR_ASPECTS)
                .withDefault(new C2StreamColorAspectsTuning::output(
                        0u, C2Color::RANGE_UNSPECIFIED, C2Color::PRIMARIES_UNSPECIFIED,
                        C2Color::TRANSFER_UNSPECIFIED, C2Color::MATRIX_UNSPECIFIED))
                .withFields({
                    C2F(mDefaultColorAspects, range).inRange(
                            C2Color::RANGE_UNSPECIFIED, C2Color::RANGE_OTHER),
                    C2F(mDefaultColorAspects, primaries).inRange(
                            C2Color::PRIMARIES_UNSPECIFIED, C2Color::PRIMARIES_OTHER),
                    C2F(mDefaultColorAspects, transfer).inRange(
                            C2Color::TRANSFER_UNSPECIFIED, C2Color::TRANSFER_OTHER),
                    C2F(mDefaultColorAspects, matrix).inRange(
                            C2Color::MATRIX_UNSPECIFIED, C2Color::MATRIX_OTHER)
                })
                .withSetter(DefaultColorAspectsSetter)
                .build());

        // Colour aspects and HDR static info of the decoded stream, reported
        // to the framework through configUpdate once the first frame is known.
        addParameter(
                DefineParam(mColorAspects, C2_PARAMKEY_COLOR_ASPECTS)
                .withDefault(new C2StreamColorAspectsInfo::output(
                        0u, C2Color::RANGE_UNSPECIFIED, C2Color::PRIMARIES_UNSPECIFIED,
                        C2Color::TRANSFER_UNSPECIFIED, C2Color::MATRIX_UNSPECIFIED))
                .withFields({
                    C2F(mColorAspects, range).inRange(
                            C2Color::RANGE_UNSPECIFIED, C2Color::RANGE_OTHER),
                    C2F(mColorAspects, primaries).inRange(
                            C2Color::PRIMARIES_UNSPECIFIED, C2Color::PRIMARIES_OTHER),
                    C2F(mColorAspects, transfer).inRange(
                            C2Color::TRANSFER_UNSPECIFIED, C2Color::TRANSFER_OTHER),
                    C2F(mColorAspects, matrix).inRange(
                            C2Color::MATRIX_UNSPECIFIED, C2Color::MATRIX_OTHER)
                })
                .withSetter(ColorAspectsSetter)
                .build());

        helper->addStructDescriptors<C2MasteringDisplayColorVolumeStruct, C2ColorXyStruct>();
        addParameter(
                DefineParam(mHdrStaticInfo, C2_PARAMKEY_HDR_STATIC_INFO)
                .withDefault(new C2StreamHdrStaticInfo::output(0u))
                .withFields({
                    C2F(mHdrStaticInfo, mastering.red.x).any(),
                    C2F(mHdrStaticInfo, mastering.red.y).any(),
                    C2F(mHdrStaticInfo, mastering.green.x).any(),
                    C2F(mHdrStaticInfo, mastering.green.y).any(),
                    C2F(mHdrStaticInfo, mastering.blue.x).any(),
                    C2F(mHdrStaticInfo, mastering.blue.y).any(),
                    C2F(mHdrStaticInfo, mastering.white.x).any(),
                    C2F(mHdrStaticInfo, mastering.white.y).any(),
                    C2F(mHdrStaticInfo, mastering.maxLuminance).any(),
                    C2F(mHdrStaticInfo, mastering.minLuminance).any(),
                    C2F(mHdrStaticInfo, maxCll).any(),
                    C2F(mHdrStaticInfo, maxFall).any(),
                })
                .withSetter(HdrStaticInfoSetter)
                .build());
    } else {
        addParameter(
                DefineParam(mPixelFormat, C2_PARAMKEY_PIXEL_FORMAT)
                .withConstValue(new C2StreamPixelFormatInfo::output(
                                     0u, HAL_PIXEL_FORMAT_YV12))
                .build());
    }

    addParameter(
            DefineParam(mConsumerUsage, C2_PARAMKEY_OUTPUT_STREAM_USAGE)
            .withDefault(new C2StreamUsageTuning::output(
                                0u, GRALLOC_USAGE_HW_TEXTURE | GRALLOC_USAGE_HW_COMPOSER))
            .withFields({C2F(mConsumerUsage, value).any()})
            .withSetter(Setter<decltype(*mConsumerUsage)>::StrictValueWithNoDeps)
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

C2R C2FFMPEGVideoDecodeInterface::PixelFormatSetter(
        bool /* mayBlock */,
        const C2P<C2StreamPixelFormatInfo::output> &oldMe,
        C2P<C2StreamPixelFormatInfo::output> &me) {
    // Never fail: unknown formats (e.g. RGB requested by a thumbnailer) keep
    // the previous value; the component then outputs YV12 as it always did.
    if (!me.F(me.v.value).supportsAtAll(me.v.value)) {
        me.set().value = oldMe.v.value;
    }
    return C2R::Ok();
}

C2R C2FFMPEGVideoDecodeInterface::DefaultColorAspectsSetter(
        bool /* mayBlock */,
        C2P<C2StreamColorAspectsTuning::output> &me) {
    if (me.v.range > C2Color::RANGE_OTHER) {
        me.set().range = C2Color::RANGE_OTHER;
    }
    if (me.v.primaries > C2Color::PRIMARIES_OTHER) {
        me.set().primaries = C2Color::PRIMARIES_OTHER;
    }
    if (me.v.transfer > C2Color::TRANSFER_OTHER) {
        me.set().transfer = C2Color::TRANSFER_OTHER;
    }
    if (me.v.matrix > C2Color::MATRIX_OTHER) {
        me.set().matrix = C2Color::MATRIX_OTHER;
    }
    return C2R::Ok();
}

C2R C2FFMPEGVideoDecodeInterface::ColorAspectsSetter(
        bool /* mayBlock */,
        C2P<C2StreamColorAspectsInfo::output> &me) {
    if (me.v.range > C2Color::RANGE_OTHER) {
        me.set().range = C2Color::RANGE_OTHER;
    }
    if (me.v.primaries > C2Color::PRIMARIES_OTHER) {
        me.set().primaries = C2Color::PRIMARIES_OTHER;
    }
    if (me.v.transfer > C2Color::TRANSFER_OTHER) {
        me.set().transfer = C2Color::TRANSFER_OTHER;
    }
    if (me.v.matrix > C2Color::MATRIX_OTHER) {
        me.set().matrix = C2Color::MATRIX_OTHER;
    }
    return C2R::Ok();
}

C2R C2FFMPEGVideoDecodeInterface::HdrStaticInfoSetter(
        bool /* mayBlock */,
        C2P<C2StreamHdrStaticInfo::output> &me __unused) {
    return C2R::Ok();
}

C2R C2FFMPEGVideoDecodeInterface::ProfileLevelSetter(
        bool /* mayBlock */,
        C2P<C2StreamProfileLevelInfo::input>& /* me */,
        const C2P<C2StreamPictureSizeInfo::output>& /* size */) {
    return C2R::Ok();
}

} // namespace android
