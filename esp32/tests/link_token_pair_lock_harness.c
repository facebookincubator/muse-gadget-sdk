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
 * The token pair's writers and readers on a gadget: app.c's
 * pair lock code (extracted by test_link_gadget_setup.py into
 * token_pair_lock.inc) on the real main/config_store.c, over a fake NVS that
 * writes each key at once and logs who wrote what, with real threads:
 *  - forced interleavings: one writer is paused in the middle of its pair
 *    (between refresh_token and access_token) until the other writer and a
 *    reader are both blocked on the lock (or the writer has written: a
 *    missing lock); the other's first write must come after the first's
 *    last, and the reader sees a whole pair;
 *  - the refresh refused while a re-setup holds the session's tokens.
 */

#include <pthread.h>
#include <stdatomic.h>
#include <sys/time.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config_store.h"
#include "nvs.h"

#define CHECK(cond, ...) do {                                                \
    if (!(cond)) {                                                           \
        fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);               \
        fprintf(stderr, __VA_ARGS__);                                        \
        fputc('\n', stderr);                                                 \
        exit(1);                                                             \
    }                                                                        \
} while (0)

/* ---- FreeRTOS and logging, as app.c uses them ---------------------------- */

typedef pthread_mutex_t *SemaphoreHandle_t;
#define portMAX_DELAY 0
static __thread char t_who;   /* this thread's tag in the log */
/* Set once the second writer ('B') or the reader ('C') finds the lock held
 * and waits for it. TOKEN_PAIR_NO_LOCK builds the negative control: no lock
 * at all, which the interleavings must catch. */
static atomic_int s_blocked_b, s_blocked_c;
static int take_lock(pthread_mutex_t *m) {
#ifdef TOKEN_PAIR_NO_LOCK
    (void)m;
#else
    if (pthread_mutex_trylock(m) != 0) {
        if (t_who == 'B') s_blocked_b = 1;
        if (t_who == 'C') s_blocked_c = 1;
        pthread_mutex_lock(m);
    }
#endif
    return 1;
}
static int give_lock(pthread_mutex_t *m) {
#ifdef TOKEN_PAIR_NO_LOCK
    (void)m;
#else
    pthread_mutex_unlock(m);
#endif
    return 1;
}
#define xSemaphoreTake(m, t) take_lock(m)
#define xSemaphoreGive(m) give_lock(m)
#define TAG "app"
#define TOKEN_BUF_BYTES 2048   /* app.c's */
#define ESP_LOGW(tag, ...) ((void)0)
#define CONFIG_MUSE_GADGET_BLE_AUDIO 1
/* app.c calls it under its auth lock; the harness writers hold none. */
static atomic_int s_fresh;
static void note_access_token_fresh(void) { atomic_fetch_add(&s_fresh, 1); }

/* ---- a fake NVS: one key at a time, with a log of who wrote ------------- */

#define MAX_ENTRIES 64
typedef struct {
    bool used;
    char key[16];
    char value[96];
} entry_t;
static entry_t s_nvs[MAX_ENTRIES];
static pthread_mutex_t s_nvs_guard = PTHREAD_MUTEX_INITIALIZER;   /* only so a broken lock can't corrupt the fake */

static char s_log[4096];
static int s_log_n;
/* Pause hook: the thread tagged s_pause_who stops after writing refresh_token
 * until s_resume is set (the other writer meanwhile tries to start). */
static _Atomic char s_pause_who;
static atomic_int s_paused, s_resume;

static entry_t *find(const char *key) {
    for (int i = 0; i < MAX_ENTRIES; i++) {
        if (s_nvs[i].used && !strcmp(s_nvs[i].key, key)) return &s_nvs[i];
    }
    return NULL;
}

esp_err_t nvs_flash_init(void) { return ESP_OK; }
esp_err_t nvs_flash_init_partition(const char *p) { (void)p; return ESP_OK; }
esp_err_t nvs_open(const char *ns, nvs_open_mode_t m, nvs_handle_t *h) { (void)ns; (void)m; *h = 1; return ESP_OK; }
void nvs_close(nvs_handle_t h) { (void)h; }
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }
const char *esp_err_to_name(esp_err_t e) { return e ? "ESP_FAIL" : "ESP_OK"; }

