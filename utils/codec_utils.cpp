/*
 * Copyright 2012 Michael Chen <omxcodec@gmail.com>
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

#define LOG_TAG "codec_utils"
#include <utils/Log.h>

extern "C" {

#include "config.h"
#include "libavcodec/xiph.h"
#include "libavutil/intreadwrite.h"

}

#include <utils/Errors.h>
#include <media/NdkMediaFormat.h>
#include <media/stagefright/foundation/ABitReader.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/avc_utils.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MetaDataUtils.h>

#include "codec_utils.h"

namespace android {

static void EncodeSize14(uint8_t **_ptr, size_t size) {
    CHECK_LE(size, 0x3fffu);

    uint8_t *ptr = *_ptr;

    *ptr++ = 0x80 | (size >> 7);
    *ptr++ = size & 0x7f;

    *_ptr = ptr;
}

static sp<ABuffer> MakeMPEGVideoESDS(const sp<ABuffer> &csd) {
    sp<ABuffer> esds = new ABuffer(csd->size() + 25);

    uint8_t *ptr = esds->data();
    *ptr++ = 0x03;
    EncodeSize14(&ptr, 22 + csd->size());

    *ptr++ = 0x00;  // ES_ID
    *ptr++ = 0x00;

    *ptr++ = 0x00;  // streamDependenceFlag, URL_Flag, OCRstreamFlag

    *ptr++ = 0x04;
    EncodeSize14(&ptr, 16 + csd->size());

    *ptr++ = 0x40;  // Audio ISO/IEC 14496-3

    for (size_t i = 0; i < 12; ++i) {
        *ptr++ = 0x00;
    }

    *ptr++ = 0x05;
    EncodeSize14(&ptr, csd->size());

    memcpy(ptr, csd->data(), csd->size());

    return esds;
}

//video

//H.264 Video Types
//http://msdn.microsoft.com/en-us/library/dd757808(v=vs.85).aspx

// H.264 bitstream without start codes.
media_status_t setAVCFormat(AVCodecParameters *avpar, AMediaFormat *meta)
{
    ALOGV("AVC");

    CHECK_GT(avpar->extradata_size, 0);
    CHECK_EQ((int)avpar->extradata[0], 1); //configurationVersion

    if (avpar->width == 0 || avpar->height == 0) {
         int32_t width, height;
         sp<ABuffer> seqParamSet = new ABuffer(avpar->extradata_size - 8);
         memcpy(seqParamSet->data(), avpar->extradata + 8, avpar->extradata_size - 8);
         FindAVCDimensions(seqParamSet, &width, &height);
         avpar->width  = width;
         avpar->height = height;
     }

    AMediaFormat_setString(meta, AMEDIAFORMAT_KEY_MIME, MEDIA_MIMETYPE_VIDEO_AVC);
    AMediaFormat_setBuffer(meta, AMEDIAFORMAT_KEY_CSD_AVC, avpar->extradata, avpar->extradata_size);

    return AMEDIA_OK;
}

// H.264 bitstream with start codes.
media_status_t setH264Format(AVCodecParameters *avpar, AMediaFormat *meta)
{
    ALOGV("H264");

    CHECK_NE((int)avpar->extradata[0], 1); //configurationVersion

    if (!MakeAVCCodecSpecificData(meta, avpar->extradata, avpar->extradata_size))
      return AMEDIA_ERROR_UNKNOWN;

    return AMEDIA_OK;
}

media_status_t setMPEG4Format(AVCodecParameters *avpar, AMediaFormat *meta)
{
    ALOGV("MPEG4");

    sp<ABuffer> csd = new ABuffer(avpar->extradata_size);
    memcpy(csd->data(), avpar->extradata, avpar->extradata_size);
    sp<ABuffer> esds = MakeMPEGVideoESDS(csd);

    AMediaFormat_setBuffer(meta, AMEDIAFORMAT_KEY_ESDS, esds->data(), esds->size());

    int divxVersion = getDivXVersion(avpar);
    if (divxVersion >= 0) {
        AMediaFormat_setString(meta, AMEDIAFORMAT_KEY_MIME, MEDIA_MIMETYPE_VIDEO_DIVX);
        AMediaFormat_setInt32(meta, "divx-version", divxVersion);
    } else {
        AMediaFormat_setString(meta, AMEDIAFORMAT_KEY_MIME, MEDIA_MIMETYPE_VIDEO_MPEG4);
    }
    return AMEDIA_OK;
}

media_status_t setH263Format(AVCodecParameters *avpar __unused, AMediaFormat *meta)
{
    ALOGV("H263");

    AMediaFormat_setString(meta, AMEDIAFORMAT_KEY_MIME, MEDIA_MIMETYPE_VIDEO_H263);

    return AMEDIA_OK;
}

media_status_t setMPEG2VIDEOFormat(AVCodecParameters *avpar, AMediaFormat *meta)
{
    ALOGV("MPEG%uVIDEO", avpar->codec_id == AV_CODEC_ID_MPEG2VIDEO ? 2 : 1);

    sp<ABuffer> csd = new ABuffer(avpar->extradata_size);
    memcpy(csd->data(), avpar->extradata, avpar->extradata_size);
    sp<ABuffer> esds = MakeMPEGVideoESDS(csd);

    AMediaFormat_setBuffer(meta, AMEDIAFORMAT_KEY_ESDS, esds->data(), esds->size());
    AMediaFormat_setString(meta, AMEDIAFORMAT_KEY_MIME, MEDIA_MIMETYPE_VIDEO_MPEG2);

    return AMEDIA_OK;
}

media_status_t setVC1Format(AVCodecParameters *avpar, AMediaFormat *meta)
{
    ALOGV("VC1");

    AMediaFormat_setString(meta, AMEDIAFORMAT_KEY_MIME, MEDIA_MIMETYPE_VIDEO_VC1);
    AMediaFormat_setBuffer(meta, "raw-codec-specific-data", avpar->extradata, avpar->extradata_size);

    return AMEDIA_OK;
}

media_status_t setWMV1Format(AVCodecParameters *avpar __unused, AMediaFormat *meta)
{
    ALOGV("WMV1");

    AMediaFormat_setString(meta, AMEDIAFORMAT_KEY_MIME, MEDIA_MIMETYPE_VIDEO_WMV);
    AMediaFormat_setInt32(meta, "wmv-version", kTypeWMVVer_7);

    return AMEDIA_OK;
}

media_status_t setWMV2Format(AVCodecParameters *avpar, AMediaFormat *meta)
{
    ALOGV("WMV2");

    AMediaFormat_setString(meta, AMEDIAFORMAT_KEY_MIME, MEDIA_MIMETYPE_VIDEO_WMV);
    AMediaFormat_setBuffer(meta, "raw-codec-specific-data", avpar->extradata, avpar->extradata_size);
    AMediaFormat_setInt32(meta, "wmv-version", kTypeWMVVer_8);

    return AMEDIA_OK;
}

media_status_t setWMV3Format(AVCodecParameters *avpar, AMediaFormat *meta)
{
    ALOGV("WMV3");

    AMediaFormat_setString(meta, AMEDIAFORMAT_KEY_MIME, MEDIA_MIMETYPE_VIDEO_WMV);
    AMediaFormat_setBuffer(meta, "raw-codec-specific-data", avpar->extradata, avpar->extradata_size);
    AMediaFormat_setInt32(meta, "wmv-version", kTypeWMVVer_9);

    return AMEDIA_OK;
}

media_status_t setRV20Format(AVCodecParameters *avpar, AMediaFormat *meta)
{
    ALOGV("RV20");

    AMediaFormat_setString(meta, AMEDIAFORMAT_KEY_MIME, MEDIA_MIMETYPE_VIDEO_RV);
    AMediaFormat_setBuffer(meta, "raw-codec-specific-data", avpar->extradata, avpar->extradata_size);
    AMediaFormat_setInt32(meta, "rv-version", kTypeRVVer_G2); //http://en.wikipedia.org/wiki/RealVide

    return AMEDIA_OK;
}

media_status_t setRV30Format(AVCodecParameters *avpar, AMediaFormat *meta)
{
    ALOGV("RV30");

    AMediaFormat_setString(meta, AMEDIAFORMAT_KEY_MIME, MEDIA_MIMETYPE_VIDEO_RV);
    AMediaFormat_setBuffer(meta, "raw-codec-specific-data", avpar->extradata, avpar->extradata_size);
    AMediaFormat_setInt32(meta, "rv-version", kTypeRVVer_8); //http://en.wikipedia.org/wiki/RealVide

    return AMEDIA_OK;
}

media_status_t setRV40Format(AVCodecParameters *avpar, AMediaFormat *meta)
{
    ALOGV("RV40");

    AMediaFormat_setString(meta, AMEDIAFORMAT_KEY_MIME, MEDIA_MIMETYPE_VIDEO_RV);
    AMediaFormat_setBuffer(meta, "raw-codec-specific-data", avpar->extradata, avpar->extradata_size);
    AMediaFormat_setInt32(meta, "rv-version", kTypeRVVer_9); //http://en.wikipedia.org/wiki/RealVide

    return AMEDIA_OK;
}

media_status_t setFLV1Format(AVCodecParameters *avpar, AMediaFormat *meta)
{
    ALOGV("FLV1(Sorenson H263)");

    AMediaFormat_setString(meta, AMEDIAFORMAT_KEY_MIME, MEDIA_MIMETYPE_VIDEO_FLV1);
    AMediaFormat_setBuffer(meta, "raw-codec-specific-data", avpar->extradata, avpar->extradata_size);

    return AMEDIA_OK;
}

media_status_t setHEVCFormat(AVCodecParameters *avpar, AMediaFormat *meta)
{
    ALOGV("HEVC");

    AMediaFormat_setString(meta, AMEDIAFORMAT_KEY_MIME, MEDIA_MIMETYPE_VIDEO_HEVC);
    AMediaFormat_setBuffer(meta, AMEDIAFORMAT_KEY_CSD_HEVC, avpar->extradata, avpar->extradata_size);

    return AMEDIA_OK;
}

media_status_t setVP8Format(AVCodecParameters *avpar __unused, AMediaFormat *meta)
{
    ALOGV("VP8");

    AMediaFormat_setString(meta, AMEDIAFORMAT_KEY_MIME, MEDIA_MIMETYPE_VIDEO_VP8);

    return AMEDIA_OK;
}

media_status_t setVP9Format(AVCodecParameters *avpar __unused, AMediaFormat *meta)
{
    ALOGV("VP9");

    AMediaFormat_setString(meta, AMEDIAFORMAT_KEY_MIME, MEDIA_MIMETYPE_VIDEO_VP9);

    return AMEDIA_OK;
}

//audio

media_status_t setMP2Format(AVCodecParameters *avpar __unused, AMediaFormat *meta)
{
    ALOGV("MP2");

    AMediaFormat_setString(meta, AMEDIAFORMAT_KEY_MIME, MEDIA_MIMETYPE_AUDIO_MPEG_LAYER_II);

    return AMEDIA_OK;
}

media_status_t setMP3Format(AVCodecParameters *avpar __unused, AMediaFormat *meta)
{
    ALOGV("MP3");

    AMediaFormat_setString(meta, AMEDIAFORMAT_KEY_MIME, MEDIA_MIMETYPE_AUDIO_MPEG);

    return AMEDIA_OK;
}

media_status_t setVORBISFormat(AVCodecParameters *avpar, AMediaFormat *meta)
{
    ALOGV("VORBIS");

    const uint8_t *header_start[3];
    int header_len[3];
    if (avpriv_split_xiph_headers(avpar->extradata,
                avpar->extradata_size, 30,
                header_start, header_len) < 0) {
        ALOGE("vorbis extradata corrupt.");
        return AMEDIA_ERROR_UNKNOWN;
    }

    AMediaFormat_setString(meta, AMEDIAFORMAT_KEY_MIME, MEDIA_MIMETYPE_AUDIO_VORBIS);
    //identification header
    AMediaFormat_setBuffer(meta, AMEDIAFORMAT_KEY_CSD_0, header_start[0], header_len[0]);
    //setup header
    AMediaFormat_setBuffer(meta, AMEDIAFORMAT_KEY_CSD_1, header_start[2], header_len[2]);

    return AMEDIA_OK;
}

media_status_t setAC3Format(AVCodecParameters *avpar __unused, AMediaFormat *meta)
{
    ALOGV("AC3");

    AMediaFormat_setString(meta, AMEDIAFORMAT_KEY_MIME, MEDIA_MIMETYPE_AUDIO_AC3);

    return AMEDIA_OK;
}

media_status_t setAACFormat(AVCodecParameters *avpar, AMediaFormat *meta)
{
    ALOGV("AAC");

    AMediaFormat_setString(meta, AMEDIAFORMAT_KEY_MIME, MEDIA_MIMETYPE_AUDIO_AAC);
    AMediaFormat_setBuffer(meta, "raw-codec-specific-data", avpar->extradata, avpar->extradata_size);
    AMediaFormat_setInt32(meta, AMEDIAFORMAT_KEY_AAC_PROFILE, avpar->profile + 1);

    return AMEDIA_OK;
}

media_status_t setWMAV1Format(AVCodecParameters *avpar, AMediaFormat *meta)
{
    ALOGV("WMAV1");

    AMediaFormat_setString(meta, AMEDIAFORMAT_KEY_MIME, MEDIA_MIMETYPE_AUDIO_WMA);
    AMediaFormat_setBuffer(meta, "raw-codec-specific-data", avpar->extradata, avpar->extradata_size);
    AMediaFormat_setInt32(meta, "wma-version", kTypeWMA); //FIXME version?

    return AMEDIA_OK;
}

media_status_t setWMAV2Format(AVCodecParameters *avpar, AMediaFormat *meta)
{
    ALOGV("WMAV2");

    AMediaFormat_setString(meta, AMEDIAFORMAT_KEY_MIME, MEDIA_MIMETYPE_AUDIO_WMA);
    AMediaFormat_setBuffer(meta, "raw-codec-specific-data", avpar->extradata, avpar->extradata_size);
    AMediaFormat_setInt32(meta, "wma-version", kTypeWMA); //FIXME version?

    return AMEDIA_OK;
}

media_status_t setWMAProFormat(AVCodecParameters *avpar, AMediaFormat *meta)
{
    ALOGV("WMAPro");

    AMediaFormat_setString(meta, AMEDIAFORMAT_KEY_MIME, MEDIA_MIMETYPE_AUDIO_WMA);
    AMediaFormat_setBuffer(meta, "raw-codec-specific-data", avpar->extradata, avpar->extradata_size);
    AMediaFormat_setInt32(meta, "wma-version", kTypeWMAPro);

    return AMEDIA_OK;
}

media_status_t setWMALossLessFormat(AVCodecParameters *avpar, AMediaFormat *meta)
{
    ALOGV("WMALOSSLESS");

    AMediaFormat_setString(meta, AMEDIAFORMAT_KEY_MIME, MEDIA_MIMETYPE_AUDIO_WMA);
    AMediaFormat_setBuffer(meta, "raw-codec-specific-data", avpar->extradata, avpar->extradata_size);
    AMediaFormat_setInt32(meta, "wma-version", kTypeWMALossLess);

    return AMEDIA_OK;
}

media_status_t setRAFormat(AVCodecParameters *avpar, AMediaFormat *meta)
{
    ALOGV("COOK");

    AMediaFormat_setString(meta, AMEDIAFORMAT_KEY_MIME, MEDIA_MIMETYPE_AUDIO_RA);
    AMediaFormat_setBuffer(meta, "raw-codec-specific-data", avpar->extradata, avpar->extradata_size);

    return AMEDIA_OK;
}

media_status_t setALACFormat(AVCodecParameters *avpar, AMediaFormat *meta)
{
    ALOGV("ALAC");

    AMediaFormat_setString(meta, AMEDIAFORMAT_KEY_MIME, MEDIA_MIMETYPE_AUDIO_ALAC);
    AMediaFormat_setBuffer(meta, "raw-codec-specific-data", avpar->extradata, avpar->extradata_size);

    return AMEDIA_OK;
}

media_status_t setAPEFormat(AVCodecParameters *avpar, AMediaFormat *meta)
{
    ALOGV("APE");

    AMediaFormat_setString(meta, AMEDIAFORMAT_KEY_MIME, MEDIA_MIMETYPE_AUDIO_APE);
    AMediaFormat_setBuffer(meta, "raw-codec-specific-data", avpar->extradata, avpar->extradata_size);

    return AMEDIA_OK;
}

media_status_t setDTSFormat(AVCodecParameters *avpar, AMediaFormat *meta)
{
    ALOGV("DTS");

    AMediaFormat_setString(meta, AMEDIAFORMAT_KEY_MIME, MEDIA_MIMETYPE_AUDIO_DTS);
    AMediaFormat_setBuffer(meta, "raw-codec-specific-data", avpar->extradata, avpar->extradata_size);

    return AMEDIA_OK;
}

media_status_t setFLACFormat(AVCodecParameters *avpar, AMediaFormat *meta)
{
    ALOGV("FLAC");

    AMediaFormat_setString(meta, AMEDIAFORMAT_KEY_MIME, MEDIA_MIMETYPE_AUDIO_FLAC);
    AMediaFormat_setBuffer(meta, "raw-codec-specific-data", avpar->extradata, avpar->extradata_size);

    if (avpar->extradata_size < 10) {
        ALOGE("Invalid extradata in FLAC file! (size=%d)", avpar->extradata_size);
        return AMEDIA_ERROR_UNKNOWN;
    }

    ABitReader br(avpar->extradata, avpar->extradata_size);
    int32_t minBlockSize = br.getBits(16);
    int32_t maxBlockSize = br.getBits(16);
    int32_t minFrameSize = br.getBits(24);
    int32_t maxFrameSize = br.getBits(24);

    AMediaFormat_setInt32(meta, "min-block-size", minBlockSize);
    AMediaFormat_setInt32(meta, "max-block-size", maxBlockSize);
    AMediaFormat_setInt32(meta, "min-frame-size", minFrameSize);
    AMediaFormat_setInt32(meta, "max-frame-size", maxFrameSize);

    return AMEDIA_OK;
}

//Convert H.264 NAL format to annex b
media_status_t convertNal2AnnexB(uint8_t *dst, size_t dst_size,
        uint8_t *src, size_t src_size, size_t nal_len_size)
{
    size_t i = 0;
    size_t nal_len = 0;
    media_status_t status = AMEDIA_OK;

    CHECK_EQ(dst_size, src_size);
    CHECK(nal_len_size == 3 || nal_len_size == 4);

    while (src_size >= nal_len_size) {
        nal_len = 0;
        for( i = 0; i < nal_len_size; i++ ) {
            nal_len = (nal_len << 8) | src[i];
            dst[i] = 0;
        }
        dst[nal_len_size - 1] = 1;
        if (nal_len > INT_MAX || nal_len > src_size) {
            status = AMEDIA_ERROR_MALFORMED;
            break;
        }
        dst += nal_len_size;
        src += nal_len_size;
        src_size -= nal_len_size;

        memcpy(dst, src, nal_len);

        dst += nal_len;
        src += nal_len;
        src_size -= nal_len;
    }

    return status;
}

int getDivXVersion(AVCodecParameters *avpar)
{
    if (avpar->codec_tag == AV_RL32("DIV3")
            || avpar->codec_tag == AV_RL32("div3")
            || avpar->codec_tag == AV_RL32("DIV4")
            || avpar->codec_tag == AV_RL32("div4")) {
        return kTypeDivXVer_3_11;
    }
    if (avpar->codec_tag == AV_RL32("DIVX")
            || avpar->codec_tag == AV_RL32("divx")) {
        return kTypeDivXVer_4;
    }
    if (avpar->codec_tag == AV_RL32("DX50")
           || avpar->codec_tag == AV_RL32("dx50")) {
        return kTypeDivXVer_5;
    }
    return -1;
}

media_status_t parseMetadataTags(AVFormatContext *ctx, AMediaFormat *meta) {
    if (ctx == NULL) {
        return AMEDIA_ERROR_INVALID_OPERATION;
    }

    AVDictionary *dict = ctx->metadata;
    if (dict == NULL) {
        return AMEDIA_ERROR_INVALID_OPERATION;
    }

    struct MetadataMapping {
        const char *from;
        const char *to;
    };

    // avformat -> android mapping
    static const MetadataMapping kMap[] = {
        { "track", AMEDIAFORMAT_KEY_CDTRACKNUMBER },
        { "disc", AMEDIAFORMAT_KEY_DISCNUMBER },
        { "album", AMEDIAFORMAT_KEY_ALBUM },
        { "artist", AMEDIAFORMAT_KEY_ARTIST },
        { "album_artist", AMEDIAFORMAT_KEY_ALBUMARTIST },
        { "composer", AMEDIAFORMAT_KEY_COMPOSER },
        { "date", AMEDIAFORMAT_KEY_DATE },
        { "genre", AMEDIAFORMAT_KEY_GENRE },
        { "title", AMEDIAFORMAT_KEY_TITLE },
        { "year", AMEDIAFORMAT_KEY_YEAR },
        { "compilation", AMEDIAFORMAT_KEY_COMPILATION },
        { "location", AMEDIAFORMAT_KEY_LOCATION },
    };

    static const size_t kNumEntries = sizeof(kMap) / sizeof(kMap[0]);

    for (size_t i = 0; i < kNumEntries; ++i) {
        AVDictionaryEntry *entry = av_dict_get(dict, kMap[i].from, NULL, 0);
        if (entry != NULL) {
            ALOGV("found key %s with value %s", entry->key, entry->value);
            AMediaFormat_setString(meta, kMap[i].to, entry->value);
        }
    }

    // now look for album art- this will be in a separate stream
    for (size_t i = 0; i < ctx->nb_streams; i++) {
        if (ctx->streams[i]->disposition & AV_DISPOSITION_ATTACHED_PIC) {
            AVPacket& pkt = ctx->streams[i]->attached_pic;
            if (pkt.size > 0) {
                if (ctx->streams[i]->codecpar != NULL) {
                    const char *mime;
                    if (ctx->streams[i]->codecpar->codec_id == AV_CODEC_ID_MJPEG) {
                        mime = MEDIA_MIMETYPE_IMAGE_JPEG;
                    } else if (ctx->streams[i]->codecpar->codec_id == AV_CODEC_ID_PNG) {
                        mime = "image/png";
                    } else {
                        mime = NULL;
                    }
                    if (mime != NULL) {
                        ALOGV("found albumart in stream %zu with type %s len %d", i, mime, pkt.size);
                        AMediaFormat_setBuffer(meta, AMEDIAFORMAT_KEY_ALBUMART, pkt.data, pkt.size);
                    }
                }
            }
        }
    }

    return AMEDIA_OK;
}

AudioEncoding sampleFormatToEncoding(AVSampleFormat fmt) {

    // we resample planar formats to interleaved
    switch (fmt) {
        case AV_SAMPLE_FMT_U8:
        case AV_SAMPLE_FMT_U8P:
            return kAudioEncodingPcm8bit;
        case AV_SAMPLE_FMT_S16:
        case AV_SAMPLE_FMT_S16P:
            return kAudioEncodingPcm16bit;
        case AV_SAMPLE_FMT_FLT:
        case AV_SAMPLE_FMT_FLTP:
            return kAudioEncodingPcmFloat;
        case AV_SAMPLE_FMT_DBL:
        case AV_SAMPLE_FMT_DBLP:
            return kAudioEncodingPcmFloat;
        default:
            return kAudioEncodingInvalid;
    }

}

AVSampleFormat encodingToSampleFormat(AudioEncoding encoding) {
    switch (encoding) {
        case kAudioEncodingPcm8bit:
            return AV_SAMPLE_FMT_U8;
        case kAudioEncodingPcm16bit:
            return AV_SAMPLE_FMT_S16;
        case kAudioEncodingPcmFloat:
            return AV_SAMPLE_FMT_FLT;
        default:
            return AV_SAMPLE_FMT_NONE;
    }
}

}  // namespace android

