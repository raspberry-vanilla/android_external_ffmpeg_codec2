/*
 * Copyright 2012 Michael Chen <omxcodec@gmail.com>
 * Copyright 2015 The CyanogenMod Project
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

#define LOG_TAG "FFmpegExtractor"
#include <utils/Log.h>

#include <stdint.h>
#include <limits.h> /* INT_MAX */
#include <inttypes.h>
#include <sys/prctl.h>

#include <utils/misc.h>
#include <utils/String8.h>
#include <cutils/properties.h>
#include <media/stagefright/DataSourceBase.h>
#include <media/stagefright/foundation/ABitReader.h>
#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/foundation/avc_utils.h>
#include <media/stagefright/foundation/hexdump.h>
#include <media/stagefright/MediaBuffer.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MediaErrors.h>
#include <media/stagefright/Utils.h>

#include "codec_utils.h"
#include "ffmpeg_cmdutils.h"

#include "FFmpegExtractor.h"

#define MAX_QUEUE_SIZE (40 * 1024 * 1024)
#define MIN_AUDIOQ_SIZE (2 * 1024 * 1024)
#define MIN_FRAMES 5
#define EXTRACTOR_MAX_PROBE_PACKETS 200
#define FF_MAX_EXTRADATA_SIZE ((1 << 28) - AV_INPUT_BUFFER_PADDING_SIZE)

#define WAIT_KEY_PACKET_AFTER_SEEK 1
#define SUPPOURT_UNKNOWN_FORMAT    1

//debug
#define DEBUG_READ_ENTRY           0
#define DEBUG_DISABLE_VIDEO        0
#define DEBUG_DISABLE_AUDIO        0
#define DEBUG_PKT                  0
#define DEBUG_EXTRADATA            0
#define DEBUG_FORMATS              0

enum {
    NO_SEEK = 0,
    SEEK,
};

namespace android {

static const char *findMatchingContainer(const char *name);
static CMediaExtractor *CreateFFMPEGExtractor(CDataSource *source, void *meta);

struct FFmpegSource : public MediaTrackHelper {
    FFmpegSource(FFmpegExtractor *extractor, size_t index);

    virtual media_status_t start();
    virtual media_status_t stop();
    virtual media_status_t getFormat(AMediaFormat *meta);

    virtual media_status_t read(
            MediaBufferHelper **buffer, const ReadOptions *options);

protected:
    virtual ~FFmpegSource();

private:
    friend struct FFmpegExtractor;

    FFmpegExtractor *mExtractor;
    size_t mTrackIndex;

    enum AVMediaType mMediaType;

    mutable Mutex mLock;

    bool mIsAVC;
    bool mIsHEVC;
    size_t mNALLengthSize;
    bool mNal2AnnexB;

    AVStream *mStream;
    PacketQueue *mQueue;

    int64_t mFirstKeyPktTimestamp;
    int64_t mLastPTS;
    int64_t mTargetTime;