esp_err_t nvs_get_str(nvs_handle_t h, const char *key, char *out, size_t *len) {
    (void)h;
    pthread_mutex_lock(&s_nvs_guard);
    entry_t *e = find(key);
    esp_err_t r = ESP_ERR_NVS_NOT_FOUND;
    if (e) {
        size_t need = strlen(e->value) + 1;
        if (!out) {
            *len = need;
            r = ESP_OK;
        } else if (*len < need) {
            *len = need;
            r = ESP_ERR_NVS_INVALID_LENGTH;
        } else {
            memcpy(out, e->value, need);
            *len = need;
            r = ESP_OK;
        }
    }
    pthread_mutex_unlock(&s_nvs_guard);
    return r;
}

esp_err_t nvs_set_str(nvs_handle_t h, const char *key, const char *value) {
    (void)h;
    pthread_mutex_lock(&s_nvs_guard);
    entry_t *e = find(key);
    for (int i = 0; !e && i < MAX_ENTRIES; i++) {
        if (!s_nvs[i].used) {
            e = &s_nvs[i];
            e->used = true;
            snprintf(e->key, sizeof(e->key), "%s", key);
        }
    }
    snprintf(e->value, sizeof(e->value), "%s", value);
    if (t_who && s_log_n < (int)sizeof(s_log) - 1) s_log[s_log_n++] = t_who;
    pthread_mutex_unlock(&s_nvs_guard);
    if (t_who && t_who == s_pause_who && !strcmp(key, "refresh_token")) {
        s_paused = 1;
        while (!s_resume) usleep(1000);
    } else {
        usleep(50);   /* widen every window */
    }
    return ESP_OK;
}

esp_err_t nvs_erase_key(nvs_handle_t h, const char *key) {
    (void)h;
    pthread_mutex_lock(&s_nvs_guard);
    entry_t *e = find(key);
    if (e) memset(e, 0, sizeof(*e));
    if (e && t_who && s_log_n < (int)sizeof(s_log) - 1) s_log[s_log_n++] = t_who;
    pthread_mutex_unlock(&s_nvs_guard);
    return e ? ESP_OK : ESP_ERR_NVS_NOT_FOUND;
}

/* ---- the wall clock, as access_expires_from() reads it ----------------------- */

static long s_clock_s;   /* 0: never set since power-on (the epoch), as on a gadget no phone set */
static int fake_gettimeofday(struct timeval *tv, void *tz) {
    (void)tz;
    tv->tv_sec = s_clock_s;
    tv->tv_usec = 0;
    return 0;
}
#define gettimeofday fake_gettimeofday

/* ---- app.c's code under test ---------------------------------------------- */

#include "token_pair_lock.inc"

static pthread_mutex_t s_lock_storage = PTHREAD_MUTEX_INITIALIZER;

static void put_pair(const char *a, const char *r) {
    CHECK(config_set_str("access_token", a) && config_set_str("refresh_token", r), "seed");
}

/* access "a-X", refresh "r-X": the same X, or a torn read. */
static bool same_pair(const char *a, const char *r) {
    return a[0] == 'a' && r[0] == 'r' && a[1] == '-' && r[1] == '-' && !strcmp(a + 2, r + 2);
}

/* ---- forced interleavings ------------------------------------------------- */

typedef enum { W_SESSION, W_REFRESH, W_SETUP } writer_t;

static bool write_pair(writer_t w, const char *tag) {
    char a[48], r[48];
    snprintf(a, sizeof(a), "a-%s", tag);
    snprintf(r, sizeof(r), "r-%s", tag);
    switch (w) {
    case W_SESSION: return app_gadget_store_session_tokens(a, r, 0);
    case W_REFRESH: {
        /* A refresh answers for the pair it read: one superseded meanwhile is dropped, never torn. */
        char ua[48] = "", ur[48] = "";
        app_gadget_read_token_pair(ua, sizeof(ua), ur, sizeof(ur), NULL);
        return store_refreshed_pair_unlocked(a, r, 0, ur);
    }
    case W_SETUP: return store_token_pair_unlocked(a, r);
    }
    return false;
}

typedef struct {
    writer_t w;
    char who;
    const char *tag;
    bool ok;
} job_t;

