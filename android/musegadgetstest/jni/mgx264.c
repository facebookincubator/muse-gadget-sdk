/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * JNI wrapper around libx264 for Muse Gadgets Test's software encoder
 * (com.musegadgets.test.X264): one I420 picture in, one Annex-B access
 * unit out, no delay (zerolatency: no lookahead, no B frames, no frame
 * threads). Built by jni/build.sh against a static libx264.
 */
#include <jni.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <android/log.h>
#include <x264.h>

#define TAG "MuseCam"

typedef struct {
    x264_t *enc;
    x264_param_t p;
    int w, h;
} mgx264_t;

static void logcb(void *priv, int level, const char *fmt, va_list ap)
{
    (void)priv;
    if (level <= X264_LOG_WARNING) {
        __android_log_vprint(ANDROID_LOG_WARN, TAG, fmt, ap);
    }
}

static void set_rate(x264_param_t *p, int kbps, int vbv_ms)
{
    p->rc.i_rc_method = X264_RC_ABR;
    p->rc.i_bitrate = kbps;
    p->rc.i_vbv_max_bitrate = kbps;
    p->rc.i_vbv_buffer_size = kbps * vbv_ms / 1000 > 1 ? kbps * vbv_ms / 1000 : 1;
}

JNIEXPORT jlong JNICALL Java_com_musegadgets_test_X264_open(JNIEnv *env, jclass cls, jint w, jint h, jint fps,
                                                                 jint kbps, jstring jpreset, jstring jparams)
{
    (void)cls;
    mgx264_t *m = calloc(1, sizeof(*m));
    const char *preset = (*env)->GetStringUTFChars(env, jpreset, NULL);
    int rc = x264_param_default_preset(&m->p, preset, "zerolatency,psnr");
    (*env)->ReleaseStringUTFChars(env, jpreset, preset);
    if (rc < 0) {
        free(m);
        return 0;
    }
    x264_param_t *p = &m->p;
    p->i_width = m->w = w;
    p->i_height = m->h = h;
    p->i_csp = X264_CSP_I420;
    p->i_fps_num = fps;
    p->i_fps_den = 1;
    p->i_timebase_num = 1;
    p->i_timebase_den = fps;
    p->b_vfr_input = 0;
    p->i_threads = 1;
    p->b_sliced_threads = 0;
    p->i_bframe = 0;
    p->i_frame_reference = 1;
    p->i_keyint_max = 3 * fps;   /* the intra refresh period */
    p->b_intra_refresh = 1;
    p->b_annexb = 1;
    p->b_repeat_headers = 1;
    p->rc.i_lookahead = 0;
    p->rc.b_mb_tree = 0;
    p->i_log_level = X264_LOG_WARNING;
    p->pf_log = logcb;
    set_rate(p, kbps, 200);
    if (jparams) {
        /* "name=value:name=value", x264's own option names (as on its command line) */
        const char *s = (*env)->GetStringUTFChars(env, jparams, NULL);
        char *buf = strdup(s), *save = NULL;
        (*env)->ReleaseStringUTFChars(env, jparams, s);
        for (char *kv = strtok_r(buf, ":", &save); kv; kv = strtok_r(NULL, ":", &save)) {
            char *eq = strchr(kv, '=');
            if (eq) {
                *eq = 0;
            }
            if (!strcmp(kv, "vbv-ms")) {
                set_rate(p, kbps, eq ? atoi(eq + 1) : 200);
            } else if (x264_param_parse(p, kv, eq ? eq + 1 : NULL) < 0) {
                __android_log_print(ANDROID_LOG_WARN, TAG, "x264: bad option %s", kv);
            }
        }
        free(buf);
    }
    x264_param_apply_profile(p, "baseline");
    m->enc = x264_encoder_open(p);
    if (!m->enc) {
        free(m);
        return 0;
    }
    return (jlong)(intptr_t)m;
}

JNIEXPORT jbyteArray JNICALL Java_com_musegadgets_test_X264_encode(JNIEnv *env, jclass cls, jlong h,
                                                                        jbyteArray i420, jlong pts, jboolean idr,
                                                                        jintArray info)
{
    (void)cls;
    mgx264_t *m = (mgx264_t *)(intptr_t)h;
    if (!m) {
        return NULL;
    }
    x264_picture_t in, out;
    x264_nal_t *nal;
    int nn = 0;
    if (!i420) {
        /* flush: the frames the lookahead still holds, one per call */
        if (x264_encoder_delayed_frames(m->enc) <= 0) {
            return NULL;
        }
        int bytes = x264_encoder_encode(m->enc, &nal, &nn, NULL, &out);
        if (bytes <= 0) {
            return NULL;
        }
        jbyteArray r = (*env)->NewByteArray(env, bytes);
        (*env)->SetByteArrayRegion(env, r, 0, bytes, (const jbyte *)nal[0].p_payload);
        if (info) {
            jint v[2] = { out.i_type == X264_TYPE_IDR, out.i_qpplus1 - 1 };
            (*env)->SetIntArrayRegion(env, info, 0, 2, v);
        }
        return r;
    }
    x264_picture_init(&in);
    in.img.i_csp = X264_CSP_I420;
    in.img.i_plane = 3;
    in.i_pts = pts;
    in.i_type = idr ? X264_TYPE_IDR : X264_TYPE_AUTO;
    jbyte *y = (*env)->GetPrimitiveArrayCritical(env, i420, NULL);
    in.img.plane[0] = (uint8_t *)y;
    in.img.plane[1] = (uint8_t *)y + m->w * m->h;
    in.img.plane[2] = (uint8_t *)y + m->w * m->h * 5 / 4;
    in.img.i_stride[0] = m->w;
    in.img.i_stride[1] = in.img.i_stride[2] = m->w / 2;
    int bytes = x264_encoder_encode(m->enc, &nal, &nn, &in, &out);
    (*env)->ReleasePrimitiveArrayCritical(env, i420, y, JNI_ABORT);
    if (bytes <= 0) {
        return NULL;
    }
    /* the payloads are contiguous in x264's buffer */
    jbyteArray r = (*env)->NewByteArray(env, bytes);
    (*env)->SetByteArrayRegion(env, r, 0, bytes, (const jbyte *)nal[0].p_payload);
    if (info) {
        jint v[2] = { out.i_type == X264_TYPE_IDR, out.i_qpplus1 - 1 };
        (*env)->SetIntArrayRegion(env, info, 0, 2, v);
    }
    return r;
}