    DISALLOW_EVIL_CONSTRUCTORS(FFmpegSource);
};

////////////////////////////////////////////////////////////////////////////////

FFmpegExtractor::FFmpegExtractor(DataSourceHelper *source, const sp<AMessage> &meta)
    : mDataSource(source),
      mAudioQ(NULL),
      mVideoQ(NULL),
      mFormatCtx(NULL),
      mParsedMetadata(false) {
    ALOGV("FFmpegExtractor::FFmpegExtractor");

    mMeta = AMediaFormat_new();
    fetchStuffsFromSniffedMeta(meta);

    mVideoQ = packet_queue_alloc();
    mAudioQ = packet_queue_alloc();

    int err = initStreams();
    if (err < 0) {
        ALOGE("failed to init ffmpeg");
        return;
    }

    while (mPktCounter <= EXTRACTOR_MAX_PROBE_PACKETS &&
           (mDefersToCreateVideoTrack || mDefersToCreateAudioTrack)) {
        err = feedNextPacket();
        if (err < 0 && err != AVERROR(EAGAIN)) {
            ALOGE("deferred track creation failed, %s (%08x)", av_err2str(err), err);
            return;
        }
    }

    ALOGV("mPktCounter: %zu, mEOF: %d, pb->error(if has): %d, mDefersToCreateVideoTrack: %d, mDefersToCreateAudioTrack: %d",
          mPktCounter, mEOF, mFormatCtx->pb ? mFormatCtx->pb->error : 0, mDefersToCreateVideoTrack, mDefersToCreateAudioTrack);

    if (mDefersToCreateVideoTrack) {
        ALOGW("deferred creation of video track failed, disabling stream");
        streamComponentClose(mVideoStreamIdx);
    }

    if (mDefersToCreateAudioTrack) {
        ALOGW("deferred creation of audio track failed, disabling stream");
        streamComponentClose(mAudioStreamIdx);
    }
}

FFmpegExtractor::~FFmpegExtractor() {
    ALOGV("FFmpegExtractor::~FFmpegExtractor");

    mAbortRequest = 1;
    deInitStreams();

    Mutex::Autolock autoLock(mLock);

    packet_queue_free(&mVideoQ);
    packet_queue_free(&mAudioQ);

    for (auto& trackInfo : mTracks) {
        AMediaFormat_delete(trackInfo.mMeta);
    }
    AMediaFormat_delete(mMeta);
}

size_t FFmpegExtractor::countTracks() {
    return mTracks.size();
}

MediaTrackHelper* FFmpegExtractor::getTrack(size_t index) {
    ALOGV("FFmpegExtractor::getTrack[%zu]", index);

    if (index >= mTracks.size()) {
        return NULL;
    }

    return new FFmpegSource(this, index);
}

media_status_t FFmpegExtractor::getTrackMetaData(AMediaFormat *meta, size_t index, uint32_t flags __unused) {
    ALOGV("FFmpegExtractor::getTrackMetaData[%zu]", index);

    if (index >= mTracks.size()) {
        return AMEDIA_ERROR_UNKNOWN;
    }

    /* Quick and dirty, just get a frame 1/4 in */
    if (mTracks.itemAt(index).mIndex == mVideoStreamIdx &&
            mFormatCtx->duration != AV_NOPTS_VALUE) {
        AMediaFormat_setInt64(mTracks.editItemAt(index).mMeta,
                AMEDIAFORMAT_KEY_THUMBNAIL_TIME, mFormatCtx->duration / 4);
    }

    AMediaFormat_copy(meta, mTracks.itemAt(index).mMeta);
    return AMEDIA_OK;
}

media_status_t FFmpegExtractor::getMetaData(AMediaFormat *meta) {
    ALOGV("FFmpegExtractor::getMetaData");

    if (!mParsedMetadata) {
        parseMetadataTags(mFormatCtx, mMeta);
        mParsedMetadata = true;
    }

    AMediaFormat_copy(meta, mMeta);
    return AMEDIA_OK;
}

uint32_t FFmpegExtractor::flags() const {
    ALOGV("FFmpegExtractor::flags");

    uint32_t flags = CAN_PAUSE;

    if (mFormatCtx->duration != AV_NOPTS_VALUE) {
        flags |= CAN_SEEK_BACKWARD | CAN_SEEK_FORWARD | CAN_SEEK;
    }

    return flags;
}

int FFmpegExtractor::checkExtradata(AVCodecParameters *avpar)
{
    enum AVCodecID codec_id = AV_CODEC_ID_NONE;
    const char *name = NULL;
    bool *defersToCreateTrack = NULL;
    AVBSFContext **bsfc = NULL;

    // init
    if (avpar->codec_type == AVMEDIA_TYPE_VIDEO) {
        bsfc = &mVideoBsfc;
        defersToCreateTrack = &mDefersToCreateVideoTrack;
    } else if (avpar->codec_type == AVMEDIA_TYPE_AUDIO){
        bsfc = &mAudioBsfc;
        defersToCreateTrack = &mDefersToCreateAudioTrack;
    }

    codec_id = avpar->codec_id;

    // ignore extradata
    if (codec_id != AV_CODEC_ID_H264
            && codec_id != AV_CODEC_ID_MPEG4
            && codec_id != AV_CODEC_ID_MPEG1VIDEO
            && codec_id != AV_CODEC_ID_MPEG2VIDEO
            && codec_id != AV_CODEC_ID_AAC) {
        return 1;
    }

    // is extradata compatible with android?
    if (codec_id != AV_CODEC_ID_AAC) {
        int is_compatible = is_extradata_compatible_with_android(avpar);
        if (!is_compatible) {
            ALOGI("[%s] extradata is not compatible with android, should to extract it from bitstream",
                    av_get_media_type_string(avpar->codec_type));
            *defersToCreateTrack = true;
            *bsfc = NULL; // H264 don't need bsfc, only AAC?
            return 0;
        }
        return 1;
    }

    if (codec_id == AV_CODEC_ID_AAC) {
        name = "aac_adtstoasc";
    }

    if (avpar->extradata_size <= 0) {
        const char* type = av_get_media_type_string(avpar->codec_type);
        ALOGI("[%s] no extradata found, extract it from bitstream", type);
        *defersToCreateTrack = true;
         //CHECK(name != NULL);
        if (!*bsfc && name) {
            const AVBitStreamFilter* bsf = av_bsf_get_by_name(name);
            if (!bsf) {
                ALOGE("[%s] (%s) cannot find bitstream filter", type, name);
                *defersToCreateTrack = false;
                return -1;
            }
            if (av_bsf_alloc(bsf, bsfc) < 0 || !*bsfc) {
                ALOGE("[%s] (%s) cannot allocate bitstream filter", type, name);
                *defersToCreateTrack = false;
                return -1;
            }
            // (*bsfc)->time_base_in = avpar->time_base;
            if (avcodec_parameters_copy((*bsfc)->par_in, avpar)
                    || av_bsf_init(*bsfc)) {
                ALOGE("[%s] (%s) cannot initialize bitstream filter", type, name);
                *defersToCreateTrack = false;
                return -1;
            }
            ALOGV("[%s] (%s) created bitstream filter", type, name);
            return 0;
        } else {
            return 0;
        }
    }
    return 1;
}

static void printTime(int64_t time, const char* type)
{
    int hours, mins, secs, us;

    if (time == AV_NOPTS_VALUE)
        return;

    secs = time / AV_TIME_BASE;
    us = time % AV_TIME_BASE;
    mins = secs / 60;
    secs %= 60;
    hours = mins / 60;
    mins %= 60;
    ALOGI("[%s] the time is %02d:%02d:%02d.%02d",
          type, hours, mins, secs, (100 * us) / AV_TIME_BASE);
}

bool FFmpegExtractor::isCodecSupported(enum AVCodecID codec_id)
{
    switch(codec_id) {
    case AV_CODEC_ID_H264:
    case AV_CODEC_ID_MPEG4:
    case AV_CODEC_ID_H263:
    case AV_CODEC_ID_H263P:
    case AV_CODEC_ID_H263I:
    case AV_CODEC_ID_AAC:
    case AV_CODEC_ID_AC3:
    case AV_CODEC_ID_MP2:
    case AV_CODEC_ID_MP3:
    case AV_CODEC_ID_MPEG1VIDEO:
    case AV_CODEC_ID_MPEG2VIDEO:
    case AV_CODEC_ID_WMV1:
    case AV_CODEC_ID_WMV2:
    case AV_CODEC_ID_WMV3:
    case AV_CODEC_ID_VC1:
    case AV_CODEC_ID_VP8:
    case AV_CODEC_ID_VP9:
    case AV_CODEC_ID_WMAV1:
    case AV_CODEC_ID_WMAV2:
    case AV_CODEC_ID_WMAPRO:
    case AV_CODEC_ID_WMALOSSLESS:
    case AV_CODEC_ID_RV20:
    case AV_CODEC_ID_RV30:
    case AV_CODEC_ID_RV40:
    case AV_CODEC_ID_COOK:
    case AV_CODEC_ID_APE:
    case AV_CODEC_ID_DTS:
    case AV_CODEC_ID_FLAC:
    case AV_CODEC_ID_FLV1:
    case AV_CODEC_ID_VORBIS:
    case AV_CODEC_ID_HEVC:
    case AV_CODEC_ID_ALAC:
        return true;
    default:
        return false;
    }
}

media_status_t FFmpegExtractor::setVideoFormat(AVStream *stream, AMediaFormat *meta)
{
    AVCodecParameters *avpar = NULL;
    media_status_t ret = AMEDIA_ERROR_UNKNOWN;

    avpar = stream->codecpar;
    CHECK_EQ((int)avpar->codec_type, (int)AVMEDIA_TYPE_VIDEO);

    switch(avpar->codec_id) {
    case AV_CODEC_ID_H264:
        if (avpar->extradata[0] == 1) {
            ret = setAVCFormat(avpar, meta);
        } else {
            ret = setH264Format(avpar, meta);
        }
        break;
    case AV_CODEC_ID_MPEG4:
        ret = setMPEG4Format(avpar, meta);
        break;
    case AV_CODEC_ID_H263:
    case AV_CODEC_ID_H263P:
    case AV_CODEC_ID_H263I:
        ret = setH263Format(avpar, meta);
        break;
    case AV_CODEC_ID_MPEG1VIDEO:
    case AV_CODEC_ID_MPEG2VIDEO:
        ret = setMPEG2VIDEOFormat(avpar, meta);
        break;
    case AV_CODEC_ID_VC1:
        ret = setVC1Format(avpar, meta);
        break;
    case AV_CODEC_ID_WMV1:
        ret = setWMV1Format(avpar, meta);
        break;
    case AV_CODEC_ID_WMV2:
        ret = setWMV2Format(avpar, meta);
        break;
    case AV_CODEC_ID_WMV3:
        ret = setWMV3Format(avpar, meta);
        break;
    case AV_CODEC_ID_RV20:
        ret = setRV20Format(avpar, meta);
        break;
    case AV_CODEC_ID_RV30:
        ret = setRV30Format(avpar, meta);
        break;
    case AV_CODEC_ID_RV40:
        ret = setRV40Format(avpar, meta);
        break;
    case AV_CODEC_ID_FLV1:
        ret = setFLV1Format(avpar, meta);
        break;
    case AV_CODEC_ID_HEVC:
        ret = setHEVCFormat(avpar, meta);
        break;
    case AV_CODEC_ID_VP8:
        ret = setVP8Format(avpar, meta);
        break;
    case AV_CODEC_ID_VP9:
        ret = setVP9Format(avpar, meta);
        break;
    default:
        ALOGD("[video] unsupported codec (id: %d, name: %s), but give it a chance",
                avpar->codec_id, avcodec_get_name(avpar->codec_id));
        AMediaFormat_setInt32(meta, "codec-id", avpar->codec_id);
        AMediaFormat_setString(meta, AMEDIAFORMAT_KEY_MIME, MEDIA_MIMETYPE_VIDEO_FFMPEG);
        if (avpar->extradata_size > 0) {
            AMediaFormat_setBuffer(meta, "raw-codec-specific-data", avpar->extradata, avpar->extradata_size);
        }
        //CHECK(!"Should not be here. Unsupported codec.");
        break;
    }

    if (ret == AMEDIA_OK) {
        // rotation
        double theta = get_rotation(stream);
        int rotationDegrees = 0;

        if (fabs(theta - 90) < 1.0) {
            rotationDegrees = 90;
        } else if (fabs(theta - 180) < 1.0) {
            rotationDegrees = 180;
        } else if (fabs(theta - 270) < 1.0) {
            rotationDegrees = 270;
        }
        if (rotationDegrees != 0) {
            AMediaFormat_setInt32(meta, AMEDIAFORMAT_KEY_ROTATION, rotationDegrees);
        }
    }

    if (ret == AMEDIA_OK) {
        float aspect_ratio;
        int width, height;

        if (avpar->sample_aspect_ratio.num == 0)
            aspect_ratio = 0;
        else
            aspect_ratio = av_q2d(avpar->sample_aspect_ratio);

        if (aspect_ratio <= 0.0)
            aspect_ratio = 1.0;
        aspect_ratio *= (float)avpar->width / (float)avpar->height;

        /* XXX: we suppose the screen has a 1.0 pixel ratio */
        height = avpar->height;
        width = ((int)rint(height * aspect_ratio)) & ~1;

        ALOGI("[video] width: %d, height: %d, bit_rate: % " PRId64 " aspect ratio: %f",
                avpar->width, avpar->height, avpar->bit_rate, aspect_ratio);

        AMediaFormat_setInt32(meta, AMEDIAFORMAT_KEY_WIDTH, avpar->width);
        AMediaFormat_setInt32(meta, AMEDIAFORMAT_KEY_HEIGHT, avpar->height);
        if ((width > 0) && (height > 0) &&
            ((avpar->width != width || avpar->height != height))) {
            AMediaFormat_setInt32(meta, AMEDIAFORMAT_KEY_SAR_WIDTH, width);
            AMediaFormat_setInt32(meta, AMEDIAFORMAT_KEY_SAR_HEIGHT, height);
            ALOGI("[video] SAR width: %d, SAR height: %d", width, height);
        }
        if (avpar->bit_rate > 0) {
            AMediaFormat_setInt32(meta, AMEDIAFORMAT_KEY_BIT_RATE, avpar->bit_rate);
        }
        AMediaFormat_setString(meta, "file-format", findMatchingContainer(mFormatCtx->iformat->name));
        setDurationMetaData(stream, meta);
    }

    return AMEDIA_OK;
}

media_status_t FFmpegExtractor::setAudioFormat(AVStream *stream, AMediaFormat *meta)
{
    AVCodecParameters *avpar = NULL;
    media_status_t ret = AMEDIA_ERROR_UNKNOWN;

    avpar = stream->codecpar;
    CHECK_EQ((int)avpar->codec_type, (int)AVMEDIA_TYPE_AUDIO);

    switch(avpar->codec_id) {
    case AV_CODEC_ID_MP2:
        ret = setMP2Format(avpar, meta);
        break;
    case AV_CODEC_ID_MP3:
        ret = setMP3Format(avpar, meta);
        break;
    case AV_CODEC_ID_VORBIS:
        ret = setVORBISFormat(avpar, meta);
        break;
    case AV_CODEC_ID_AC3:
        ret = setAC3Format(avpar, meta);
        break;
    case AV_CODEC_ID_AAC:
        ret = setAACFormat(avpar, meta);
        break;
    case AV_CODEC_ID_WMAV1:
        ret = setWMAV1Format(avpar, meta);
        break;
    case AV_CODEC_ID_WMAV2:
        ret = setWMAV2Format(avpar, meta);
        break;
    case AV_CODEC_ID_WMAPRO:
        ret = setWMAProFormat(avpar, meta);
        break;
    case AV_CODEC_ID_WMALOSSLESS:
        ret = setWMALossLessFormat(avpar, meta);
        break;
    case AV_CODEC_ID_COOK:
        ret = setRAFormat(avpar, meta);
        break;
    case AV_CODEC_ID_APE:
        ret = setAPEFormat(avpar, meta);
        break;
    case AV_CODEC_ID_DTS:
        ret = setDTSFormat(avpar, meta);
        break;
    case AV_CODEC_ID_FLAC:
        ret = setFLACFormat(avpar, meta);
        break;
    case AV_CODEC_ID_ALAC:
        ret = setALACFormat(avpar, meta);
        break;
    default:
        ALOGD("[audio] unsupported codec (id: %d, name: %s), but give it a chance",
                avpar->codec_id, avcodec_get_name(avpar->codec_id));
        AMediaFormat_setInt32(meta, "codec-id", avpar->codec_id);
        AMediaFormat_setInt32(meta, "coded-sample-bits", avpar->bits_per_coded_sample);
        AMediaFormat_setString(meta, AMEDIAFORMAT_KEY_MIME, MEDIA_MIMETYPE_AUDIO_FFMPEG);
        if (avpar->extradata_size > 0) {
            AMediaFormat_setBuffer(meta, "raw-codec-specific-data", avpar->extradata, avpar->extradata_size);
        }
        //CHECK(!"Should not be here. Unsupported codec.");
        break;
    }

    if (ret == AMEDIA_OK) {
        ALOGD("[audio] bit_rate: %" PRId64 ", sample_rate: %d, channels: %d, "
                "bits_per_coded_sample: %d, block_align: %d "
                "bits_per_raw_sample: %d, sample_format: %d",
                avpar->bit_rate, avpar->sample_rate, avpar->ch_layout.nb_channels,
                avpar->bits_per_coded_sample, avpar->block_align,
                avpar->bits_per_raw_sample, avpar->format);

        AMediaFormat_setInt32(meta, AMEDIAFORMAT_KEY_CHANNEL_COUNT, avpar->ch_layout.nb_channels);
        AMediaFormat_setInt32(meta, AMEDIAFORMAT_KEY_BIT_RATE, avpar->bit_rate);
        int32_t bits = avpar->bits_per_raw_sample > 0 ?
                avpar->bits_per_raw_sample :
                av_get_bytes_per_sample((enum AVSampleFormat)avpar->format) * 8;
        AMediaFormat_setInt32(meta, "bits-per-raw-sample", bits);
        AMediaFormat_setInt32(meta, "sample-rate", avpar->sample_rate);
        AMediaFormat_setInt32(meta, "block-align", avpar->block_align);
        AMediaFormat_setInt32(meta, "sample-format", avpar->format);
        //AMediaFormat_setInt32(meta, AMEDIAFORMAT_KEY_PCM_ENCODING, sampleFormatToEncoding(avpar->sample_fmt));
        AMediaFormat_setString(meta, "file-format", findMatchingContainer(mFormatCtx->iformat->name));
        setDurationMetaData(stream, meta);
    }

    return AMEDIA_OK;
}

void FFmpegExtractor::setDurationMetaData(AVStream *stream, AMediaFormat *meta)
{
    AVCodecParameters *avpar = stream->codecpar;

    if (stream->duration != AV_NOPTS_VALUE) {
        int64_t duration = av_rescale_q(stream->duration, stream->time_base, AV_TIME_BASE_Q);
        const char *s = av_get_media_type_string(avpar->codec_type);
        printTime(duration, s);
        if (stream->start_time != AV_NOPTS_VALUE) {
            ALOGV("[%s] startTime: %" PRId64, s, stream->start_time);
        } else {
            ALOGV("[%s] startTime:N/A", s);
        }
        AMediaFormat_setInt64(meta, AMEDIAFORMAT_KEY_DURATION, duration);
    } else {
        // default when no stream duration
        AMediaFormat_setInt64(meta, AMEDIAFORMAT_KEY_DURATION, mFormatCtx->duration);
    }
}

int FFmpegExtractor::streamComponentOpen(int streamIndex)
{
    TrackInfo *trackInfo = NULL;
    AVCodecParameters *avpar = NULL;
    bool supported = false;
    int ret = 0;

    if (streamIndex < 0 || streamIndex >= (int)mFormatCtx->nb_streams) {
        ALOGE("opening stream with invalid stream index(%d)", streamIndex);
        return -1;
    }
    avpar = mFormatCtx->streams[streamIndex]->codecpar;

    const char* type = av_get_media_type_string(avpar->codec_type);
    ALOGI("[%s] opening stream @ index(%d)", type, streamIndex);

    supported = isCodecSupported(avpar->codec_id);
    if (! supported) {
        ALOGD("[%s] unsupported codec (%s), but give it a chance",
              type, avcodec_get_name(avpar->codec_id));
    }

    if ((mFormatCtx->streams[streamIndex]->disposition & AV_DISPOSITION_ATTACHED_PIC) ||
        avpar->codec_tag == MKTAG('j', 'p', 'e', 'g')) {
        ALOGD("[%s] not opening attached picture(%s)", type, avcodec_get_name(avpar->codec_id));
        return -1;
    }
    ALOGI("[%s] support the codec(%s) disposition(%x)",
          type, avcodec_get_name(avpar->codec_id), mFormatCtx->streams[streamIndex]->disposition);

    for (size_t i = 0; i < mTracks.size(); ++i) {
        if (streamIndex == mTracks.editItemAt(i).mIndex) {
            ALOGE("[%s] this track already exists", type);
            return 0;
        }
    }

    mFormatCtx->streams[streamIndex]->discard = AVDISCARD_DEFAULT;

    ALOGV("[%s] tag %s/0x%08x with codec(%s)\n",
          type, av_fourcc2str(avpar->codec_tag), avpar->codec_tag, avcodec_get_name(avpar->codec_id));

    AMediaFormat *meta = AMediaFormat_new();

    switch (avpar->codec_type) {
    case AVMEDIA_TYPE_VIDEO:
        if (mVideoStreamIdx == -1)
            mVideoStreamIdx = streamIndex;
        if (mVideoStream == NULL)
            mVideoStream = mFormatCtx->streams[streamIndex];

        ret = checkExtradata(avpar);
        if (ret != 1) {
            if (ret == -1) {
                // disable the stream
                mVideoStreamIdx = -1;
                mVideoStream = NULL;
                packet_queue_flush(mVideoQ);
                mFormatCtx->streams[streamIndex]->discard = AVDISCARD_ALL;
            }
            return ret;
         }
#if DEBUG_EXTRADATA
        if (avpar->extradata) {
            ALOGV("[%s] stream extradata(%d):", type, avpar->extradata_size);
            hexdump(avpar->extradata, avpar->extradata_size);
        } else {
            ALOGV("[%s] stream has no extradata, but we can ignore it.", type);
        }
#endif
        if (setVideoFormat(mVideoStream, meta) != AMEDIA_OK) {
            ALOGE("[%s] setVideoFormat failed", type);
            return -1;
        }

        ALOGV("[%s] creating track", type);
        mTracks.push();
        trackInfo = &mTracks.editItemAt(mTracks.size() - 1);
        trackInfo->mIndex  = streamIndex;
        trackInfo->mMeta   = meta;
        trackInfo->mStream = mVideoStream;
        trackInfo->mQueue  = mVideoQ;
        trackInfo->mSeek   = false;

        mDefersToCreateVideoTrack = false;

        break;
    case AVMEDIA_TYPE_AUDIO:
        if (mAudioStreamIdx == -1)
            mAudioStreamIdx = streamIndex;
        if (mAudioStream == NULL)
            mAudioStream = mFormatCtx->streams[streamIndex];

        ret = checkExtradata(avpar);
        if (ret != 1) {
            if (ret == -1) {
                // disable the stream
                mAudioStreamIdx = -1;
                mAudioStream = NULL;
                packet_queue_flush(mAudioQ);
                mFormatCtx->streams[streamIndex]->discard = AVDISCARD_ALL;
            }
            return ret;
        }
#if DEBUG_EXTRADATA
        if (avpar->extradata) {
            ALOGV("[%s] stream extradata(%d):", type, avpar->extradata_size);
            hexdump(avpar->extradata, avpar->extradata_size);
        } else {
            ALOGV("[%s] stream has no extradata, but we can ignore it.", type);
        }
#endif
        if (setAudioFormat(mAudioStream, meta) != AMEDIA_OK) {
            ALOGE("[%s] setAudioFormat failed", type);
            return -1;
        }

        ALOGV("[%s] creating track", type);
        mTracks.push();
        trackInfo = &mTracks.editItemAt(mTracks.size() - 1);
        trackInfo->mIndex  = streamIndex;
        trackInfo->mMeta   = meta;
        trackInfo->mStream = mAudioStream;
        trackInfo->mQueue  = mAudioQ;
        trackInfo->mSeek   = false;

        mDefersToCreateAudioTrack = false;

        break;
    case AVMEDIA_TYPE_SUBTITLE:
        /* Unsupport now */
        CHECK(!"Should not be here. Unsupported media type.");
        break;
    default:
        CHECK(!"Should not be here. Unsupported media type.");
        break;
    }
    return 0;
}

void FFmpegExtractor::streamComponentClose(int streamIndex)
{
    AVCodecParameters *avpar;

    if (streamIndex < 0 || streamIndex >= (int)mFormatCtx->nb_streams) {
        ALOGE("closing stream with invalid index(%d)", streamIndex);
        return;
    }
    avpar = mFormatCtx->streams[streamIndex]->codecpar;

    const char* type = av_get_media_type_string(avpar->codec_type);
    ALOGI("[%s] closing stream @ index(%d)", type, streamIndex);

    switch (avpar->codec_type) {
    case AVMEDIA_TYPE_VIDEO:
        ALOGV("[%s] packet_queue_abort", type);
        packet_queue_abort(mVideoQ);
        ALOGV("[%s] packet_queue_end", type);
        packet_queue_flush(mVideoQ);
        break;
    case AVMEDIA_TYPE_AUDIO:
        ALOGV("[%s] packet_queue_abort", type);
        packet_queue_abort(mAudioQ);
        ALOGV("[%s] packet_queue_end", type);
        packet_queue_flush(mAudioQ);
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        break;
    default:
        break;
    }

    mFormatCtx->streams[streamIndex]->discard = AVDISCARD_ALL;
    switch (avpar->codec_type) {
    case AVMEDIA_TYPE_VIDEO:
        mVideoStream    = NULL;
        mVideoStreamIdx = -1;
        if (mVideoBsfc) {
            av_bsf_free(&mVideoBsfc);
        }
        mDefersToCreateVideoTrack = false;
        break;
    case AVMEDIA_TYPE_AUDIO:
        mAudioStream    = NULL;
        mAudioStreamIdx = -1;
        if (mAudioBsfc) {
            av_bsf_free(&mAudioBsfc);
        }
        mDefersToCreateAudioTrack = false;
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        break;
    default:
        break;
    }
}

/* seek in the stream */
int FFmpegExtractor::streamSeek(int trackIndex, int64_t pos,
        MediaTrackHelper::ReadOptions::SeekMode mode)
{
    Mutex::Autolock _l(mLock);

    const TrackInfo& track = mTracks.itemAt(trackIndex);
    const char* type = av_get_media_type_string(track.mStream->codecpar->codec_type);

    if (track.mSeek) {
        // Don't do anything if seeking is already in progress
        ALOGV("[%s] seek already in progress",
              av_get_media_type_string(track.mStream->codecpar->codec_type));
        return NO_SEEK;
    }

    int64_t seekPos = pos, seekMin, seekMax;
    int err;

    switch (mode) {
        case MediaTrackHelper::ReadOptions::SEEK_PREVIOUS_SYNC:
            seekMin = 0;
            seekMax = seekPos;
            break;
        case MediaTrackHelper::ReadOptions::SEEK_NEXT_SYNC:
            seekMin = seekPos;
            seekMax = INT64_MAX;
            break;
        case MediaTrackHelper::ReadOptions::SEEK_CLOSEST_SYNC:
            seekMin = 0;
            seekMax = INT64_MAX;
            break;
        case MediaTrackHelper::ReadOptions::SEEK_CLOSEST:
            seekMin = 0;
            seekMax = seekPos;
            break;
        default:
            TRESPASS();
    }

    err = avformat_seek_file(mFormatCtx, -1, seekMin, seekPos, seekMax, 0);
    if (err < 0) {
        ALOGE("[%s] seek failed(%s (%08x)), restarting at the beginning",
              type, av_err2str(err), err);
        err = avformat_seek_file(mFormatCtx, -1, 0, 0, 0, 0);
        if (err < 0) {
            ALOGE("[%s] seek failed(%s (%08x))", type, av_err2str(err), err);
            return NO_SEEK;
        }
    }

    ALOGV("[%s] (seek) pos=%" PRId64 ", min=%" PRId64 ", max=%" PRId64,
          type, seekPos, seekMin, seekMax);

    mEOF = false;
    for (int i = 0; i < mTracks.size(); i++) {
        TrackInfo& ti = mTracks.editItemAt(i);
        packet_queue_flush(ti.mQueue);
        ti.mSeek = true;
    }

    return SEEK;
}

int FFmpegExtractor::decodeInterruptCb(void *ctx)
{
    FFmpegExtractor *extractor = static_cast<FFmpegExtractor *>(ctx);
    return extractor->mAbortRequest;
}

void FFmpegExtractor::fetchStuffsFromSniffedMeta(const sp<AMessage> &meta)
{
    AString url;
    AString mime;

    //url
    CHECK(meta->findString("extended-extractor-url", &url));
    CHECK(url.c_str() != NULL);
    CHECK(url.size() < PATH_MAX);

    memcpy(mFilename, url.c_str(), url.size());
    mFilename[url.size()] = '\0';

    //mime
    CHECK(meta->findString("extended-extractor-mime", &mime));
    CHECK(mime.c_str() != NULL);
    AMediaFormat_setString(mMeta, AMEDIAFORMAT_KEY_MIME, mime.c_str());
}

void FFmpegExtractor::setFFmpegDefaultOpts()
{
    mGenPTS       = 0;
#if DEBUG_DISABLE_VIDEO
    mVideoDisable = 1;
#else
    mVideoDisable = 0;
#endif
#if DEBUG_DISABLE_AUDIO
    mAudioDisable = 1;
#else
    mAudioDisable = 0;
#endif
    mShowStatus   = 0;
    mSeekByBytes  = 0; /* seek by bytes 0=off 1=on -1=auto" */
    mDuration     = AV_NOPTS_VALUE;

    mVideoStreamIdx = -1;
    mAudioStreamIdx = -1;
    mVideoStream  = NULL;
    mAudioStream  = NULL;
    mDefersToCreateVideoTrack = false;
    mDefersToCreateAudioTrack = false;
    mVideoBsfc = NULL;
    mAudioBsfc = NULL;

    mAbortRequest = 0;
    mPktCounter   = 0;
    mEOF          = false;
}

int FFmpegExtractor::initStreams()
{
    int err = 0;
    int i = 0;
    int ret = 0, audio_ret = -1, video_ret = -1;
    AVDictionaryEntry *t = NULL;
    AVDictionary **opts = NULL;
    int orig_nb_streams = 0;
    int st_index[AVMEDIA_TYPE_NB] = {0};
    int wanted_stream[AVMEDIA_TYPE_NB] = {0};
    st_index[AVMEDIA_TYPE_AUDIO]  = -1;
    st_index[AVMEDIA_TYPE_VIDEO]  = -1;
    wanted_stream[AVMEDIA_TYPE_AUDIO]  = -1;
    wanted_stream[AVMEDIA_TYPE_VIDEO]  = -1;
    AVDictionary *format_opts = NULL, *codec_opts = NULL;

    setFFmpegDefaultOpts();

    mFormatCtx = avformat_alloc_context();
    if (!mFormatCtx)
    {
        ALOGE("oom for alloc avformat context");
        ret = -1;
        goto fail;
    }
    mFormatCtx->interrupt_callback.callback = decodeInterruptCb;
    mFormatCtx->interrupt_callback.opaque = this;
    ALOGV("mFilename: %s", mFilename);
    err = avformat_open_input(&mFormatCtx, mFilename, NULL, &format_opts);
    if (err < 0) {
        ALOGE("avformat_open_input(%s) failed: %s (%08x)", mFilename, av_err2str(err), err);
        ret = -1;
        goto fail;
    }

    if ((t = av_dict_get(format_opts, "", NULL, AV_DICT_IGNORE_SUFFIX))) {
        ALOGE("Option %s not found.\n", t->key);
        //ret = AVERROR_OPTION_NOT_FOUND;
        ret = -1;
        av_dict_free(&format_opts);
        goto fail;
    }

    av_dict_free(&format_opts);

    if (mGenPTS)
        mFormatCtx->flags |= AVFMT_FLAG_GENPTS;

    opts = setup_find_stream_info_opts(mFormatCtx, codec_opts);
    orig_nb_streams = mFormatCtx->nb_streams;

    err = avformat_find_stream_info(mFormatCtx, opts);
    if (err < 0) {
        ALOGE("avformat_find_stream_info(%s) failed: %s (%08x)", mFilename, av_err2str(err), err);
        ret = -1;
        goto fail;
    }
    for (i = 0; i < orig_nb_streams; i++)
        av_dict_free(&opts[i]);
    av_freep(&opts);

    if (mFormatCtx->pb)
        mFormatCtx->pb->eof_reached = 0; // FIXME hack, ffplay maybe should not use url_feof() to test for the end

    if (mSeekByBytes < 0)
        mSeekByBytes = !!(mFormatCtx->iformat->flags & AVFMT_TS_DISCONT)
            && strcmp("ogg", mFormatCtx->iformat->name);

    for (i = 0; i < (int)mFormatCtx->nb_streams; i++)
        mFormatCtx->streams[i]->discard = AVDISCARD_ALL;
    if (!mVideoDisable)
        st_index[AVMEDIA_TYPE_VIDEO] =
            av_find_best_stream(mFormatCtx, AVMEDIA_TYPE_VIDEO,
                                wanted_stream[AVMEDIA_TYPE_VIDEO], -1, NULL, 0);
    if (!mAudioDisable)
        st_index[AVMEDIA_TYPE_AUDIO] =
            av_find_best_stream(mFormatCtx, AVMEDIA_TYPE_AUDIO,
                                wanted_stream[AVMEDIA_TYPE_AUDIO],
                                st_index[AVMEDIA_TYPE_VIDEO],
                                NULL, 0);
    if (mShowStatus) {
        av_dump_format(mFormatCtx, 0, mFilename, 0);
    }

    if (mFormatCtx->duration != AV_NOPTS_VALUE &&
            mFormatCtx->start_time != AV_NOPTS_VALUE) {
        int hours, mins, secs, us;

        ALOGV("file startTime: %" PRId64, mFormatCtx->start_time);

        mDuration = mFormatCtx->duration;

        secs = mDuration / AV_TIME_BASE;
        us = mDuration % AV_TIME_BASE;
        mins = secs / 60;
        secs %= 60;
        hours = mins / 60;
        mins %= 60;
        ALOGI("the duration is %02d:%02d:%02d.%02d",
            hours, mins, secs, (100 * us) / AV_TIME_BASE);
    }

    if (st_index[AVMEDIA_TYPE_AUDIO] >= 0) {
        audio_ret = streamComponentOpen(st_index[AVMEDIA_TYPE_AUDIO]);
        if (audio_ret >= 0)
            packet_queue_start(mAudioQ);
    }

    if (st_index[AVMEDIA_TYPE_VIDEO] >= 0) {
        video_ret = streamComponentOpen(st_index[AVMEDIA_TYPE_VIDEO]);
        if (video_ret >= 0)
            packet_queue_start(mVideoQ);
    }

    if (audio_ret < 0 && video_ret < 0) {
        ALOGE("initStreams(%s) could not find any audio/video", mFilename);
        ret = -1;
        goto fail;
    }

    ret = 0;

fail:
    return ret;
}

void FFmpegExtractor::deInitStreams()
{
    if (mAudioStreamIdx >= 0)
        streamComponentClose(mAudioStreamIdx);
    if (mVideoStreamIdx >= 0)
        streamComponentClose(mVideoStreamIdx);

    if (mFormatCtx) {
        avformat_close_input(&mFormatCtx);
    }
}

int FFmpegExtractor::feedNextPacket() {
    AVPacket pkt1, *pkt = &pkt1;
    int ret;

    // Shortcut if EOF already reached

    if (mEOF) {
        return AVERROR_EOF;
    }

    // Read next frame

    ret = av_read_frame(mFormatCtx, pkt);
    if (ret < 0) {
        if (ret == AVERROR_EOF) {
            ALOGV("file reached EOF");
        } else {
            ALOGE("failed to read next frame: %s (%08x)", av_err2str(ret), ret);
        }
        mEOF = true;
        return AVERROR_EOF;
    }
    mPktCounter++;

#if DEBUG_PKT
    ALOGV("next packet [%d] pts=%" PRId64 ", dts=%" PRId64 ", size=%d",
          pkt->stream_index, pkt->pts, pkt->dts, pkt->size);
#endif

    // Handle bitstream filter and deferred track creation

    if (pkt->stream_index == mVideoStreamIdx) {
         if (mDefersToCreateVideoTrack) {
            AVCodecParameters *avpar = mFormatCtx->streams[mVideoStreamIdx]->codecpar;
            int i = parser_split(avpar, pkt->data, pkt->size);

            if (i > 0 && i < FF_MAX_EXTRADATA_SIZE) {
                if (avpar->extradata) {
                    av_freep(&avpar->extradata);
                }
                avpar->extradata_size = i;
                avpar->extradata = (uint8_t *)av_malloc(avpar->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
                if (avpar->extradata) {
                    // sps + pps(there may be sei in it)
                    memcpy(avpar->extradata, pkt->data, avpar->extradata_size);
                    memset(avpar->extradata + i, 0, AV_INPUT_BUFFER_PADDING_SIZE);
                } else {
                    ALOGE("[video] failed to allocate new extradata");
                    return AVERROR(ENOMEM);
                }
            } else {
                av_packet_unref(pkt);
                return AVERROR(EAGAIN);
            }

            streamComponentOpen(mVideoStreamIdx);
            if (!mDefersToCreateVideoTrack) {
                ALOGI("[video] probe packet counter: %zu when track created", mPktCounter);
            }
        }
    } else if (pkt->stream_index == mAudioStreamIdx) {
        AVCodecParameters *avpar = mFormatCtx->streams[mAudioStreamIdx]->codecpar;

        if (mAudioBsfc && pkt->data) {
            ret = av_bsf_send_packet(mAudioBsfc, pkt);
            if (ret < 0) {
                ALOGE("[audio::%s] failed to send packet to filter, err = %d", mAudioBsfc->filter->name, ret);
                av_packet_unref(pkt);
                return ret;
            }
            ret = av_bsf_receive_packet(mAudioBsfc, pkt);
            if (ret < 0) {
                ALOGE_IF(ret != AVERROR(EAGAIN), "[audio::%s] failed to received packet from filter, err=%d",
                         mAudioBsfc->filter->name, ret);
                av_packet_unref(pkt);
                return ret;
            }
            if (mDefersToCreateAudioTrack && avpar->extradata_size <= 0) {
                size_t new_extradata_size = 0;
                uint8_t* new_extradata = av_packet_get_side_data(pkt, AV_PKT_DATA_NEW_EXTRADATA, &new_extradata_size);

                if (new_extradata_size > 0) {
                    ALOGV("[audio::%s] extradata found, len=%zd", mAudioBsfc->filter->name, new_extradata_size);
                    avpar->extradata = (uint8_t*)av_mallocz(new_extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
                    if (avpar->extradata) {
                        memcpy(avpar->extradata, new_extradata, new_extradata_size);
                        avpar->extradata_size = new_extradata_size;
                    } else {
                        ALOGE("[audio::%s] failed to allocate new extradata", mAudioBsfc->filter->name);
                        return AVERROR(ENOMEM);
                    }
                }
            }
        }

        if (mDefersToCreateAudioTrack) {
            if (avpar->extradata_size <= 0) {
                av_packet_unref(pkt);
                return AVERROR(EAGAIN);
            }
            streamComponentOpen(mAudioStreamIdx);
            if (!mDefersToCreateAudioTrack) {
                ALOGI("[audio] probe packet counter: %zu when track created", mPktCounter);
            }
        }
    }

    // Queue frame

    if (pkt->stream_index == mVideoStreamIdx) {
        packet_queue_put(mVideoQ, pkt);
        return mVideoStreamIdx;
    } else if (pkt->stream_index == mAudioStreamIdx) {
        packet_queue_put(mAudioQ, pkt);
        return mAudioStreamIdx;
    } else {
        av_packet_unref(pkt);
        return AVERROR(EAGAIN);
    }
}

int FFmpegExtractor::getPacket(int trackIndex, AVPacket *pkt) {
    TrackInfo& track = mTracks.editItemAt(trackIndex);
    const char* type = av_get_media_type_string(track.mStream->codecpar->codec_type);
    int err;

    while (true) {
        Mutex::Autolock _l(mLock);

        err = packet_queue_get(track.mQueue, pkt, 0);
        if (err > 0) {
            if (track.mSeek && (pkt->flags & AV_PKT_FLAG_KEY) != 0) {
                ALOGV("[%s] (seek) key frame found @ ts=%" PRId64,
                      type, pkt->pts != AV_NOPTS_VALUE ? av_rescale_q(pkt->pts, track.mStream->time_base, AV_TIME_BASE_Q) : -1);
                track.mSeek = false;
            }
            if (! track.mSeek) {
                return 0;
            } else {
                ALOGV("[%s] (seek) drop non key frame", type);
            }
        } else if (err < 0) {
            return AVERROR_UNKNOWN;
        } else if (err == 0) {
            err = feedNextPacket();
            if (err < 0 && err != AVERROR(EAGAIN)) {
                return err;
            }
        }
    }
}

////////////////////////////////////////////////////////////////////////////////

FFmpegSource::FFmpegSource(
        FFmpegExtractor *extractor, size_t index)
    : mExtractor(extractor),
      mTrackIndex(index),
      mIsAVC(false),
      mIsHEVC(false),
      mNal2AnnexB(false),
      mStream(mExtractor->mTracks.itemAt(index).mStream),
      mLastPTS(AV_NOPTS_VALUE),
      mTargetTime(AV_NOPTS_VALUE) {
    AMediaFormat *meta = mExtractor->mTracks.itemAt(index).mMeta;
    AVCodecParameters *avpar = mStream->codecpar;

    mMediaType = mStream->codecpar->codec_type;
    mFirstKeyPktTimestamp = AV_NOPTS_VALUE;

    ALOGV("[%s] FFmpegSource::FFmpegSource", av_get_media_type_string(mMediaType));

    /* Parse codec specific data */
    if (avpar->codec_id == AV_CODEC_ID_H264
            && avpar->extradata_size > 0
            && avpar->extradata[0] == 1) {
        mIsAVC = true;

        void *data;
        size_t size;
        CHECK(AMediaFormat_getBuffer(meta, AMEDIAFORMAT_KEY_CSD_AVC, &data, &size));

        const uint8_t *ptr = (const uint8_t *)data;

        CHECK(size >= 7);
        CHECK_EQ((unsigned)ptr[0], 1u);  // configurationVersion == 1

        // The number of bytes used to encode the length of a NAL unit.
        mNALLengthSize = 1 + (ptr[4] & 3);

        ALOGV("[video] the stream is AVC, the length of a NAL unit: %zu", mNALLengthSize);

        mNal2AnnexB = true;
    } else if (avpar->codec_id == AV_CODEC_ID_HEVC
            && avpar->extradata_size > 3
            && (avpar->extradata[0] || avpar->extradata[1] ||
                avpar->extradata[2] > 1)) {
        /* It seems the extradata is encoded as hvcC format.
         * Temporarily, we support configurationVersion==0 until 14496-15 3rd
         * is finalized. When finalized, configurationVersion will be 1 and we
         * can recognize hvcC by checking if avpar->extradata[0]==1 or not. */
        mIsHEVC = true;

        void *data;
        size_t size;
        CHECK(AMediaFormat_getBuffer(meta, AMEDIAFORMAT_KEY_CSD_HEVC, &data, &size));

        const uint8_t *ptr = (const uint8_t *)data;

        CHECK(size >= 7);
        //CHECK_EQ((unsigned)ptr[0], 1u);  // configurationVersion == 1

        // The number of bytes used to encode the length of a NAL unit.
        mNALLengthSize = 1 + (ptr[21] & 3);

        ALOGD("[video] the stream is HEVC, the length of a NAL unit: %zu", mNALLengthSize);

        mNal2AnnexB = true;
    }
}

FFmpegSource::~FFmpegSource() {
    ALOGV("[%s] FFmpegSource::~FFmpegSource",
            av_get_media_type_string(mMediaType));
    mExtractor = NULL;
}

media_status_t FFmpegSource::start() {
    ALOGV("[%s] FFmpegSource::start",
          av_get_media_type_string(mMediaType));
    mBufferGroup->init(1, 1024, 64);
    return AMEDIA_OK;
}

media_status_t FFmpegSource::stop() {
    ALOGV("[%s] FFmpegSource::stop",
          av_get_media_type_string(mMediaType));
    return AMEDIA_OK;
}

media_status_t FFmpegSource::getFormat(AMediaFormat *meta) {
    AMediaFormat_copy(meta, mExtractor->mTracks.itemAt(mTrackIndex).mMeta);
    return AMEDIA_OK;
}

media_status_t FFmpegSource::read(
        MediaBufferHelper **buffer, const ReadOptions *options) {
    *buffer = NULL;

    AVPacket pkt;
    ReadOptions::SeekMode mode;
    int64_t pktTS = AV_NOPTS_VALUE;
    int64_t seekTimeUs = AV_NOPTS_VALUE;
    int64_t timeUs = AV_NOPTS_VALUE;
    int key = 0;
    media_status_t status = AMEDIA_OK;
    int max_negative_time_frame = 100;
    int err;

    // FIXME: should we really use mStream->start_time?
    // int64_t startTimeUs = mStream->start_time == AV_NOPTS_VALUE ? 0 :
    //     av_rescale_q(mStream->start_time, mStream->time_base, AV_TIME_BASE_Q);
    int64_t startTimeUs = 0;

    if (options && options->getSeekTo(&seekTimeUs, &mode)) {
        int64_t seekPTS = seekTimeUs;
        ALOGV("[%s] (seek) seekTimeUs: %" PRId64 ", seekPTS: %" PRId64 ", mode: %d",
              av_get_media_type_string(mMediaType), seekTimeUs, seekPTS, mode);
        /* add the stream start time */
        if (mStream->start_time != AV_NOPTS_VALUE) {
            seekPTS += startTimeUs;
        }
        ALOGV("[%s] (seek) seekTimeUs[+startTime]: %" PRId64 ", mode: %d start_time=%" PRId64,
              av_get_media_type_string(mMediaType), seekPTS, mode, startTimeUs);
        mExtractor->streamSeek(mTrackIndex, seekPTS, mode);
    }

retry:
    err = mExtractor->getPacket(mTrackIndex, &pkt);
    if (err < 0) {
        if (err == AVERROR_EOF) {
            ALOGV("[%s] read EOS", av_get_media_type_string(mMediaType));
        } else {
            ALOGE("[%s] read error: %s (%08x)", av_get_media_type_string(mMediaType), av_err2str(err), err);
        }
        return AMEDIA_ERROR_END_OF_STREAM;
    }

    key = pkt.flags & AV_PKT_FLAG_KEY ? 1 : 0;
    pktTS = pkt.pts == AV_NOPTS_VALUE ? pkt.dts : pkt.pts;

    if (pktTS != AV_NOPTS_VALUE && mFirstKeyPktTimestamp == AV_NOPTS_VALUE) {
        // update the first key timestamp
        mFirstKeyPktTimestamp = pktTS;
    }

    MediaBufferHelper *mediaBuffer;
    mBufferGroup->acquire_buffer(&mediaBuffer, false, pkt.size + AV_INPUT_BUFFER_PADDING_SIZE);
    AMediaFormat_clear(mediaBuffer->meta_data());
    mediaBuffer->set_range(0, pkt.size);

    //copy data
    if ((mIsAVC || mIsHEVC) && mNal2AnnexB) {
        /* This only works for NAL sizes 3-4 */
        if ((mNALLengthSize != 3) && (mNALLengthSize != 4)) {
            ALOGE("[%s] cannot use convertNal2AnnexB, nal size: %zu",
                  av_get_media_type_string(mMediaType), mNALLengthSize);
            mediaBuffer->release();
            mediaBuffer = NULL;
            av_packet_unref(&pkt);
            return AMEDIA_ERROR_MALFORMED;
        }

        uint8_t *dst = (uint8_t *)mediaBuffer->data();
        /* Convert H.264 NAL format to annex b */
        status = convertNal2AnnexB(dst, pkt.size, pkt.data, pkt.size, mNALLengthSize);
        if (status != AMEDIA_OK) {
            ALOGE("[%s] convertNal2AnnexB failed",
                  av_get_media_type_string(mMediaType));
            mediaBuffer->release();
            mediaBuffer = NULL;
            av_packet_unref(&pkt);
            return AMEDIA_ERROR_MALFORMED;
        }
    } else {
        memcpy(mediaBuffer->data(), pkt.data, pkt.size);
    }

    if (pktTS != AV_NOPTS_VALUE)
        timeUs = av_rescale_q(pktTS, mStream->time_base, AV_TIME_BASE_Q) - startTimeUs;
    else
        timeUs = SF_NOPTS_VALUE; //FIXME AV_NOPTS_VALUE is negative, but stagefright need positive

    // Negative timestamp will cause crash for media_server
    // in OMXCodec.cpp CHECK(lastBufferTimeUs >= 0).
    // And we should not get negative timestamp
    if (timeUs < 0) {
        ALOGE("[%s] negative timestamp encounter: time: %" PRId64
               " startTimeUs: %" PRId64
               " packet dts: %" PRId64
               " packet pts: %" PRId64
               , av_get_media_type_string(mMediaType), timeUs, startTimeUs, pkt.dts, pkt.pts);
        mediaBuffer->release();
        mediaBuffer = NULL;
        av_packet_unref(&pkt);
        if (max_negative_time_frame-- > 0) {
            goto retry;
        } else {
            ALOGE("[%s] too many negative timestamp packets, abort decoding",
                  av_get_media_type_string(mMediaType));
            return AMEDIA_ERROR_MALFORMED;
        }
    }

    // FIXME: figure out what this is supposed to do...
    // // predict the next PTS to use for exact-frame seek below
    // int64_t nextPTS = AV_NOPTS_VALUE;
    // if (mLastPTS != AV_NOPTS_VALUE && timeUs > mLastPTS) {
    //     nextPTS = timeUs + (timeUs - mLastPTS);
    //     mLastPTS = timeUs;
    // } else if (mLastPTS == AV_NOPTS_VALUE) {
    //     mLastPTS = timeUs;
    // }

#if DEBUG_PKT
    if (pktTS != AV_NOPTS_VALUE)
        ALOGV("[%s] read pkt, size:%d, key:%d, pktPTS: %lld, pts:%lld, dts:%lld, timeUs[-startTime]:%lld us (%.2f secs) start_time=%lld",
            av_get_media_type_string(mMediaType), pkt.size, key, pktTS, pkt.pts, pkt.dts, timeUs, timeUs/1E6, startTimeUs);
    else
        ALOGV("[%s] read pkt, size:%d, key:%d, pts:N/A, dts:N/A, timeUs[-startTime]:N/A",
            av_get_media_type_string(mMediaType), pkt.size, key);
#endif

    AMediaFormat_setInt64(mediaBuffer->meta_data(), AMEDIAFORMAT_KEY_TIME_US, timeUs);
    AMediaFormat_setInt32(mediaBuffer->meta_data(), AMEDIAFORMAT_KEY_IS_SYNC_FRAME, key);

    // FIXME: also figure out what this is supposed to do...
    // // deal with seek-to-exact-frame, we might be off a bit and Stagefright will assert on us
    // if (seekTimeUs != AV_NOPTS_VALUE && timeUs < seekTimeUs &&
    //         mode == MediaSource::ReadOptions::SEEK_CLOSEST) {
    //     mTargetTime = seekTimeUs;
    //     AMediaFormat_setInt64(mediaBuffer->meta_data(), AMEDIAFORMAT_KEY_TARGET_TIME, seekTimeUs);
    // }

    // if (mTargetTime != AV_NOPTS_VALUE) {
    //     if (timeUs == mTargetTime) {
    //         mTargetTime = AV_NOPTS_VALUE;
    //     } else if (nextPTS != AV_NOPTS_VALUE && nextPTS > mTargetTime) {
    //         ALOGV("[%s] adjust target frame time to %" PRId64,
    //               av_get_media_type_string(mMediaType), timeUs);
    //         AMediaFormat_setInt64(mediaBuffer->meta_data(), AMEDIAFORMAT_KEY_TIME_US, mTargetTime);
    //         mTargetTime = AV_NOPTS_VALUE;
    //     }
    // }

    *buffer = mediaBuffer;

    av_packet_unref(&pkt);

    return AMEDIA_OK;
}

////////////////////////////////////////////////////////////////////////////////

typedef struct {
    const char *format;
    const char *container;
} formatmap;

static formatmap FILE_FORMATS[] = {
        {"mpeg",                    MEDIA_MIMETYPE_CONTAINER_MPEG2PS  },
        {"mpegts",                  MEDIA_MIMETYPE_CONTAINER_TS       },
        {"mov,mp4,m4a,3gp,3g2,mj2", MEDIA_MIMETYPE_CONTAINER_MPEG4    },
        {"matroska,webm",           MEDIA_MIMETYPE_CONTAINER_MATROSKA },
        {"asf",                     MEDIA_MIMETYPE_CONTAINER_ASF      },
        {"rm",                      MEDIA_MIMETYPE_CONTAINER_RM       },
        {"flv",                     MEDIA_MIMETYPE_CONTAINER_FLV      },
        {"swf",                     MEDIA_MIMETYPE_CONTAINER_FLV      },
        {"avi",                     MEDIA_MIMETYPE_CONTAINER_AVI      },
        {"ape",                     MEDIA_MIMETYPE_CONTAINER_APE      },
        {"dts",                     MEDIA_MIMETYPE_CONTAINER_DTS      },
        {"flac",                    MEDIA_MIMETYPE_CONTAINER_FLAC     },
        {"ac3",                     MEDIA_MIMETYPE_AUDIO_AC3          },
        {"mp3",                     MEDIA_MIMETYPE_AUDIO_MPEG         },
        {"wav",                     MEDIA_MIMETYPE_CONTAINER_WAV      },
        {"ogg",                     MEDIA_MIMETYPE_CONTAINER_OGG      },
        {"vc1",                     MEDIA_MIMETYPE_CONTAINER_VC1      },
        {"hevc",                    MEDIA_MIMETYPE_CONTAINER_HEVC     },
        {"divx",                    MEDIA_MIMETYPE_CONTAINER_DIVX     },
};

static AVCodecParameters* getCodecParameters(AVFormatContext *ic, AVMediaType codec_type)
{
    unsigned int idx = 0;
    AVCodecParameters *avpar = NULL;

    for (idx = 0; idx < ic->nb_streams; idx++) {
        if (ic->streams[idx]->disposition & AV_DISPOSITION_ATTACHED_PIC) {
            // FFMPEG converts album art to MJPEG, but we don't want to
            // include that in the parsing as MJPEG is not supported by
            // Android, which forces the media to be extracted by FFMPEG
            // while in fact, Android supports it.
            continue;
        }

        avpar = ic->streams[idx]->codecpar;
        if (avpar->codec_tag == MKTAG('j', 'p', 'e', 'g')) {
            // Sometimes the disposition isn't set
            continue;
        }
        if (avpar->codec_type == codec_type) {
            return avpar;
        }
    }

    return NULL;
}

static enum AVCodecID getCodecId(AVFormatContext *ic, AVMediaType codec_type)
{
    AVCodecParameters *avpar = getCodecParameters(ic, codec_type);
    return avpar == NULL ? AV_CODEC_ID_NONE : avpar->codec_id;
}

static bool hasAudioCodecOnly(AVFormatContext *ic)
{
    bool haveVideo = false;
    bool haveAudio = false;

    if (getCodecId(ic, AVMEDIA_TYPE_VIDEO) != AV_CODEC_ID_NONE) {
        haveVideo = true;
    }
    if (getCodecId(ic, AVMEDIA_TYPE_AUDIO) != AV_CODEC_ID_NONE) {
        haveAudio = true;
    }

    if (!haveVideo && haveAudio) {
        return true;
    }

    return false;
}

//FIXME all codecs: frameworks/av/media/libstagefright/codecs/*
static bool isCodecSupportedByStagefright(enum AVCodecID codec_id)
{
    bool supported = false;

    switch(codec_id) {
    //video
    case AV_CODEC_ID_HEVC:
    case AV_CODEC_ID_H264:
    case AV_CODEC_ID_MPEG4:
    case AV_CODEC_ID_H263:
    case AV_CODEC_ID_H263P:
    case AV_CODEC_ID_H263I:
    case AV_CODEC_ID_VP6:
    case AV_CODEC_ID_VP8:
    case AV_CODEC_ID_VP9:
    //audio
    case AV_CODEC_ID_AAC:
    case AV_CODEC_ID_MP3:
    case AV_CODEC_ID_AMR_NB:
    case AV_CODEC_ID_AMR_WB:
    case AV_CODEC_ID_VORBIS:
    case AV_CODEC_ID_PCM_MULAW: //g711
    case AV_CODEC_ID_PCM_ALAW:  //g711
    case AV_CODEC_ID_GSM_MS:
    case AV_CODEC_ID_PCM_U8:
    case AV_CODEC_ID_PCM_S16LE:
    case AV_CODEC_ID_PCM_S24LE:
    case AV_CODEC_ID_OPUS:
        supported = true;
        break;

    default:
        break;
    }

    ALOGD("%ssupported codec(%s) by official Stagefright",
            (supported ? "" : "un"),
            avcodec_get_name(codec_id));

    return supported;
}

static void adjustMPEG4Confidence(AVFormatContext *ic, float *confidence)
{
    AVDictionary *tags = NULL;
    AVDictionaryEntry *tag = NULL;
    enum AVCodecID codec_id = AV_CODEC_ID_NONE;
    bool is_mov = false;

    //1. check codec id
    codec_id = getCodecId(ic, AVMEDIA_TYPE_VIDEO);
    if (codec_id != AV_CODEC_ID_NONE
            && codec_id != AV_CODEC_ID_HEVC
            && codec_id != AV_CODEC_ID_H264
            && codec_id != AV_CODEC_ID_MPEG4
            && codec_id != AV_CODEC_ID_H263
            && codec_id != AV_CODEC_ID_H263P
            && codec_id != AV_CODEC_ID_H263I) {
        //the MEDIA_MIMETYPE_CONTAINER_MPEG4 of confidence is 0.4f
        ALOGI("[mp4] video codec(%s), confidence should be larger than MPEG4Extractor",
                avcodec_get_name(codec_id));
        *confidence = 0.41f;
    }

    codec_id = getCodecId(ic, AVMEDIA_TYPE_AUDIO);
    if (codec_id != AV_CODEC_ID_NONE
            && codec_id != AV_CODEC_ID_MP3
            && codec_id != AV_CODEC_ID_AAC
            && codec_id != AV_CODEC_ID_AMR_NB
            && codec_id != AV_CODEC_ID_AMR_WB) {
        ALOGI("[mp4] audio codec(%s), confidence should be larger than MPEG4Extractor",
                avcodec_get_name(codec_id));
        *confidence = 0.41f;
    }

    //2. check tag
    tags = ic->metadata;
    //NOTE: You can use command to show these tags,
    //e.g. "ffprobe -show_format 2012.mov"
    tag = av_dict_get(tags, "major_brand", NULL, 0);
    if (tag) {
        ALOGV("major_brand tag: %s", tag->value);

        //when MEDIA_MIMETYPE_CONTAINER_MPEG4
        //WTF, MPEG4Extractor.cpp can not extractor mov format
        //NOTE: isCompatibleBrand(MPEG4Extractor.cpp)
        //  Won't promise that the following file types can be played.
        //  Just give these file types a chance.
        //  FOURCC('q', 't', ' ', ' '),  // Apple's QuickTime
        //So......
        if (!strcmp(tag->value, "qt  ")) {
            ALOGI("[mp4] format is mov, confidence should be larger than mpeg4");
            *confidence = 0.41f;
            is_mov = true;
        }
    }
}

static void adjustMPEG2PSConfidence(AVFormatContext *ic, float *confidence)
{
    enum AVCodecID codec_id = AV_CODEC_ID_NONE;

    codec_id = getCodecId(ic, AVMEDIA_TYPE_VIDEO);
    if (codec_id != AV_CODEC_ID_NONE
            && codec_id != AV_CODEC_ID_H264
            && codec_id != AV_CODEC_ID_MPEG4
            && codec_id != AV_CODEC_ID_MPEG1VIDEO
            && codec_id != AV_CODEC_ID_MPEG2VIDEO) {
        //the MEDIA_MIMETYPE_CONTAINER_MPEG2TS of confidence is 0.25f
        ALOGI("[mpeg2ps] video codec(%s), confidence should be larger than MPEG2PSExtractor",
                avcodec_get_name(codec_id));
        *confidence = 0.26f;
    }

    codec_id = getCodecId(ic, AVMEDIA_TYPE_AUDIO);
    if (codec_id != AV_CODEC_ID_NONE
            && codec_id != AV_CODEC_ID_AAC
            && codec_id != AV_CODEC_ID_PCM_S16LE
            && codec_id != AV_CODEC_ID_PCM_S24LE
            && codec_id != AV_CODEC_ID_MP1
            && codec_id != AV_CODEC_ID_MP2
            && codec_id != AV_CODEC_ID_MP3) {
        ALOGI("[mpeg2ps] audio codec(%s), confidence should be larger than MPEG2PSExtractor",
                avcodec_get_name(codec_id));
        *confidence = 0.26f;
    }
}

static void adjustMPEG2TSConfidence(AVFormatContext *ic, float *confidence)
{
    enum AVCodecID codec_id = AV_CODEC_ID_NONE;

    codec_id = getCodecId(ic, AVMEDIA_TYPE_VIDEO);
    if (codec_id != AV_CODEC_ID_NONE
            && codec_id != AV_CODEC_ID_H264
            && codec_id != AV_CODEC_ID_MPEG4
            && codec_id != AV_CODEC_ID_MPEG1VIDEO
            && codec_id != AV_CODEC_ID_MPEG2VIDEO) {
        //the MEDIA_MIMETYPE_CONTAINER_MPEG2TS of confidence is 0.1f
        ALOGI("[mpeg2ts] video codec(%s), confidence should be larger than MPEG2TSExtractor",
                avcodec_get_name(codec_id));
        *confidence = 0.11f;
    }

    codec_id = getCodecId(ic, AVMEDIA_TYPE_AUDIO);
    if (codec_id != AV_CODEC_ID_NONE
            && codec_id != AV_CODEC_ID_AAC
            && codec_id != AV_CODEC_ID_PCM_S16LE
            && codec_id != AV_CODEC_ID_PCM_S24LE
            && codec_id != AV_CODEC_ID_MP1
            && codec_id != AV_CODEC_ID_MP2
            && codec_id != AV_CODEC_ID_MP3) {
        ALOGI("[mpeg2ts] audio codec(%s), confidence should be larger than MPEG2TSExtractor",
                avcodec_get_name(codec_id));
        *confidence = 0.11f;
    }
}

static void adjustMKVConfidence(AVFormatContext *ic, float *confidence)
{
    enum AVCodecID codec_id = AV_CODEC_ID_NONE;

    codec_id = getCodecId(ic, AVMEDIA_TYPE_VIDEO);
    if (codec_id != AV_CODEC_ID_NONE
            && codec_id != AV_CODEC_ID_H264
            && codec_id != AV_CODEC_ID_MPEG4
            && codec_id != AV_CODEC_ID_VP6
            && codec_id != AV_CODEC_ID_VP8
            && codec_id != AV_CODEC_ID_VP9) {
        //the MEDIA_MIMETYPE_CONTAINER_MATROSKA of confidence is 0.6f
        ALOGI("[mkv] video codec(%s), confidence should be larger than MatroskaExtractor",
                avcodec_get_name(codec_id));
        *confidence = 0.61f;
    }

    codec_id = getCodecId(ic, AVMEDIA_TYPE_AUDIO);
    if (codec_id != AV_CODEC_ID_NONE
            && codec_id != AV_CODEC_ID_AAC
            && codec_id != AV_CODEC_ID_MP3
            && codec_id != AV_CODEC_ID_OPUS
            && codec_id != AV_CODEC_ID_VORBIS) {
        ALOGI("[mkv] audio codec(%s), confidence should be larger than MatroskaExtractor",
                avcodec_get_name(codec_id));
        *confidence = 0.61f;
    }
}

static void adjustCodecConfidence(AVFormatContext *ic, float *confidence)
{
    enum AVCodecID codec_id = AV_CODEC_ID_NONE;

    codec_id = getCodecId(ic, AVMEDIA_TYPE_VIDEO);
    if (codec_id != AV_CODEC_ID_NONE) {
        if (!isCodecSupportedByStagefright(codec_id)) {
            *confidence = 0.88f;
        }
    }

    codec_id = getCodecId(ic, AVMEDIA_TYPE_AUDIO);
    if (codec_id != AV_CODEC_ID_NONE) {
        if (!isCodecSupportedByStagefright(codec_id)) {
            *confidence = 0.88f;
        }
    }

    if (getCodecId(ic, AVMEDIA_TYPE_VIDEO) != AV_CODEC_ID_NONE
            && getCodecId(ic, AVMEDIA_TYPE_AUDIO) == AV_CODEC_ID_MP3) {
        *confidence = 0.22f; //larger than MP3Extractor
    }
}

//TODO need more checks
static void adjustConfidenceIfNeeded(const char *mime,
        AVFormatContext *ic, float *confidence)
{
    //1. check mime
    if (!strcasecmp(mime, MEDIA_MIMETYPE_CONTAINER_MPEG4)) {
        adjustMPEG4Confidence(ic, confidence);
    } else if (!strcasecmp(mime, MEDIA_MIMETYPE_CONTAINER_MPEG2TS)) {
        adjustMPEG2TSConfidence(ic, confidence);
    } else if (!strcasecmp(mime, MEDIA_MIMETYPE_CONTAINER_MPEG2PS)) {
        adjustMPEG2PSConfidence(ic, confidence);
    } else if (!strcasecmp(mime, MEDIA_MIMETYPE_CONTAINER_MATROSKA)) {
        adjustMKVConfidence(ic, confidence);
    } else if (!strcasecmp(mime, MEDIA_MIMETYPE_CONTAINER_DIVX)) {
        *confidence = 0.4f;
    } else {
        //todo here
    }

    //2. check codec
    adjustCodecConfidence(ic, confidence);
}

static void adjustContainerIfNeeded(const char **mime, AVFormatContext *ic)
{
    const char *newMime = *mime;
    enum AVCodecID codec_id = AV_CODEC_ID_NONE;

    AVCodecParameters *avpar = getCodecParameters(ic, AVMEDIA_TYPE_VIDEO);
    if (avpar != NULL && getDivXVersion(avpar) >= 0) {
        newMime = MEDIA_MIMETYPE_VIDEO_DIVX;

    } else if (hasAudioCodecOnly(ic)) {
        codec_id = getCodecId(ic, AVMEDIA_TYPE_AUDIO);
        CHECK(codec_id != AV_CODEC_ID_NONE);
        switch (codec_id) {
        case AV_CODEC_ID_MP3:
            newMime = MEDIA_MIMETYPE_AUDIO_MPEG;
            break;
        case AV_CODEC_ID_AAC:
            newMime = MEDIA_MIMETYPE_AUDIO_AAC;
            break;
        case AV_CODEC_ID_VORBIS:
            newMime = MEDIA_MIMETYPE_AUDIO_VORBIS;
            break;
        case AV_CODEC_ID_FLAC:
            newMime = MEDIA_MIMETYPE_AUDIO_FLAC;
            break;
        case AV_CODEC_ID_AC3:
            newMime = MEDIA_MIMETYPE_AUDIO_AC3;
            break;
        case AV_CODEC_ID_APE:
            newMime = MEDIA_MIMETYPE_AUDIO_APE;
            break;
        case AV_CODEC_ID_DTS:
            newMime = MEDIA_MIMETYPE_AUDIO_DTS;
            break;
        case AV_CODEC_ID_MP2:
            newMime = MEDIA_MIMETYPE_AUDIO_MPEG_LAYER_II;
            break;
        case AV_CODEC_ID_COOK:
            newMime = MEDIA_MIMETYPE_AUDIO_RA;
            break;
        case AV_CODEC_ID_WMAV1:
        case AV_CODEC_ID_WMAV2:
        case AV_CODEC_ID_WMAPRO:
        case AV_CODEC_ID_WMALOSSLESS:
            newMime = MEDIA_MIMETYPE_AUDIO_WMA;
            break;
        case AV_CODEC_ID_ALAC:
            newMime = MEDIA_MIMETYPE_AUDIO_ALAC;
            break;
        default:
            break;
        }

        if (!strcmp(*mime, MEDIA_MIMETYPE_CONTAINER_FFMPEG)) {
            newMime = MEDIA_MIMETYPE_AUDIO_FFMPEG;
        }
    }

    if (strcmp(*mime, newMime)) {
        ALOGI("adjust mime(%s -> %s)", *mime, newMime);
        *mime = newMime;
    }
}

static const char *findMatchingContainer(const char *name)
{
    size_t i = 0;
#if SUPPOURT_UNKNOWN_FORMAT
    //The FFmpegExtractor support all ffmpeg formats!!!
    //Unknown format is defined as MEDIA_MIMETYPE_CONTAINER_FFMPEG
    const char *container = MEDIA_MIMETYPE_CONTAINER_FFMPEG;
#else
    const char *container = NULL;
#endif

    for (i = 0; i < NELEM(FILE_FORMATS); ++i) {
        int len = strlen(FILE_FORMATS[i].format);
        if (!strncasecmp(name, FILE_FORMATS[i].format, len)) {
            container = FILE_FORMATS[i].container;
            break;
        }
    }

    return container;
}

static const char *SniffFFMPEGCommon(const char *url, float *confidence, bool isStreaming)
{
    int err = 0;
    size_t i = 0;
    size_t nb_streams = 0;
    int64_t timeNow = 0;
    const char *container = NULL;
    AVFormatContext *ic = NULL;
    AVDictionary *codec_opts = NULL;
    AVDictionary **opts = NULL;
    bool needProbe = false;

    static status_t status = initFFmpeg();
    if (status != OK) {
        ALOGE("could not init ffmpeg");
        return NULL;
    }

    ic = avformat_alloc_context();
    if (!ic)
    {
        ALOGE("oom for alloc avformat context");
        goto fail;
    }

    // Don't download more than a meg
    ic->probesize = 1024 * 1024;

    timeNow = ALooper::GetNowUs();

    err = avformat_open_input(&ic, url, NULL, NULL);

    if (err < 0) {
        ALOGE("avformat_open_input(%s) failed: %s (%08x)", url, av_err2str(err), err);
        goto fail;
    }

    if (ic->iformat != NULL && ic->iformat->name != NULL) {
        container = findMatchingContainer(ic->iformat->name);
    }

    ALOGV("opened, nb_streams: %d container: %s delay: %.2f ms", ic->nb_streams, container,
            ((float)ALooper::GetNowUs() - timeNow) / 1000);

    // Only probe if absolutely necessary. For formats with headers, avformat_open_input will
    // figure out the components.
    for (unsigned int i = 0; i < ic->nb_streams; i++) {
        AVStream* stream = ic->streams[i];
        if (!stream->codecpar || !stream->codecpar->codec_id) {
            needProbe = true;
            break;
        }
        ALOGV("found stream %d id %d codec %s", i, stream->codecpar->codec_id, avcodec_get_name(stream->codecpar->codec_id));
    }

    // We must go deeper.
    if (!isStreaming && (!ic->nb_streams || needProbe)) {
        timeNow = ALooper::GetNowUs();

        opts = setup_find_stream_info_opts(ic, codec_opts);
        nb_streams = ic->nb_streams;
        err = avformat_find_stream_info(ic, opts);
        if (err < 0) {
            ALOGE("avformat_find_stream_info(%s) failed: %s (%08x)", url, av_err2str(err), err);
            goto fail;
        }

        ALOGV("probed stream info after %.2f ms", ((float)ALooper::GetNowUs() - timeNow) / 1000);

        for (i = 0; i < nb_streams; i++) {
            av_dict_free(&opts[i]);
        }
        av_freep(&opts);

        av_dump_format(ic, 0, url, 0);
    }

    ALOGV("sniff(%s): format_name: %s, format_long_name: %s",
            url, ic->iformat->name, ic->iformat->long_name);

    container = findMatchingContainer(ic->iformat->name);
    if (container) {
        adjustContainerIfNeeded(&container, ic);
        adjustConfidenceIfNeeded(container, ic, confidence);
        if (*confidence == 0)
            container = NULL;
    }

fail:
    if (ic) {
        avformat_close_input(&ic);
    }

    return container;
}

static const char *BetterSniffFFMPEG(CDataSource *source,
        float *confidence, AMessage *meta)
{
    const char *ret = NULL;
    char url[PATH_MAX] = {0};

    ALOGI("android-source:%p", source);

    // pass the addr of smart pointer("source")
    snprintf(url, sizeof(url), "android-source:%p", source);

    ret = SniffFFMPEGCommon(url, confidence,
            (source->flags(source->handle) & DataSourceBase::kIsCachingDataSource));
    if (ret) {
        meta->setString("extended-extractor-url", url);
    }

    return ret;
}

static const char *LegacySniffFFMPEG(CDataSource *source,
         float *confidence, AMessage *meta)
{
    const char *ret = NULL;
    char uri[PATH_MAX] = {0};
    char url[PATH_MAX] = {0};

    if (!source->getUri(source->handle, uri, sizeof(uri))) {
        return NULL;
    }

    if (source->flags(source->handle) & DataSourceBase::kIsCachingDataSource)
       return NULL;

    ALOGV("source url: %s", uri);

    // pass the addr of smart pointer("source") + file name
    snprintf(url, sizeof(url), "android-source:%p|file:%s", source, uri);

    ret = SniffFFMPEGCommon(url, confidence, false);
    if (ret) {
        meta->setString("extended-extractor-url", url);
    }

    return ret;
}

static void FreeMeta(void *meta) {
    if (meta != nullptr) {
        static_cast<AMessage *>(meta)->decStrong(nullptr);
    }
}

static CreatorFunc
SniffFFMPEG(
        CDataSource *source, float *confidence, void **meta,
        FreeMetaFunc *freeMeta) {

    float newConfidence = 0.08f;

    ALOGV("SniffFFMPEG (initial confidence: %f)", *confidence);

    // This is a heavyweight sniffer, don't invoke it if Stagefright knows
    // what it is doing already.
    if (confidence != NULL) {
        if (*confidence > 0.8f) {
            return NULL;
        }
    }

    AMessage *msg = new AMessage;

    *meta = msg;
    *freeMeta = FreeMeta;
    msg->incStrong(nullptr);

    const char *container = BetterSniffFFMPEG(source, &newConfidence, msg);
    if (!container) {
        ALOGW("sniff through BetterSniffFFMPEG failed, try LegacySniffFFMPEG");
        container = LegacySniffFFMPEG(source, &newConfidence, msg);
        if (container) {
            ALOGV("sniff through LegacySniffFFMPEG success");
        }
    } else {
        ALOGV("sniff through BetterSniffFFMPEG success");
    }

    if (container == NULL) {
        ALOGD("SniffFFMPEG failed to sniff this source");
        msg->decStrong(nullptr);
        *meta = NULL;
        *freeMeta = NULL;
        return NULL;
    }

    ALOGD("ffmpeg detected media content as '%s' with confidence %.2f",
            container, newConfidence);

    msg->setString("extended-extractor", "extended-extractor");
    msg->setString("extended-extractor-subtype", "ffmpegextractor");
    msg->setString("extended-extractor-mime", container);

    //debug only
    char value[PROPERTY_VALUE_MAX];
    property_get("sys.media.parser.ffmpeg", value, "0");
    if (atoi(value)) {
        ALOGD("[debug] use ffmpeg parser");
        newConfidence = 0.88f;
    }

    if (newConfidence > *confidence) {
        msg->setString("extended-extractor-use", "ffmpegextractor");
        *confidence = newConfidence;
    }

    return CreateFFMPEGExtractor;
}

static CMediaExtractor *CreateFFMPEGExtractor(CDataSource *source, void *meta) {
    CMediaExtractor *ret = NULL;
    sp<AMessage> msg = static_cast<AMessage *>(meta);
    AString mime;
    if (msg.get() && msg->findString("extended-extractor-mime", &mime) && (
            !strcasecmp(mime.c_str(), MEDIA_MIMETYPE_AUDIO_MPEG)          ||
            !strcasecmp(mime.c_str(), MEDIA_MIMETYPE_AUDIO_AAC)           ||
            !strcasecmp(mime.c_str(), MEDIA_MIMETYPE_AUDIO_VORBIS)        ||
            !strcasecmp(mime.c_str(), MEDIA_MIMETYPE_AUDIO_ALAC)          ||
            !strcasecmp(mime.c_str(), MEDIA_MIMETYPE_AUDIO_FLAC)          ||
            !strcasecmp(mime.c_str(), MEDIA_MIMETYPE_AUDIO_AC3)           ||
            !strcasecmp(mime.c_str(), MEDIA_MIMETYPE_AUDIO_APE)           ||
            !strcasecmp(mime.c_str(), MEDIA_MIMETYPE_AUDIO_DTS)           ||
            !strcasecmp(mime.c_str(), MEDIA_MIMETYPE_AUDIO_MPEG_LAYER_II) ||
            !strcasecmp(mime.c_str(), MEDIA_MIMETYPE_AUDIO_RA)            ||
            !strcasecmp(mime.c_str(), MEDIA_MIMETYPE_AUDIO_WMA)           ||
            !strcasecmp(mime.c_str(), MEDIA_MIMETYPE_AUDIO_FFMPEG)        ||
            !strcasecmp(mime.c_str(), MEDIA_MIMETYPE_CONTAINER_MPEG4)     ||
            !strcasecmp(mime.c_str(), MEDIA_MIMETYPE_CONTAINER_MOV)       ||
            !strcasecmp(mime.c_str(), MEDIA_MIMETYPE_CONTAINER_MATROSKA)  ||
            !strcasecmp(mime.c_str(), MEDIA_MIMETYPE_CONTAINER_TS)        ||
            !strcasecmp(mime.c_str(), MEDIA_MIMETYPE_CONTAINER_MPEG2PS)   ||
            !strcasecmp(mime.c_str(), MEDIA_MIMETYPE_CONTAINER_AVI)       ||
            !strcasecmp(mime.c_str(), MEDIA_MIMETYPE_CONTAINER_ASF)       ||
            !strcasecmp(mime.c_str(), MEDIA_MIMETYPE_CONTAINER_WEBM)      ||
            !strcasecmp(mime.c_str(), MEDIA_MIMETYPE_CONTAINER_WMV)       ||
            !strcasecmp(mime.c_str(), MEDIA_MIMETYPE_CONTAINER_MPG)       ||
            !strcasecmp(mime.c_str(), MEDIA_MIMETYPE_CONTAINER_FLV)       ||
            !strcasecmp(mime.c_str(), MEDIA_MIMETYPE_CONTAINER_DIVX)      ||
            !strcasecmp(mime.c_str(), MEDIA_MIMETYPE_CONTAINER_RM)        ||
            !strcasecmp(mime.c_str(), MEDIA_MIMETYPE_CONTAINER_WAV)       ||
            !strcasecmp(mime.c_str(), MEDIA_MIMETYPE_CONTAINER_FLAC)      ||
            !strcasecmp(mime.c_str(), MEDIA_MIMETYPE_CONTAINER_APE)       ||
            !strcasecmp(mime.c_str(), MEDIA_MIMETYPE_CONTAINER_DTS)       ||
            !strcasecmp(mime.c_str(), MEDIA_MIMETYPE_CONTAINER_MP2)       ||
            !strcasecmp(mime.c_str(), MEDIA_MIMETYPE_CONTAINER_RA)        ||
            !strcasecmp(mime.c_str(), MEDIA_MIMETYPE_CONTAINER_OGG)       ||
            !strcasecmp(mime.c_str(), MEDIA_MIMETYPE_CONTAINER_VC1)       ||
            !strcasecmp(mime.c_str(), MEDIA_MIMETYPE_CONTAINER_HEVC)      ||
            !strcasecmp(mime.c_str(), MEDIA_MIMETYPE_CONTAINER_WMA)       ||
            !strcasecmp(mime.c_str(), MEDIA_MIMETYPE_CONTAINER_FFMPEG))) {
        ret = wrap(new FFmpegExtractor(new DataSourceHelper(source), msg));
    }

    ALOGD("%ssupported mime: %s", (ret ? "" : "un"), mime.c_str());
    return ret;
}

static const char* extensions[] = {
    "adts",
    "dm", "m2ts", "mp3d", "wmv", "asf", "flv", ".ra",
    "rm", "rmvb", "ac3", "ape", "dts", "mp1", "mp2",
    "f4v", "hlv", "nrg", "m2v", "swf", "vc1", "vob",
    "divx", "qcp", "ec3"
};

extern "C" {

__attribute__ ((visibility ("default")))
ExtractorDef GETEXTRACTORDEF() {
    return {
        EXTRACTORDEF_VERSION,
        UUID("280e1e71-d08b-4d8c-ba03-d775497fc4bc"),
        1, // version
        "FFMPEG Extractor",
        { .v3 = { SniffFFMPEG, extensions } }
    };
}

}

};  // namespace android
