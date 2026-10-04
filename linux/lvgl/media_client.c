#define _POSIX_C_SOURCE 200809L
#include "media_client.h"
#include "lvgl.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const char *directory;
static media_result_cb completed;
static bool pending;
static char request_id[33], request_kind[32];
static uint32_t started, timeout_ms;

void media_configure(media_result_cb callback, const char *path) { completed = callback; directory = path; }
bool media_busy(void) { return pending; }

bool media_start(const char *kind, const char *text)
{
    if (!directory || pending || (strcmp(kind, "camera") && strcmp(kind, "microphone") && strcmp(kind, "speak"))) return false;
    char path[1024], tmp[1024];
    snprintf(path, sizeof(path), "%s/host-ready.txt", directory);
    if (access(path, R_OK)) return false;
    uint32_t random[4];
    FILE *file = fopen("/dev/urandom", "rb");
    if (!file) return false;
    size_t count = fread(random, sizeof(*random), 4, file); fclose(file);
    if (count != 4) return false;
    snprintf(request_id, sizeof(request_id), "%08x%08x%08x%08x", random[0], random[1], random[2], random[3]);
    snprintf(request_kind, sizeof(request_kind), "%s", kind);
    snprintf(tmp, sizeof(tmp), "%s/request.tmp", directory);
    file = fopen(tmp, "w");
    if (!file) return false;
    bool ok = fprintf(file, "%s\n%s\n%s", request_id, kind, text ? text : "") >= 0;
    if (fclose(file)) ok = false;
    snprintf(path, sizeof(path), "%s/request.txt", directory);
    if (!ok || rename(tmp, path)) { unlink(tmp); return false; }
    pending = true; started = lv_tick_get();
    timeout_ms = !strcmp(kind, "speak") ? 130000 : 60000;
    return true;
}

void media_poll(void)
{
    if (!pending) return;
    char path[1024]; snprintf(path, sizeof(path), "%s/response.txt", directory);
    FILE *file = fopen(path, "r");
    if (file) {
        char id[64], status[32], kind[32], message[8192];
        bool valid = fgets(id, sizeof(id), file) && fgets(status, sizeof(status), file) && fgets(kind, sizeof(kind), file);
        size_t size = valid ? fread(message, 1, sizeof(message) - 1, file) : 0;
        message[size] = 0; fclose(file); unlink(path);
        if (valid) {
            id[strcspn(id, "\r\n")] = 0; kind[strcspn(kind, "\r\n")] = 0;
            status[strcspn(status, "\r\n")] = 0;
            if (!strcmp(id, request_id) && !strcmp(kind, request_kind)) {
                pending = false;
                completed(kind, !strcmp(status, "OK"), message);
                return;
            }
        }
    }
    if (lv_tick_get() - started >= timeout_ms) {
        pending = false;
        // An eventual response carries its old ID and cannot complete a retry.
        completed(request_kind, false, "Hardware request timed out. Please try again.");
    }
}
