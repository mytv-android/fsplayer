/*****************************************************************************
 * ijksdl_aout_android_audiotrack.c
 *****************************************************************************
 *
 * Copyright (c) 2013 Bilibili
 * copyright (c) 2013 Zhang Rui <bbcallen@gmail.com>
 *
 * This file is part of ijkPlayer.
 *
 * ijkPlayer is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * ijkPlayer is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with ijkPlayer; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "ijksdl_aout_android_audiotrack.h"

#include <stdbool.h>
#include <assert.h>
#include <pthread.h>
#include <jni.h>
#include "../ijksdl_inc_internal.h"
#include "../ijksdl_thread.h"
#include "../ijksdl_aout_internal.h"
#include "../ijksdl_timer.h"
#include "ijksdl_android_jni.h"
#include "android_audiotrack.h"

#ifdef SDLTRACE
#undef SDLTRACE
#define SDLTRACE(...)
//#define SDLTRACE ALOGE
#endif

static SDL_Class g_audiotrack_class = {
    .name = "AudioTrack",
};

typedef struct SDL_Aout_Opaque {
    SDL_cond *wakeup_cond;
    SDL_mutex *wakeup_mutex;

    SDL_AudioSpec spec;
    SDL_Android_AudioTrack* atrack;
    uint8_t *buffer;
    int buffer_size;

    volatile bool need_flush;
    volatile bool pause_on;
    volatile bool abort_request;
    bool loop_exited;           /* 音频线程已退出写循环、不再碰 AudioTrack 的 write；受 wakeup_mutex 保护 */
    bool created;               /* Java AudioTrack 已在音频线程上建好；受 wakeup_mutex 保护 */
    bool create_failed;
    bool open_aborted;          /* stream_close 叫停了正在等 AudioTrack 建好的 open；受 wakeup_mutex 保护 */
    /* 音频线程调回调时持有。回调读的是 ffp->is，下一台会换成新的：异步关闭前拿一次这把锁，
       确认回调已经停下、之后也不会再进 */
    SDL_mutex *cblk_mutex;

    volatile bool need_set_volume;
    volatile float left_volume;
    volatile float right_volume;

    SDL_Thread *audio_tid;
    SDL_Thread _audio_tid;

    int audio_session_id;

    volatile float speed;
    volatile bool speed_changed;
} SDL_Aout_Opaque;

static void *audiotrack_release_thread(void *arg)
{
    SDL_Android_AudioTrack *atrack = arg;
    JNIEnv *env = NULL;

    if (JNI_OK != SDL_JNI_SetupThreadEnv(&env)) {
        ALOGE("aout: release thread could not attach JNI, AudioTrack leaked\n");
        return NULL;
    }
    SDL_Android_AudioTrack_free(env, atrack);
    SDL_JNI_DetachThreadEnv();
    return NULL;
}

/*
 * 音频线程退出时释放 AudioTrack。
 *
 * AudioTrack.release() 要和 AudioFlinger 跨进程收尾，部分设备上几十毫秒；它跑在音频线程的
 * 退出路径上，而 stop() 要 join 音频线程，换台时这段全算进下一台的等待。先 pause + flush
 * 让声音立刻停（stop() 在流模式下会把缓冲放完才停），release 交给一个分离的后台线程
 * （Media3 的 DefaultAudioSink 同样把 release 放到后台线程）。线程起不来就退回同步释放。
 */
static void audiotrack_free_async(JNIEnv *env, SDL_Android_AudioTrack *atrack)
{
    pthread_attr_t attr;
    pthread_t tid;
    int started = 0;
    Uint64 t0 = SDL_GetTickHR();

    SDL_Android_AudioTrack_pause(env, atrack);
    SDL_Android_AudioTrack_flush(env, atrack);
    ALOGI("aout exit: pause+flush %d ms\n", (int)(SDL_GetTickHR() - t0));

    if (pthread_attr_init(&attr) == 0) {
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        started = pthread_create(&tid, &attr, audiotrack_release_thread, atrack) == 0;
        pthread_attr_destroy(&attr);
    }
    if (!started)
        SDL_Android_AudioTrack_free(env, atrack);
}

