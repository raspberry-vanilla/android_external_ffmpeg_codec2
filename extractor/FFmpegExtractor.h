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

#ifndef SUPER_EXTRACTOR_H_

#define SUPER_EXTRACTOR_H_

#include <media/MediaExtractorPluginApi.h>
#include <media/MediaExtractorPluginHelper.h>
#include <media/NdkMediaFormat.h>
#include <media/stagefright/foundation/ABase.h>
#include <utils/threads.h>
#include <utils/KeyedVector.h>

#include "ffmpeg_utils.h"

namespace android {

struct ABuffer;
struct AMessage;
class String8;
struct FFmpegSource;

struct FFmpegExtractor : public MediaExtractorPluginHelper {
    FFmpegExtractor(DataSourceHelper *source, const sp<AMessage> &meta);

    virtual size_t countTracks();
    virtual MediaTrackHelper* getTrack(size_t index);
    virtual media_status_t getTrackMetaData(AMediaFormat *meta, size_t index, uint32_t flags);

    virtual media_status_t getMetaData(AMediaFormat *meta);

    virtual uint32_t flags() const;
    virtual const char* name() { return "FFmpegExtractor"; }

protected:
    virtual ~FFmpegExtractor();

private:
    friend struct FFmpegSource;

    struct TrackInfo {
        int mIndex; //stream index
        AMediaFormat *mMeta;
        AVStream *mStream;
        PacketQueue *mQueue;
    };

    Vector<TrackInfo> mTracks;

    mutable Mutex mLock;
    mutable Mutex mExtractorMutex;
    Condition mCondition;

    DataSourceHelper *mDataSource;
    AMediaFormat *mMeta;
    status_t mInitCheck;

    char mFilename[PATH_MAX];
    int mGenPTS;
    int mVideoDisable;
    int mAudioDisable;
    int mShowStatus;
    int mSeekByBytes;
    int mAutoExit;
    int64_t mStartTime;
    int64_t mDuration;
    int mLoop;
    bool mEOF;
    size_t mProbePkts;

    int mAbortRequest;
    int mPaused;
    int mLastPaused;
    int mSeekIdx;
    MediaTrackHelper::ReadOptions::SeekMode mSeekMode;
    int64_t mSeekPos;
    int64_t mSeekMin;
    int64_t mSeekMax;

    int mReadPauseReturn;
    PacketQueue mAudioQ;
    PacketQueue mVideoQ;
    bool mVideoEOSReceived;
    bool mAudioEOSReceived;

    AVFormatContext *mFormatCtx;
    int mVideoStreamIdx;
    int mAudioStreamIdx;
    AVStream *mVideoStream;
    AVStream *mAudioStream;
    bool mDefersToCreateVideoTrack;
    bool mDefersToCreateAudioTrack;
    AVBSFContext *mVideoBsfc;
    AVBSFContext *mAudioBsfc;

    static int decode_interrupt_cb(void *ctx);
    int initStreams();
    void deInitStreams();
    void fetchStuffsFromSniffedMeta(const sp<AMessage> &meta);
    void setFFmpegDefaultOpts();
    void printTime(int64_t time);
    bool is_codec_supported(enum AVCodecID codec_id);
    media_status_t setVideoFormat(AVStream *stream, AMediaFormat *meta);
    media_status_t setAudioFormat(AVStream *stream, AMediaFormat *meta);
    void setDurationMetaData(AVStream *stream, AMediaFormat *meta);
    int stream_component_open(int stream_index);
    void stream_component_close(int stream_index);
    void reachedEOS(enum AVMediaType media_type);
    int stream_seek(int64_t pos, enum AVMediaType media_type,
            MediaTrackHelper::ReadOptions::SeekMode mode);
    int check_extradata(AVCodecParameters *avpar);

    bool mReaderThreadStarted;
    pthread_t mReaderThread;
    status_t startReaderThread();
    void stopReaderThread();
    static void *ReaderWrapper(void *me);
    void readerEntry();

    bool mParsedMetadata;

    DISALLOW_EVIL_CONSTRUCTORS(FFmpegExtractor);
};

}  // namespace android

#endif  // SUPER_EXTRACTOR_H_

