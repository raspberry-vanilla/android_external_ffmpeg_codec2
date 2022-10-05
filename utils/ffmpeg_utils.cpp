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

#define LOG_TAG "FFMPEG"
#include <utils/Log.h>

#include <utils/Errors.h>

extern "C" {

#include "config.h"

#include <unistd.h>
#include <stdlib.h>
#include <inttypes.h>
#include <math.h>
#include <limits.h> /* INT_MAX */
#include <time.h>

#undef strncpy
#include <string.h>

}

#include <cutils/properties.h>

#include "ffmpeg_utils.h"
#include "ffmpeg_source.h"

// log
static int flags;

// init ffmpeg
static pthread_mutex_t s_init_mutex = PTHREAD_MUTEX_INITIALIZER;
static int s_ref_count = 0;

namespace android {

//////////////////////////////////////////////////////////////////////////////////
// log
//////////////////////////////////////////////////////////////////////////////////
static void sanitize(uint8_t *line){
    while(*line){
        if(*line < 0x08 || (*line > 0x0D && *line < 0x20))
            *line='?';
        line++;
    }
}

// TODO, remove static variables to support multi-instances
void nam_av_log_callback(void* ptr, int level, const char* fmt, va_list vl)
{
    static int print_prefix = 1;
    static int count;
    static char prev[1024];
    char line[1024];

    if (level > av_log_get_level())
        return;
    av_log_format_line(ptr, level, fmt, vl, line, sizeof(line), &print_prefix);

    if (print_prefix && (flags & AV_LOG_SKIP_REPEATED) && !strcmp(line, prev)){
        count++;
        return;
    }
    if (count > 0) {
        ALOGI("Last message repeated %d times\n", count);
        count = 0;
    }
    strcpy(prev, line);
    sanitize((uint8_t *)line);

#if 0
    ALOGI("%s", line);
#else
#define LOG_BUF_SIZE 1024
    static char g_msg[LOG_BUF_SIZE];
    static int g_msg_len = 0;

    int saw_lf, check_len;

    do {
        check_len = g_msg_len + strlen(line) + 1;
        if (check_len <= LOG_BUF_SIZE) {
            /* lf: Line feed ('\n') */
            saw_lf = (strchr(line, '\n') != NULL) ? 1 : 0;
            strncpy(g_msg + g_msg_len, line, strlen(line));
            g_msg_len += strlen(line);
            if (!saw_lf) {
               /* skip */
               return;
            } else {
               /* attach the line feed */
               g_msg_len += 1;
               g_msg[g_msg_len] = '\n';
            }
        } else {
            /* trace is fragmented */
            g_msg_len += 1;
            g_msg[g_msg_len] = '\n';
        }
        ALOGI("%s", g_msg);
        /* reset g_msg and g_msg_len */
        memset(g_msg, 0, LOG_BUF_SIZE);
        g_msg_len = 0;
     } while (check_len > LOG_BUF_SIZE);
#endif
}

void nam_av_log_set_flags(int arg)
{
    flags = arg;
}

#if 0
const struct { const char *name; int level; } log_levels[] = {
    { "quiet"  , AV_LOG_QUIET   },
    { "panic"  , AV_LOG_PANIC   },
    { "fatal"  , AV_LOG_FATAL   },
    { "error"  , AV_LOG_ERROR   },
    { "warning", AV_LOG_WARNING },
    { "info"   , AV_LOG_INFO    },
    { "verbose", AV_LOG_VERBOSE },
    { "debug"  , AV_LOG_DEBUG   },
};

#define AV_LOG_QUIET    -8
#define AV_LOG_PANIC     0
#define AV_LOG_FATAL     8
#define AV_LOG_ERROR    16
#define AV_LOG_WARNING  24
#define AV_LOG_INFO     32
#define AV_LOG_VERBOSE  40
#define AV_LOG_DEBUG    48
#endif

//////////////////////////////////////////////////////////////////////////////////
// constructor and destructor
//////////////////////////////////////////////////////////////////////////////////

/**
 * To debug ffmpeg", type this command on the console before starting playback:
 *     setprop debug.nam.ffmpeg 1
 * To disable the debug, type:
 *     setprop debug.nam.ffmpge 0
*/
status_t initFFmpeg() 
{
    status_t ret = OK;

    pthread_mutex_lock(&s_init_mutex);

    if (property_get_bool("debug.nam.ffmpeg", 0))
        av_log_set_level(AV_LOG_DEBUG);
    else
        av_log_set_level(AV_LOG_INFO);

    if(s_ref_count == 0) {
        nam_av_log_set_flags(AV_LOG_SKIP_REPEATED);
        av_log_set_callback(nam_av_log_callback);

        /* global ffmpeg initialization */
        avformat_network_init();

        /* register android source */
        ffmpeg_register_android_source();

        ALOGI("FFMPEG initialized: %s", av_version_info());
    }

    // update counter
    s_ref_count++;

    pthread_mutex_unlock(&s_init_mutex);

    return ret;
}

void deInitFFmpeg()
{
    pthread_mutex_lock(&s_init_mutex);

    // update counter
    s_ref_count--;

    if(s_ref_count == 0) {
        avformat_network_deinit();
        ALOGD("FFMPEG deinitialized");
    }

    pthread_mutex_unlock(&s_init_mutex);
}

//////////////////////////////////////////////////////////////////////////////////
// parser
//////////////////////////////////////////////////////////////////////////////////
/* H.264 bitstream with start codes, NOT AVC1! */
static int h264_split(AVCodecParameters *avpar __unused,
        const uint8_t *buf, int buf_size, int check_compatible_only)
{
    int i;
    uint32_t state = -1;
    int has_sps= 0;
    int has_pps= 0;

    //av_hex_dump(stderr, buf, 100);

    for(i=0; i<=buf_size; i++){
        if((state&0xFFFFFF1F) == 0x107) {
            ALOGI("found NAL_SPS");
            has_sps=1;
        }
        if((state&0xFFFFFF1F) == 0x108) {
            ALOGI("found NAL_PPS");
            has_pps=1;
            if (check_compatible_only)
                return (has_sps & has_pps);
        }
        if((state&0xFFFFFF00) == 0x100
                && ((state&0xFFFFFF1F) == 0x101
                    || (state&0xFFFFFF1F) == 0x102
                    || (state&0xFFFFFF1F) == 0x105)){
            if(has_pps){
                while(i>4 && buf[i-5]==0) i--;
                return i-4;
            }
        }
        if (i<buf_size)
            state= (state<<8) | buf[i];
    }
    return 0;
}

static int mpegvideo_split(AVCodecParameters *avpar __unused,
        const uint8_t *buf, int buf_size, int check_compatible_only __unused)
{
    int i;
    uint32_t state= -1;
    int found=0;

    for(i=0; i<buf_size; i++){
        state= (state<<8) | buf[i];
        if(state == 0x1B3){
            found=1;
        }else if(found && state != 0x1B5 && state < 0x200 && state >= 0x100)
            return i-3;
    }
    return 0;
}

/* split extradata from buf for Android OMXCodec */
int parser_split(AVCodecParameters *avpar,
        const uint8_t *buf, int buf_size)
{
    if (!avpar || !buf || buf_size <= 0) {
        ALOGE("parser split, valid params");
        return 0;
    }

    if (avpar->codec_id == AV_CODEC_ID_H264) {
        return h264_split(avpar, buf, buf_size, 0);
    } else if (avpar->codec_id == AV_CODEC_ID_MPEG2VIDEO ||
            avpar->codec_id == AV_CODEC_ID_MPEG4) {
        return mpegvideo_split(avpar, buf, buf_size, 0);
    } else {
        ALOGE("parser split, unsupport the codec, id: 0x%0x", avpar->codec_id);
    }

    return 0;
}

int is_extradata_compatible_with_android(AVCodecParameters *avpar)
{
    if (avpar->extradata_size <= 0) {
        ALOGI("extradata_size <= 0, extradata is not compatible with "
                "android decoder, the codec id: 0x%0x", avpar->codec_id);
        return 0;
    }

    if (avpar->codec_id == AV_CODEC_ID_H264
            && avpar->extradata[0] != 1 /* configurationVersion */) {
        // SPS + PPS
        return !!(h264_split(avpar, avpar->extradata,
                    avpar->extradata_size, 1) > 0);
    } else {
        // default, FIXME
        return !!(avpar->extradata_size > 0);
    }
}

//////////////////////////////////////////////////////////////////////////////////
// packet queue
//////////////////////////////////////////////////////////////////////////////////

typedef struct PacketList {
    AVPacket *pkt;
    struct PacketList *next;
} PacketList;

typedef struct PacketQueue {
    PacketList *first_pkt, *last_pkt;
    int nb_packets;
    int size;
    int wait_for_data;
    int abort_request;
    Mutex lock;
    Condition cond;
} PacketQueue;

PacketQueue* packet_queue_alloc()
{
    PacketQueue *queue = (PacketQueue*)av_mallocz(sizeof(PacketQueue));
    if (queue) {
        queue->abort_request = 1;
        return queue;
    }
    return NULL;
}

void packet_queue_free(PacketQueue **q)
{
    packet_queue_abort(*q);
    packet_queue_flush(*q);
    av_freep(q);
}

void packet_queue_abort(PacketQueue *q)
{
    q->abort_request = 1;
    Mutex::Autolock autoLock(q->lock);
    q->cond.signal();
}

static int packet_queue_put_private(PacketQueue *q, AVPacket *pkt)
{
    PacketList *pkt1;

    if (q->abort_request)
        return -1;

    pkt1 = (PacketList *)av_malloc(sizeof(PacketList));
    if (!pkt1)
        return -1;
    pkt1->pkt = av_packet_alloc();
    if (!pkt1->pkt) {
        av_free(pkt1);
        return -1;
    }
    av_packet_move_ref(pkt1->pkt, pkt);
    pkt1->next = NULL;

    if (!q->last_pkt)
        q->first_pkt = pkt1;
    else
        q->last_pkt->next = pkt1;
    q->last_pkt = pkt1;
    q->nb_packets++;
    //q->size += pkt1->pkt.size + sizeof(*pkt1);
    q->size += pkt1->pkt->size;
    q->cond.signal();
    return 0;
}

int packet_queue_put(PacketQueue *q, AVPacket *pkt)
{
    int ret;

    q->lock.lock();
    ret = packet_queue_put_private(q, pkt);
    q->lock.unlock();

    return ret;
}

int packet_queue_is_wait_for_data(PacketQueue *q)
{
    Mutex::Autolock autoLock(q->lock);
    return q->wait_for_data;
}

void packet_queue_flush(PacketQueue *q)
{
    PacketList *pkt, *pkt1;

    Mutex::Autolock autoLock(q->lock);
    for (pkt = q->first_pkt; pkt != NULL; pkt = pkt1) {
        pkt1 = pkt->next;
        av_packet_free(&pkt->pkt);
        av_freep(&pkt);
    }
    q->last_pkt = NULL;
    q->first_pkt = NULL;
    q->nb_packets = 0;
    q->size = 0;
}

int packet_queue_put_nullpacket(PacketQueue *q, int stream_index)
{
    AVPacket *pkt;
    int err;

    pkt = av_packet_alloc();
    pkt->data = NULL;
    pkt->size = 0;
    pkt->stream_index = stream_index;
    err = packet_queue_put(q, pkt);
    av_packet_free(&pkt);

    return err;
}

/* packet queue handling */
/* return < 0 if aborted, 0 if no packet and > 0 if packet.  */
int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block)
{
    PacketList *pkt1;
    int ret = -1;

    Mutex::Autolock autoLock(q->lock);

    while (!q->abort_request) {
        pkt1 = q->first_pkt;
        if (pkt1) {
            q->first_pkt = pkt1->next;
            if (!q->first_pkt)
                q->last_pkt = NULL;
            q->nb_packets--;
            //q->size -= pkt1->pkt.size + sizeof(*pkt1);
            q->size -= pkt1->pkt->size;
            av_packet_move_ref(pkt, pkt1->pkt);
            av_packet_free(&pkt1->pkt);
            av_free(pkt1);
            ret = 1;
            break;
        } else if (!block) {
            ret = 0;
            break;
        } else {
            q->wait_for_data = 1;
            q->cond.waitRelative(q->lock, 10000000LL);
        }
    }
    q->wait_for_data = 0;
    return ret;
}