static int aout_thread_n(JNIEnv *env, SDL_Aout *aout)
{
    SDL_Aout_Opaque *opaque = aout->opaque;
    SDL_Android_AudioTrack *atrack = opaque->atrack;
    SDL_AudioCallback audio_cblk = opaque->spec.callback;
    void *userdata = opaque->spec.userdata;
    uint8_t *buffer = opaque->buffer;
    int copy_size = 256;

    assert(atrack);
    assert(buffer);

    SDL_SetThreadPriority(SDL_THREAD_PRIORITY_HIGH);

    /* AudioTrack 在这条线程上建（见 aout_open_audio_n）：建好或失败都要告诉在等的 open */
    bool created = SDL_Android_AudioTrack_create(env, atrack) == 0;
    int session_id = created ? SDL_Android_AudioTrack_getAudioSessionId(env, atrack) : 0;
    SDL_LockMutex(opaque->wakeup_mutex);
    if (created) {
        opaque->audio_session_id = session_id;
        opaque->created = true;
    } else {
        opaque->atrack = NULL;
        opaque->create_failed = true;
        opaque->loop_exited = true;
    }
    SDL_CondBroadcast(opaque->wakeup_cond);
    SDL_UnlockMutex(opaque->wakeup_mutex);
    if (!created) {
        ALOGE("aout_thread: failed to new AudioTrack()");
        SDL_Android_AudioTrack_free(env, atrack);
        return -1;
    }
    ALOGI("audio_session_id = %d\n", session_id);

    if (!opaque->abort_request && !opaque->pause_on)
        SDL_Android_AudioTrack_play(env, atrack);

    while (!opaque->abort_request) {
        SDL_LockMutex(opaque->wakeup_mutex);
        if (!opaque->abort_request && opaque->pause_on) {
            SDL_Android_AudioTrack_pause(env, atrack);
            while (!opaque->abort_request && opaque->pause_on) {
                SDL_CondWaitTimeout(opaque->wakeup_cond, opaque->wakeup_mutex, 1000);
            }
            if (!opaque->abort_request && !opaque->pause_on) {
                if (opaque->need_flush) {
                    opaque->need_flush = 0;
                    SDL_Android_AudioTrack_flush(env, atrack);
                }
                SDL_Android_AudioTrack_play(env, atrack);
            }
        }
        if (opaque->need_flush) {
            opaque->need_flush = 0;
            SDL_Android_AudioTrack_flush(env, atrack);
        }
        if (opaque->need_set_volume) {
            opaque->need_set_volume = 0;
            SDL_Android_AudioTrack_set_volume(env, atrack, opaque->left_volume, opaque->right_volume);
        }
        if (opaque->speed_changed) {
            opaque->speed_changed = 0;
            SDL_Android_AudioTrack_setSpeed(env, atrack, opaque->speed);
        }
        SDL_UnlockMutex(opaque->wakeup_mutex);

        SDL_LockMutex(opaque->cblk_mutex);
        if (opaque->abort_request) {
            SDL_UnlockMutex(opaque->cblk_mutex);
            break;
        }
        audio_cblk(userdata, buffer, copy_size);
        SDL_UnlockMutex(opaque->cblk_mutex);
        if (opaque->need_flush) {
            SDL_Android_AudioTrack_flush(env, atrack);
            opaque->need_flush = false;
        }

        if (opaque->need_flush) {
            opaque->need_flush = 0;
            SDL_Android_AudioTrack_flush(env, atrack);
        } else {
            /* AudioTrack.write 可能只写入一部分（缓冲将满、flush 打断等）。以前剩下的
               直接丢，音频实际缺数据、日志还每帧刷一条。这里把没写完的部分补上：
               SDL_Android_AudioTrack_write 每次都从 byte_buffer 的 0 开始拷，
               所以循环里要把剩余数据的起点作为新的 buffer 传进去。 */
            int offset = 0;
            while (offset < copy_size) {
                int written = SDL_Android_AudioTrack_write(env, atrack,
                                                           buffer + offset,
                                                           copy_size - offset);
                if (written <= 0)
                    break;
                offset += written;
            }
            if (offset != copy_size && !opaque->abort_request) {
                ALOGW("AudioTrack: dropped %d/%d bytes", copy_size - offset, copy_size);
            }
        }

        // TODO: 1 if callback return -1 or 0
    }

    /* aout_close_audio 持锁调 pause/stop 打断 write：置位之后它就不会再碰 AudioTrack，
       这里才能释放 */
    SDL_LockMutex(opaque->wakeup_mutex);
    opaque->loop_exited = true;
    SDL_CondBroadcast(opaque->wakeup_cond);
    SDL_UnlockMutex(opaque->wakeup_mutex);
    audiotrack_free_async(env, atrack);
    return 0;
}

