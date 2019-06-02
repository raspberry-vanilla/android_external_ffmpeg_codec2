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

#ifndef CODEC_UTILS_H_

#define CODEC_UTILS_H_

#include <unistd.h>
#include <stdlib.h>

#include <utils/Errors.h>
#include <media/stagefright/foundation/ABuffer.h>

#include "ffmpeg_utils.h"

namespace android {

//video
status_t setAVCFormat(AVCodecContext *avctx, MetaDataBase &meta);
status_t setH264Format(AVCodecContext *avctx, MetaDataBase &meta);
status_t setMPEG4Format(AVCodecContext *avctx, MetaDataBase &meta);
status_t setH263Format(AVCodecContext *avctx, MetaDataBase &meta);
status_t setMPEG2VIDEOFormat(AVCodecContext *avctx, MetaDataBase &meta);
status_t setVC1Format(AVCodecContext *avctx, MetaDataBase &meta);
status_t setWMV1Format(AVCodecContext *avctx, MetaDataBase &meta);
status_t setWMV2Format(AVCodecContext *avctx, MetaDataBase &meta);
status_t setWMV3Format(AVCodecContext *avctx, MetaDataBase &meta);
status_t setRV20Format(AVCodecContext *avctx, MetaDataBase &meta);
status_t setRV30Format(AVCodecContext *avctx, MetaDataBase &meta);
status_t setRV40Format(AVCodecContext *avctx, MetaDataBase &meta);
status_t setFLV1Format(AVCodecContext *avctx, MetaDataBase &meta);
status_t setHEVCFormat(AVCodecContext *avctx, MetaDataBase &meta);
status_t setVP8Format(AVCodecContext *avctx, MetaDataBase &meta);
status_t setVP9Format(AVCodecContext *avctx, MetaDataBase &meta);
//audio
status_t setMP2Format(AVCodecContext *avctx, MetaDataBase &meta);
status_t setMP3Format(AVCodecContext *avctx, MetaDataBase &meta);
status_t setVORBISFormat(AVCodecContext *avctx, MetaDataBase &meta);
status_t setAC3Format(AVCodecContext *avctx, MetaDataBase &meta);
status_t setAACFormat(AVCodecContext *avctx, MetaDataBase &meta);
status_t setWMAV1Format(AVCodecContext *avctx, MetaDataBase &meta);
status_t setWMAV2Format(AVCodecContext *avctx, MetaDataBase &meta);
status_t setWMAProFormat(AVCodecContext *avctx, MetaDataBase &meta);
status_t setWMALossLessFormat(AVCodecContext *avctx, MetaDataBase &meta);
status_t setRAFormat(AVCodecContext *avctx, MetaDataBase &meta);
status_t setAPEFormat(AVCodecContext *avctx, MetaDataBase &meta);
status_t setDTSFormat(AVCodecContext *avctx, MetaDataBase &meta);
status_t setFLACFormat(AVCodecContext *avctx, MetaDataBase &meta);
status_t setALACFormat(AVCodecContext *avctx, MetaDataBase &meta);

//Convert H.264 NAL format to annex b
status_t convertNal2AnnexB(uint8_t *dst, size_t dst_size,
        uint8_t *src, size_t src_size, size_t nal_len_size);

int getDivXVersion(AVCodecContext *avctx);

status_t parseMetadataTags(AVFormatContext *ctx, MetaDataBase &meta);

AudioEncoding sampleFormatToEncoding(AVSampleFormat fmt);
AVSampleFormat encodingToSampleFormat(AudioEncoding encoding);

}  // namespace android

#endif  // CODEC_UTILS_H_
