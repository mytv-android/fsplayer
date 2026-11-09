/*
 *  ff_sub_component.h
 *
 * Copyright (c) 2022 debugly <qianlongxu@gmail.com>
 *
 * This file is part of FSPlayer.
 *
 * FSPlayer is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FSPlayer is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FSPlayer; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef ff_sub_component_h
#define ff_sub_component_h

#include <stdio.h>

typedef void (*subComponent_retry_callback)(void *opaque);
/** Text subtitle cue in media time (seconds). The text is plain UTF-8 with ASS
 *  override tags stripped; may be NULL when the packet has no renderable text. */
typedef void (*subComponent_text_callback)(void *opaque, double begin, double end, const char *text);

typedef struct FFSubComponent FFSubComponent;
typedef struct AVStream AVStream;
typedef struct AVCodecContext AVCodecContext;
typedef struct PacketQueue PacketQueue;
typedef struct FrameQueue FrameQueue;
typedef struct FSSubtitlePreference FSSubtitlePreference;
typedef struct FFSubtitleBufferPacket FFSubtitleBufferPacket;
//when hasn't ic, not support seek;
int subComponent_open(FFSubComponent **cp, int stream_index, AVStream* stream, PacketQueue* packetq, FrameQueue* frameq, const char *enc, subComponent_retry_callback callback, subComponent_text_callback text_callback, void *opaque, int vw, int vh, double startTime);
int subComponent_close(FFSubComponent **cp);
int subComponent_get_stream(FFSubComponent *com);
int subComponent_upload_buffer(FFSubComponent *com, double pts, FFSubtitleBufferPacket *buffer_array);
void subComponent_update_preference(FFSubComponent *com, FSSubtitlePreference* sp);
void subComponent_setMixPts(FFSubComponent *com,double pts);
#endif /* ff_sub_component_h */