static int aout_thread(void *arg)
{
    SDL_Aout *aout = arg;
    // SDL_Aout_Opaque *opaque = aout->opaque;
    JNIEnv *env = NULL;

    if (JNI_OK != SDL_JNI_SetupThreadEnv(&env)) {
        ALOGE("aout_thread: SDL_AndroidJni_SetupEnv: failed");
        /* aout_close_audio 在等 loop_exited，没进写循环也要置位 */
        SDL_Aout_Opaque *opaque = aout->opaque;
        SDL_LockMutex(opaque->wakeup_mutex);
        opaque->loop_exited = true;
        opaque->create_failed = true;   /* aout_open_audio_n 在等 AudioTrack 建好 */
        SDL_CondBroadcast(opaque->wakeup_cond);
        SDL_UnlockMutex(opaque->wakeup_mutex);
        return -1;
    }

    return aout_thread_n(env, aout);
}

/*
 * Java AudioTrack 改在音频线程上建，这里只算参数、起线程、等它建好。
 *
 * 建对象要和 audioserver 跨进程走一趟（真机 10~20ms）。以前在调用方（读线程补开音轨）里
 * 同步建，换台时 stop 正好碰上就得干等它建完，全算进下一台。现在 stream_close 可以用
 * aout_abort_open 叫停这次等待，读线程立刻返回；线程自己建完后看到 abort 就收尾。
 */
static int aout_open_audio_n(JNIEnv *env, SDL_Aout *aout, const SDL_AudioSpec *desired, SDL_AudioSpec *obtained)
{
    assert(desired);
    SDL_Aout_Opaque *opaque = aout->opaque;
    bool created, failed;

    /* 已经被叫停，或者上一次被叫停的 open 留下的线程还没收尾 */
    if (opaque->open_aborted || opaque->audio_tid)
        return -1;

    opaque->spec = *desired;
    opaque->atrack = SDL_Android_AudioTrack_prepare_from_sdl_spec(env, desired);
    if (!opaque->atrack) {
        ALOGE("aout_open_audio_n: failed to new AudioTrcak()");
        return -1;
    }

    opaque->buffer_size = SDL_Android_AudioTrack_get_min_buffer_size(opaque->atrack);
    if (opaque->buffer_size <= 0) {
        ALOGE("aout_open_audio_n: failed to getMinBufferSize()");
        SDL_Android_AudioTrack_free(env, opaque->atrack);
        opaque->atrack = NULL;
        return -1;
    }

    opaque->buffer = malloc(opaque->buffer_size);
    if (!opaque->buffer) {
        ALOGE("aout_open_audio_n: failed to allocate buffer");
        SDL_Android_AudioTrack_free(env, opaque->atrack);
        opaque->atrack = NULL;
        return -1;
    }

    if (obtained) {
        SDL_Android_AudioTrack_get_target_spec(opaque->atrack, obtained);
        SDLTRACE("audio target format fmt:0x%x, channel:0x%x", (int)obtained->format, (int)obtained->channels);
    }

    opaque->pause_on = 1;
    opaque->abort_request = 0;
    opaque->loop_exited = false;
    opaque->created = false;
    opaque->create_failed = false;
    opaque->audio_tid = SDL_CreateThreadEx(&opaque->_audio_tid, aout_thread, aout, "ff_aout_android");
    if (!opaque->audio_tid) {
        ALOGE("aout_open_audio_n: failed to create audio thread");
        SDL_Android_AudioTrack_free(env, opaque->atrack);
        opaque->atrack = NULL;
        return -1;
    }

    SDL_LockMutex(opaque->wakeup_mutex);
    while (!opaque->created && !opaque->create_failed && !opaque->open_aborted)
        SDL_CondWait(opaque->wakeup_cond, opaque->wakeup_mutex);
    created = opaque->created;
    failed = opaque->create_failed;
    if (!created && !failed)
        opaque->abort_request = true; /* 被叫停：线程建完后直接收尾，join 留给 close */
    SDL_UnlockMutex(opaque->wakeup_mutex);

    if (failed) {
        SDL_WaitThread(opaque->audio_tid, NULL);
        opaque->audio_tid = NULL;
        if (opaque->atrack) {
            SDL_Android_AudioTrack_free(env, opaque->atrack);
            opaque->atrack = NULL;
        }
        return -1;
    }
    if (!created) {
        ALOGI("aout_open_audio_n: aborted while the AudioTrack was being created\n");
        return -1;
    }
    return 0;
}

static int aout_open_audio(SDL_Aout *aout, const SDL_AudioSpec *desired, SDL_AudioSpec *obtained)
{
    // SDL_Aout_Opaque *opaque = aout->opaque;
    JNIEnv *env = NULL;
    if (JNI_OK != SDL_JNI_SetupThreadEnv(&env)) {
        ALOGE("aout_open_audio: AttachCurrentThread: failed");
        return -1;
    }

    return aout_open_audio_n(env, aout, desired, obtained);
}

