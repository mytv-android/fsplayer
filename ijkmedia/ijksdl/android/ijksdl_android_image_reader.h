/*****************************************************************************
 * ijksdl_android_image_reader.h
 *****************************************************************************
 *
 * Copyright (c) 2026 debugly <qianlongxu@gmail.com>
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

/*
 * AImageReader 封装：把 MediaCodec 解码输出的硬件 buffer 以零拷贝方式交给
 * 渲染器（Vulkan external memory / EGLImage）。
 *
 * AImageReader_newWithUsage / AImage_getHardwareBuffer 等符号是 API 26 才有的，
 * 而 FSPlayer minSdk 为 24，因此这里统一用 dlopen("libmediandk.so") + dlsym
 * 延迟解析；API 24/25 设备上 isAvailable() 返回 false，调用方应回退软解。
 */

#ifndef IJKSDL_ANDROID__IJKSDL_ANDROID_IMAGE_READER_H
#define IJKSDL_ANDROID__IJKSDL_ANDROID_IMAGE_READER_H

#include <stdbool.h>
#include <jni.h>

typedef struct SDL_AndroidImageReader SDL_AndroidImageReader;

/* libmediandk 是否可用且符号齐全（API 26+）。 */
bool SDL_AndroidImageReader_isAvailable(void);

/*
 * 创建 AImageReader。
 * 只承载硬件 buffer，尺寸参数无关紧要（内部固定 16x16）。
 * max_images 为同时可 acquire 的上限（建议 3）。
 */
SDL_AndroidImageReader *SDL_AndroidImageReader_create(int max_images);

/*
 * 声明这块读端 buffer 的色彩空间（ANativeWindow_setBuffersDataSpace）。
 *
 * 不设时 MediaCodec 输出到这个窗口的数据空间是未定义的，部分驱动（例如高通的
 * c2 解码器输出 4K 10bit HDR）会按默认的 8bit sRGB 布局填充，读端按 P010 采样
 * 就得到整屏绿色 + 水平条纹。bit_depth 为 8/10/12，primaries/transfer/matrix
 * 用 AVColorPrimaries / AVColorTransferCharacteristic / AVColorSpace 的取值
 * （-1 / AVCOL_*_UNSPECIFIED 表示未知，按 SDR 处理）。
 * 需要在 MediaCodec 开始输出前调用；返回 0 表示已设置。
 */
int SDL_AndroidImageReader_setBufferDataSpace(SDL_AndroidImageReader *r,
                                              int bit_depth,
                                              int primaries, int transfer, int matrix);

/*
 * 取 reader 的输出 Surface（返回 NewLocalRef，调用方负责 DeleteLocalRef）。
 * 该 Surface 交给 MediaCodec 作为输出目标。
 */
jobject SDL_AndroidImageReader_getSurface(JNIEnv *env, SDL_AndroidImageReader *r);

/*
 * 等待并取回最新一帧的 AHardwareBuffer。
 * 成功返回 0，*out_hwbuf 为 AHardwareBuffer*，尺寸写入 *out_w / *out_h；
 * 返回的 buffer 由本对象持有，直到下一次 acquire 或 releaseImage。
 * timeout_ms 为等待首帧的最长时间。
 */
int SDL_AndroidImageReader_acquireLatest(SDL_AndroidImageReader *r, void **out_hwbuf,
                                         int *out_w, int *out_h, int timeout_ms);

/* 释放最近一次 acquire 到的 image。 */
void SDL_AndroidImageReader_releaseImage(SDL_AndroidImageReader *r);

void SDL_AndroidImageReader_destroy(SDL_AndroidImageReader *r);

/*
 * 删掉 FFmpeg mediacodec 换台预热时停在 native_window 上的硬解实例（NULL 表示全部）。
 * 硬解输出目标销毁前调用，否则那个预热实例一直占着一个硬解实例。
 */
void SDL_AndroidMediaCodec_ClearWarm(void *native_window);

#endif