JNIEXPORT void JNICALL Java_com_musegadgets_test_X264_setBitrate(JNIEnv *env, jclass cls, jlong h, jint kbps)
{
    (void)env;
    (void)cls;
    mgx264_t *m = (mgx264_t *)(intptr_t)h;
    if (!m) {
        return;
    }
    int ms = m->p.rc.i_bitrate > 0 ? m->p.rc.i_vbv_buffer_size * 1000 / m->p.rc.i_bitrate : 200;
    set_rate(&m->p, kbps, ms);
    x264_encoder_reconfig(m->enc, &m->p);
}

JNIEXPORT void JNICALL Java_com_musegadgets_test_X264_close(JNIEnv *env, jclass cls, jlong h)
{
    (void)env;
    (void)cls;
    mgx264_t *m = (mgx264_t *)(intptr_t)h;
    if (m) {
        x264_encoder_close(m->enc);
        free(m);
    }
}

/*
 * PanelScaler.scale in C (the same integer arithmetic, bit-identical), for
 * builds that carry this library: about 10x faster than the Java loops.
 * taps: nOut x len Q12 weights from start[o]; tmp: nIn x nOut ints.
 */
JNIEXPORT void JNICALL Java_com_musegadgets_test_X264_scale(JNIEnv *env, jclass cls, jbyteArray jsrc, jint rowStride,
                                                                 jint pixStride, jint x0, jint y0, jint nIn, jint nOut,
                                                                 jintArray jstart, jintArray jtaps, jint len,
                                                                 jbyteArray jdst)
{
    (void)cls;
    /* Every index stays inside the arrays it reads and writes: the filter's
     * taps (start[o] + len <= nIn), the source rows and columns, the output. */
    if (nIn <= 0 || nOut <= 0 || len <= 0 || len > nIn || nIn > 4096 || nOut > 4096 || x0 < 0 || y0 < 0 ||
        pixStride < 1 || rowStride < 1 || (*env)->GetArrayLength(env, jstart) < nOut ||
        (*env)->GetArrayLength(env, jtaps) < nOut * len || (*env)->GetArrayLength(env, jdst) < nOut * nOut ||
        (int64_t)(y0 + nIn - 1) * rowStride + (int64_t)(x0 + nIn - 1) * pixStride >= (*env)->GetArrayLength(env, jsrc)) {
        return;
    }
    int *t = malloc(sizeof(int) * nIn * nOut), *acc = malloc(sizeof(int) * nOut);
    jint *start = (*env)->GetIntArrayElements(env, jstart, NULL), *w = (*env)->GetIntArrayElements(env, jtaps, NULL);
    bool ok = t && acc && start && w;
    for (int o = 0; ok && o < nOut; o++) {
        ok = start[o] >= 0 && start[o] + len <= nIn;
    }
    if (!ok) {
        if (start) {
            (*env)->ReleaseIntArrayElements(env, jstart, start, JNI_ABORT);
        }
        if (w) {
            (*env)->ReleaseIntArrayElements(env, jtaps, w, JNI_ABORT);
        }
        free(t);
        free(acc);
        return;
    }
    uint8_t *src = (*env)->GetPrimitiveArrayCritical(env, jsrc, NULL);
    for (int y = 0; y < nIn; y++) {
        const uint8_t *row = src + (y0 + y) * rowStride + x0 * pixStride;
        int *to = t + y * nOut;
        for (int o = 0; o < nOut; o++) {
            const uint8_t *s = row + start[o] * pixStride;
            const jint *wo = w + o * len;
            int v = 0;
            for (int k = 0; k < len; k++) {
                v += s[k * pixStride] * wo[k];
            }
            to[o] = (v + 32) >> 6;
        }
    }
    (*env)->ReleasePrimitiveArrayCritical(env, jsrc, src, JNI_ABORT);
    uint8_t *dst = (*env)->GetPrimitiveArrayCritical(env, jdst, NULL);
    for (int o = 0; o < nOut; o++) {
        const int *r0 = t + start[o] * nOut;
        const jint *wo = w + o * len;
        for (int x = 0; x < nOut; x++) {
            acc[x] = r0[x] * wo[0];
        }
        for (int k = 1; k < len; k++) {
            const int *r = r0 + k * nOut;
            int c = wo[k];
            for (int x = 0; x < nOut; x++) {
                acc[x] += r[x] * c;
            }
        }
        uint8_t *d = dst + o * nOut;
        for (int x = 0; x < nOut; x++) {
            int v = (acc[x] + (1 << 17)) >> 18;
            d[x] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
        }
    }
    (*env)->ReleasePrimitiveArrayCritical(env, jdst, dst, 0);
    (*env)->ReleaseIntArrayElements(env, jstart, start, JNI_ABORT);
    (*env)->ReleaseIntArrayElements(env, jtaps, w, JNI_ABORT);
    free(t);
    free(acc);
}