static void aout_pause_audio(SDL_Aout *aout, int pause_on)
{
    SDL_Aout_Opaque *opaque = aout->opaque;

    SDL_LockMutex(opaque->wakeup_mutex);
    SDLTRACE("aout_pause_audio(%d)", pause_on);
    opaque->pause_on = pause_on;
    if (!pause_on)
        SDL_CondSignal(opaque->wakeup_cond);
    SDL_UnlockMutex(opaque->wakeup_mutex);
}

static void aout_flush_audio(SDL_Aout *aout)
{
    SDL_Aout_Opaque *opaque = aout->opaque;
    SDL_LockMutex(opaque->wakeup_mutex);
    SDLTRACE("aout_flush_audio()");
    opaque->need_flush = 1;
    SDL_CondSignal(opaque->wakeup_cond);
    SDL_UnlockMutex(opaque->wakeup_mutex);
}

static void aout_set_volume(SDL_Aout *aout, float left_volume, float right_volume)
{
    SDL_Aout_Opaque *opaque = aout->opaque;
    SDL_LockMutex(opaque->wakeup_mutex);
    SDLTRACE("aout_flush_audio()");
    opaque->left_volume = left_volume;
    opaque->right_volume = right_volume;
    opaque->need_set_volume = 1;
    SDL_CondSignal(opaque->wakeup_cond);
    SDL_UnlockMutex(opaque->wakeup_mutex);
}

static void aout_close_audio(SDL_Aout *aout)
{
    SDL_Aout_Opaque *opaque = aout->opaque;
    JNIEnv *env = NULL;
    if (NULL == opaque->audio_tid) {
        return;
    }
    SDL_LockMutex(opaque->wakeup_mutex);
    opaque->abort_request = true;
    SDL_CondSignal(opaque->wakeup_cond);
    /*
     * 音频线程多半正卡在阻塞的 AudioTrack.write 里：缓冲满了要等混音器取走数据，真机上
     * 一等 70~90ms，abort 要等这次 write 返回才看得见，换台全算进下一台。别的线程调 pause
     * 会打断阻塞的 write（AudioTrack.write 文档写明），暂停后的 write 也不再阻塞。
     *
     * 极少数情况下这次打断会被 AudioTrack 内部吞掉：落在 obtainBuffer 已判过 ACTIVE、还没
     * 开始等待之间，线程会一直卡在暂停的轨道上。所以等它退出写循环时带超时，超时就 stop
     * 再打断一次（stop 在 PAUSED 状态下也会 interrupt）。
     * 全程持锁（等待时 cond 会放锁）：音频线程置 loop_exited 也要拿这把锁，置位之后才释放
     * AudioTrack，pause/stop 期间对象不会被释放。
     */
    /* 对象可能还在音频线程上建（open 被叫停的情况）：没建好就不碰，线程建完会自己看到 abort。
       异步关闭（aout_free_async）先置了 abort，线程可能已经自己退出写循环、把 AudioTrack
       交给后台释放了：loop_exited 已置位就不能再碰。持锁判断，线程置位要拿同一把锁。 */
    if (opaque->created && !opaque->loop_exited && opaque->atrack &&
        JNI_OK == SDL_JNI_SetupThreadEnv(&env))
        SDL_Android_AudioTrack_pause(env, opaque->atrack);
    else
        env = NULL;
    while (!opaque->loop_exited) {
        if (SDL_CondWaitTimeout(opaque->wakeup_cond, opaque->wakeup_mutex, 50) == SDL_MUTEX_TIMEDOUT &&
            !opaque->loop_exited && env) {
            ALOGW("aout: audio thread still in write 50ms after pause, stop to interrupt again\n");
            SDL_Android_AudioTrack_stop(env, opaque->atrack);
            env = NULL; /* 只补一次；之后就照原样等线程自己退出 */
        }
    }
    SDL_UnlockMutex(opaque->wakeup_mutex);

    SDL_WaitThread(opaque->audio_tid, NULL);

    opaque->audio_tid = NULL;
}

static int aout_get_audio_session_id(SDL_Aout *aout)
{
    SDL_Aout_Opaque *opaque = aout->opaque;

    return opaque->audio_session_id;
}

