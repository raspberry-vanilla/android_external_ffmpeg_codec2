/*
 * Copyright (C) 2015 The CyanogenMod Project
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

#ifndef FFMPEG_COMPONENTS_H_

#define FFMPEG_COMPONENTS_H_

#include <OMX_AudioExt.h>
#include <OMX_IndexExt.h>

#include <media/stagefright/FFMPEGOmxExtn.h>

#include "config.h"
#include "libavformat/avformat.h"
#include "libavcodec/avcodec.h"

namespace android {

static const struct AudioCodingMapEntry {
    const char *mName;
    OMX_AUDIO_CODINGTYPE mAudioCodingType;
    const char *mRole;
    enum AVCodecID mCodecID;
} kAudioComponents[] = {
    { "OMX.ffmpeg.aac.decoder",
        OMX_AUDIO_CodingAAC, "audio_decoder.aac", AV_CODEC_ID_AAC },
    { "OMX.ffmpeg.ac3.decoder",
        (OMX_AUDIO_CODINGTYPE)OMX_AUDIO_CodingAC3, "audio_decoder.ac3", AV_CODEC_ID_AC3 },
    { "OMX.ffmpeg.ape.decoder",
        (OMX_AUDIO_CODINGTYPE)OMX_AUDIO_CodingAPE, "audio_decoder.ape", AV_CODEC_ID_APE },
    { "OMX.ffmpeg.atrial.decoder",
        OMX_AUDIO_CodingAutoDetect, "audio_decoder.trial", AV_CODEC_ID_NONE },
    { "OMX.ffmpeg.dts.decoder",
        (OMX_AUDIO_CODINGTYPE)OMX_AUDIO_CodingDTS, "audio_decoder.dts", AV_CODEC_ID_DTS },
    { "OMX.ffmpeg.flac.decoder",
        OMX_AUDIO_CodingFLAC, "audio_decoder.flac", AV_CODEC_ID_FLAC },
    { "OMX.ffmpeg.mp2.decoder",
        (OMX_AUDIO_CODINGTYPE)OMX_AUDIO_CodingMP2, "audio_decoder.mp2", AV_CODEC_ID_MP2 },
    { "OMX.ffmpeg.mp3.decoder",
        OMX_AUDIO_CodingMP3, "audio_decoder.mp3", AV_CODEC_ID_MP3 },
    { "OMX.ffmpeg.ra.decoder",
        OMX_AUDIO_CodingRA, "audio_decoder.ra", AV_CODEC_ID_COOK },
    { "OMX.ffmpeg.vorbis.decoder",
        OMX_AUDIO_CodingVORBIS, "audio_decoder.vorbis", AV_CODEC_ID_VORBIS },
    { "OMX.ffmpeg.wma.decoder",
        OMX_AUDIO_CodingWMA, "audio_decoder.wma", AV_CODEC_ID_WMAV2 },
    { "OMX.ffmpeg.alac.decoder",
        (OMX_AUDIO_CODINGTYPE)OMX_AUDIO_CodingALAC, "audio_decoder.alac", AV_CODEC_ID_ALAC },
};

static const struct VideoCodingMapEntry {
    const char *mName;
    OMX_VIDEO_CODINGTYPE mVideoCodingType;
    const char *mRole;
    enum AVCodecID mCodecID;
} kVideoComponents[] = {
    { "OMX.ffmpeg.divx.decoder",
        (OMX_VIDEO_CODINGTYPE)OMX_VIDEO_CodingDIVX, "video_decoder.divx", AV_CODEC_ID_MPEG4 },
    { "OMX.ffmpeg.flv1.decoder",
        (OMX_VIDEO_CODINGTYPE)OMX_VIDEO_CodingFLV1, "video_decoder.flv1", AV_CODEC_ID_FLV1 },
    { "OMX.ffmpeg.h264.decoder",
        OMX_VIDEO_CodingAVC, "video_decoder.avc", AV_CODEC_ID_H264 },
    { "OMX.ffmpeg.h263.decoder",
        OMX_VIDEO_CodingH263, "video_decoder.h263", AV_CODEC_ID_H263 },
    { "OMX.ffmpeg.hevc.decoder",
        OMX_VIDEO_CodingHEVC, "video_decoder.hevc", AV_CODEC_ID_HEVC },
    { "OMX.ffmpeg.mpeg2.decoder",
        OMX_VIDEO_CodingMPEG2, "video_decoder.mpeg2", AV_CODEC_ID_MPEG2VIDEO },
    { "OMX.ffmpeg.mpeg4.decoder",
        OMX_VIDEO_CodingMPEG4, "video_decoder.mpeg4", AV_CODEC_ID_MPEG4 },
    { "OMX.ffmpeg.rv.decoder",
        OMX_VIDEO_CodingRV, "video_decoder.rv", AV_CODEC_ID_RV40 },
    { "OMX.ffmpeg.vc1.decoder",
        (OMX_VIDEO_CODINGTYPE)OMX_VIDEO_CodingVC1, "video_decoder.vc1", AV_CODEC_ID_VC1 },
    { "OMX.ffmpeg.vp8.decoder",
        OMX_VIDEO_CodingVP8, "video_decoder.vp8", AV_CODEC_ID_VP8 },
    { "OMX.ffmpeg.vp9.decoder",
        OMX_VIDEO_CodingVP9, "video_decoder.vp9", AV_CODEC_ID_VP9 },
    { "OMX.ffmpeg.vtrial.decoder",
        OMX_VIDEO_CodingAutoDetect, "video_decoder.trial", AV_CODEC_ID_NONE },
    { "OMX.ffmpeg.wmv.decoder",
        OMX_VIDEO_CodingWMV, "video_decoder.wmv", AV_CODEC_ID_WMV2 },
};

static const size_t kNumAudioComponents =
    (sizeof(kAudioComponents) / sizeof(kAudioComponents[0]));

static const size_t kNumVideoComponents =
    (sizeof(kVideoComponents) / sizeof(kVideoComponents[0]));

static const size_t kNumComponents = kNumAudioComponents + kNumVideoComponents;

}  // namespace android

#endif  // FFMPEG_OMX_PLUGIN_H_