static atomic_int s_b_done;

static void *job(void *arg) {
    job_t *j = arg;
    t_who = j->who;
    j->ok = write_pair(j->w, j->tag);
    if (j->who == 'B') s_b_done = 1;
    return NULL;
}

static atomic_int s_read_done;
static bool s_read_whole;

static void *read_job(void *arg) {
    (void)arg;
    t_who = 'C';
    char acc[64], ref[64];
    s_read_whole = app_gadget_read_token_pair(acc, sizeof(acc), ref, sizeof(ref), NULL) && same_pair(acc, ref);
    s_read_done = 1;
    return NULL;
}

static bool logged(char who) {
    pthread_mutex_lock(&s_nvs_guard);
    bool r = memchr(s_log, who, (size_t)s_log_n) != NULL;
    pthread_mutex_unlock(&s_nvs_guard);
    return r;
}

static void interleave(writer_t first, writer_t second, const char *name) {
    memset(s_nvs, 0, sizeof(s_nvs));
    put_pair("a-old", "r-old");
    s_log_n = 0;
    s_paused = s_resume = 0;
    s_blocked_b = s_blocked_c = s_read_done = s_b_done = 0;
    s_pause_who = 'A';
    job_t a = {first, 'A', "A1", false}, b = {second, 'B', "B1", false};
    pthread_t ta, tb, tc;
    pthread_create(&ta, NULL, job, &a);
    while (!s_paused) usleep(1000);   /* A is in the middle of its pair */
    pthread_create(&tb, NULL, job, &b);
    while (!s_blocked_b && !logged('B') && !s_b_done) usleep(1000);   /* B waits on the lock, or got past it */
    pthread_create(&tc, NULL, read_job, NULL);
    while (!s_blocked_c && !s_read_done) usleep(1000);   /* so does a reader */
    s_resume = 1;
    pthread_join(ta, NULL);
    pthread_join(tb, NULL);
    pthread_join(tc, NULL);
    s_log[s_log_n] = '\0';
    CHECK(a.ok && b.ok, "%s: both stored (%d %d)", name, a.ok, b.ok);
    CHECK(s_read_whole, "%s: a read during the pair saw half of it", name);
    /* A's writes, then B's: never one inside the other. */
    const char *firstb = strchr(s_log, 'B');
    CHECK(firstb && !strchr(firstb, 'A'), "%s: interleaved writes: %s", name, s_log);
    char acc[64], ref[64];
    CHECK(app_gadget_read_token_pair(acc, sizeof(acc), ref, sizeof(ref), NULL) && !strcmp(acc, "a-B1") && !strcmp(ref, "r-B1"),
          "%s: the last pair stands whole (%s %s)", name, acc, ref);
}