static void aout_free_l(SDL_Aout *aout)
{
    if (!aout)
        return;

    aout_close_audio(aout);

    SDL_Aout_Opaque *opaque = aout->opaque;
    if (opaque) {
        free(opaque->buffer);
        opaque->buffer = NULL;
        opaque->buffer_size = 0;

        SDL_DestroyCond(opaque->wakeup_cond);
        SDL_DestroyMutex(opaque->wakeup_mutex);
        SDL_DestroyMutex(opaque->cblk_mutex);
    }

    SDL_Aout_FreeInternal(aout);
}

static void aout_abort_open(SDL_Aout *aout)
{
    SDL_Aout_Opaque *opaque = aout->opaque;

    SDL_LockMutex(opaque->wakeup_mutex);
    opaque->open_aborted = true;
    SDL_CondBroadcast(opaque->wakeup_cond);
    SDL_UnlockMutex(opaque->wakeup_mutex);
}

static void *aout_reaper_thread(void *arg)
{
    SDL_Aout *aout = arg;
    JNIEnv *env = NULL;
    Uint64 t0 = SDL_GetTickHR();
    /* aout_close_audio 里打断 write 的 pause/stop 要用 JNI */
    bool attached = JNI_OK == SDL_JNI_SetupThreadEnv(&env);

    SDL_AoutFree(aout);
    ALOGI("aout: background close done in %d ms\n", (int)(SDL_GetTickHR() - t0));
    if (attached)
        SDL_JNI_DetachThreadEnv();
    return NULL;
}

/*
 * 换台时 stream_close 用它代替同步关闭。同步关闭要打断阻塞的 write、等音频线程退出、
 * 停掉 AudioTrack，真机上每次 11~16ms，全算进下一台。
 *
 * 这里只做必须同步的一步：叫停音频线程，并确认回调已经停下（拿一次 cblk_mutex），
 * 之后它不会再读 ffp->is。其余（pause 打断 write、join、release）交给后台线程，
 * 这个 aout 对象整个归后台线程，调用方要换一个新的（ffp_prepare_async_l 见 NULL 会新建）。
 */
static void aout_free_async(SDL_Aout *aout)
{
    SDL_Aout_Opaque *opaque = aout->opaque;
    pthread_attr_t attr;
    pthread_t tid;
    int started = 0;

    SDL_LockMutex(opaque->wakeup_mutex);
    opaque->abort_request = true;
    opaque->open_aborted = true;
    SDL_CondBroadcast(opaque->wakeup_cond);
    SDL_UnlockMutex(opaque->wakeup_mutex);
    SDL_LockMutex(opaque->cblk_mutex);
    SDL_UnlockMutex(opaque->cblk_mutex);

    if (pthread_attr_init(&attr) == 0) {
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        started = pthread_create(&tid, &attr, aout_reaper_thread, aout) == 0;
        pthread_attr_destroy(&attr);
    }
    if (!started)
        SDL_AoutFree(aout);
}

static void func_set_playback_rate(SDL_Aout *aout, float speed)
{
    if (!aout)
        return;

    SDL_Aout_Opaque *opaque = aout->opaque;
    SDL_LockMutex(opaque->wakeup_mutex);
    SDLTRACE("%s %f", __func__, (double)speed);
    opaque->speed = speed;
    opaque->speed_changed = 1;
    SDL_CondSignal(opaque->wakeup_cond);
    SDL_UnlockMutex(opaque->wakeup_mutex);
}

SDL_Aout *SDL_AoutAndroid_CreateForAudioTrack()
{
    SDL_Aout *aout = SDL_Aout_CreateInternal(sizeof(SDL_Aout_Opaque));
    if (!aout)
        return NULL;

    SDL_Aout_Opaque *opaque = aout->opaque;
    opaque->wakeup_cond  = SDL_CreateCond();
    opaque->wakeup_mutex = SDL_CreateMutex();
    opaque->cblk_mutex   = SDL_CreateMutex();
    opaque->speed        = 1.0f;

    aout->opaque_class = &g_audiotrack_class;
    aout->free_l       = aout_free_l;
    aout->open_audio   = aout_open_audio;
    aout->pause_audio  = aout_pause_audio;
    aout->flush_audio  = aout_flush_audio;
    aout->set_volume   = aout_set_volume;
    aout->close_audio  = aout_close_audio;
    aout->func_get_audio_session_id = aout_get_audio_session_id;
    aout->func_set_playback_rate    = func_set_playback_rate;
    aout->func_abort_open           = aout_abort_open;
    aout->func_free_async           = aout_free_async;

    return aout;
}

bool SDL_AoutAndroid_IsObjectOfAudioTrack(SDL_Aout *aout)
{
    if (aout)
        return false;

    return aout->opaque_class == &g_audiotrack_class;
}

void SDL_Init_AoutAndroid(JNIEnv *env)
{

}
