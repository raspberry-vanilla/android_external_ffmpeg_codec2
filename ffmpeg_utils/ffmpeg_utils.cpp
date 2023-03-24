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

//////////////////////////////////////////////////////////////////////////////////
// constructor and destructor
//////////////////////////////////////////////////////////////////////////////////

static int parseLogLevel(const char* s) {
    if (strcmp(s, "quiet") == 0)
        return AV_LOG_QUIET;
    else if (strcmp(s, "panic") == 0)
        return AV_LOG_PANIC;
    else if (strcmp(s, "fatal") == 0)
        return AV_LOG_FATAL;
    else if (strcmp(s, "error") == 0)
        return AV_LOG_ERROR;
    else if (strcmp(s, "warning") == 0)
        return AV_LOG_WARNING;
    else if (strcmp(s, "info") == 0)
        return AV_LOG_INFO;
    else if (strcmp(s, "verbose") == 0)
        return AV_LOG_VERBOSE;
    else if (strcmp(s, "debug") == 0)
        return AV_LOG_DEBUG;
    else if (strcmp(s, "trace") == 0)
        return AV_LOG_TRACE;
    else {
        ALOGE("unsupported loglevel: %s", s);
        return AV_LOG_INFO;
    }
}

/**
 * To set ffmpeg log level, type this command on the console before starting playback:
 *     setprop debug.ffmpeg.loglevel [quiet|panic|fatal|error|warning|info|verbose|debug|trace]
*/
status_t initFFmpeg() 
{
    status_t ret = OK;
    char pval[PROPERTY_VALUE_MAX];

    pthread_mutex_lock(&s_init_mutex);

    if (property_get("debug.ffmpeg.loglevel", pval, "info")) {
        av_log_set_level(parseLogLevel(pval));
    } else {
        av_log_set_level(AV_LOG_INFO);
    }

    if(s_ref_count == 0) {
        nam_av_log_set_flags(AV_LOG_SKIP_REPEATED);
        av_log_set_callback(nam_av_log_callback);

        /* global ffmpeg initialization */
        avformat_network_init();

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

}  // namespace android