int main(void) {
    s_session_tokens_lock = &s_lock_storage;

    /* The device session's update while the gadget's own refresh is half
     * done, the other way round, and both against setup's store. */
    interleave(W_REFRESH, W_SESSION, "refresh, then a session update");
    interleave(W_SESSION, W_REFRESH, "a session update, then a refresh");
    interleave(W_SETUP, W_SESSION, "setup's store, then a session update");
    interleave(W_SESSION, W_SETUP, "a session update, then setup's store");

    char acc[64], ref[64];
    uint64_t exp_at = 0;
    s_pause_who = 0;

    /* A re-setup holds the session's tokens: the gadget's own refresh (the
     * old setup's pair) and the session's update are refused, the pair as
     * it was; once it's over both store again. */
    memset(s_nvs, 0, sizeof(s_nvs));
    put_pair("a-old", "r-old");
    put_pair("a-old", "r-old");
    CHECK(config_set_str("setup_complete", "1"), "seed");
    CHECK(reprovision_begin_holding_session_tokens(), "begin");
    CHECK(app_gadget_session_busy(), "busy while held");
    CHECK(!write_pair(W_REFRESH, "X") && !write_pair(W_SESSION, "Y"), "refused while held");
    CHECK(config_reprovision_rollback(), "rollback");
    session_tokens_release();
    CHECK(app_gadget_read_token_pair(acc, sizeof(acc), ref, sizeof(ref), NULL) && !strcmp(acc, "a-old"), "unchanged");
    CHECK(!app_gadget_session_busy() && write_pair(W_REFRESH, "Z"), "stored after");
    CHECK(app_gadget_read_token_pair(acc, sizeof(acc), ref, sizeof(ref), NULL) && !strcmp(acc, "a-Z") && !strcmp(ref, "r-Z"),
          "the refreshed pair");
    /* The gadget's own refresh keeps access_expires_at = now + expires_in when
     * the wall clock is set, none when it isn't or there's no expires_in,
     * with the pair in the same journalled commit. */
    s_clock_s = 0;
    CHECK(access_expires_from(14400, 0) == 0, "an unset clock: none");
    s_clock_s = 1704067199;
    CHECK(access_expires_from(14400, 0) == 0, "a clock before 2024: never set, none");
    s_clock_s = 1793870000;
    CHECK(access_expires_from(14400, 0) == 1793884400ull, "set: now + expires_in");
    CHECK(access_expires_from(0, 0) == 0 && access_expires_from(-1, 0) == 0, "no expires_in: none");
    /* The clock unset: the response's Date instead; the clock set wins over it. */
    s_clock_s = 0;
    CHECK(access_expires_from(14400, 1793860000) == 1793874400ull, "unset clock: Date + expires_in");
    CHECK(access_expires_from(14400, 1704067199) == 0, "a Date before 2024: none");
    CHECK(access_expires_from(0, 1793860000) == 0, "a Date, no expires_in: none");
    s_clock_s = 1793870000;
    CHECK(access_expires_from(14400, 1793000000) == 1793884400ull, "set clock wins over Date");
    memset(s_nvs, 0, sizeof(s_nvs));
    put_pair("a-old", "r-old");
    CHECK(config_set_str("tok_exp", "1700000000"), "seed");
    CHECK(store_refreshed_pair_unlocked("a-new", "r-new", access_expires_from(14400, 0), "r-old"), "refresh stored");
    CHECK(app_gadget_read_token_pair(acc, sizeof(acc), ref, sizeof(ref), &exp_at) && !strcmp(acc, "a-new")
          && !strcmp(ref, "r-new") && exp_at == 1793884400ull, "the pair and its expiry together");
    s_clock_s = 0;
    CHECK(store_refreshed_pair_unlocked("a-next", "r-next", access_expires_from(14400, 0), "r-new"), "refresh, clock unset");
    CHECK(app_gadget_read_token_pair(acc, sizeof(acc), ref, sizeof(ref), &exp_at) && !strcmp(acc, "a-next")
          && exp_at == 0 && !config_get_str("tok_exp", acc, sizeof(acc)), "none: the old expiry gone with the old pair");
    /* Refused while a re-setup holds the tokens: the pair and its expiry as they were. */
    s_clock_s = 1793870000;
    CHECK(store_refreshed_pair_unlocked("a-z", "r-z", access_expires_from(14400, 0), "r-next"), "again");
    CHECK(reprovision_begin_holding_session_tokens(), "begin");
    CHECK(!store_refreshed_pair_unlocked("a-y", "r-y", access_expires_from(100, 0), "r-z"), "refused while held");
    CHECK(config_reprovision_rollback(), "rollback");
    session_tokens_release();
    CHECK(app_gadget_read_token_pair(acc, sizeof(acc), ref, sizeof(ref), &exp_at) && !strcmp(acc, "a-z")
          && exp_at == 1793884400ull, "kept");
    /* The device session saved a newer pair while a refresh of the older one was out: the late answer
     * (here a new pair) is dropped and the newer pair stays. */
    CHECK(app_gadget_store_session_tokens("a-phone", "r-phone", 0), "the phone's newer pair");
    CHECK(store_refreshed_pair_unlocked("a-late", "r-late", access_expires_from(14400, 0), "r-z"), "late answer");
    CHECK(app_gadget_read_token_pair(acc, sizeof(acc), ref, sizeof(ref), &exp_at) && !strcmp(acc, "a-phone")
          && !strcmp(ref, "r-phone"), "the newer pair stays");
    CHECK(refresh_token_unchanged_locked("r-phone") && !refresh_token_unchanged_locked("r-z")
          && !refresh_token_unchanged_locked(NULL), "the comparison");
    printf("PASS token pair lock\n");
    return 0;
}
