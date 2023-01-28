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

#ifndef FFMPEG_UTILS_H_

#define FFMPEG_UTILS_H_

#include <unistd.h>
#include <stdlib.h>

#include <utils/Condition.h>
#include <utils/Errors.h>
#include <utils/Mutex.h>

extern "C" {

#include "config.h"
#include "libavformat/avformat.h"
#include "libavcodec/avcodec.h"
#include "libavcodec/bsf.h"
#include "libswscale/swscale.h"
#include "libswresample/swresample.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"

}

namespace android {

//////////////////////////////////////////////////////////////////////////////////
// log
//////////////////////////////////////////////////////////////////////////////////
void nam_av_log_callback(void* ptr, int level, const char* fmt, va_list vl);
void nam_av_log_set_flags(int arg);

//////////////////////////////////////////////////////////////////////////////////
// constructor and destructor
//////////////////////////////////////////////////////////////////////////////////
status_t initFFmpeg();
void deInitFFmpeg();

//////////////////////////////////////////////////////////////////////////////////
// misc
//////////////////////////////////////////////////////////////////////////////////
bool setup_vorbis_extradata(uint8_t **extradata, int *extradata_size,
        const uint8_t *header_start[3], const int header_len[3]);

}  // namespace android

#endif  // FFMPEG_UTILS_H_