void packet_queue_start(PacketQueue *q)
{
    Mutex::Autolock autoLock(q->lock);
    q->abort_request = 0;
}

//////////////////////////////////////////////////////////////////////////////////
// misc
//////////////////////////////////////////////////////////////////////////////////
bool setup_vorbis_extradata(uint8_t **extradata, int *extradata_size,
        const uint8_t *header_start[3], const int header_len[3])
{
    uint8_t *p = NULL;
    int len = 0;
    int i = 0;

    len = header_len[0] + header_len[1] + header_len[2];
    p = *extradata = (uint8_t *)av_mallocz(64 + len + len/255);
    if (!p) {
        ALOGE("oom for vorbis extradata");
        return false;
    }

    *p++ = 2;
    p += av_xiphlacing(p, header_len[0]);
    p += av_xiphlacing(p, header_len[1]);
    for (i = 0; i < 3; i++) {
        if (header_len[i] > 0) {
            memcpy(p, header_start[i], header_len[i]);
            p += header_len[i];
        }
    }
    *extradata_size = p - *extradata;

    return true;
}

int64_t get_timestamp() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

audio_format_t to_android_audio_format(enum AVSampleFormat fmt) {
    AVSampleFormat packed = av_get_packed_sample_fmt(fmt);
    if (packed == AV_SAMPLE_FMT_U8)
        return AUDIO_FORMAT_PCM_8_BIT;
    if (packed == AV_SAMPLE_FMT_S16)
        return AUDIO_FORMAT_PCM_16_BIT;
    if (packed == AV_SAMPLE_FMT_S32)
        return AUDIO_FORMAT_PCM_32_BIT;
    if (packed == AV_SAMPLE_FMT_FLT)
        return AUDIO_FORMAT_PCM_FLOAT;
    return AUDIO_FORMAT_DEFAULT;
}

}  // namespace android

