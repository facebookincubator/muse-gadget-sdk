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

#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define pdTRUE 1
#define portMAX_DELAY 0
#define pdMS_TO_TICKS(ms) (ms)
#define SEND_MS 3000
#define LOG_WAIT_MS 50
#define LOG_LINE 512
static bool stalled, log_lock_busy;
static int failed_writes;
static pthread_mutex_t tx = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t *s_serial_tx = &tx;
static int xSemaphoreTake(pthread_mutex_t *m, int ms)
{
    if (ms == LOG_WAIT_MS && log_lock_busy) return 0;
    assert(!pthread_mutex_lock(m));
    return pdTRUE;
}
static void xSemaphoreGive(pthread_mutex_t *m) { assert(!pthread_mutex_unlock(m)); }
static int64_t esp_timer_get_time(void)
{
    static int64_t fake_time;
    if (stalled) return fake_time += 1000;
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}
static int muse_console_write_bytes(const void *p, size_t n, int ms)
{
    assert(ms >= 0 && ms <= 200);
    assert(n <= 128);
    if (stalled) { failed_writes++; return false; }
    if (n > 17) n = 17;
    return (int)fwrite(p, 1, n, stdout);
}
static uint32_t esp_rom_crc32_le(uint32_t crc, const uint8_t *p, size_t n)
{
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        for (int i = 0; i < 8; i++) crc = (crc >> 1) ^ (0xedb88320u & -(crc & 1u));
    }
    return ~crc;
}
#include "serial_impl.inc"
static void log_line(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    assert(serial_log(fmt, ap) > 0);
    va_end(ap);
}
static void dropped_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    assert(serial_log(fmt, ap) == 0);
    va_end(ap);
}
static void *logs(void *unused)
{
    (void)unused;
    for (int i = 0; i < 100; i++) log_line("log %d %0600d\n", i, i);
    return NULL;
}
int main(void)
{
    assert(esp_rom_crc32_le(0, (const uint8_t *)"123456789", 9) == 0xcbf43926);
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, logs, NULL));
    uint8_t msg[3000];
    for (size_t i = 0; i < sizeof(msg); i++) msg[i] = i;
    for (int i = 0; i < 100; i++) assert(serial_send(msg, sizeof(msg), SEND_MS));
    assert(!pthread_join(thread, NULL));
    stalled = true;
    assert(!serial_send(msg, sizeof(msg), 5));
    assert(failed_writes > 0);
    assert(!pthread_mutex_trylock(&tx));
    pthread_mutex_unlock(&tx);
    log_lock_busy = true;
    dropped_log("driver task log\n");
    assert(atomic_load(&s_log_dropped) == 1);
    return 0;
}
