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

#include "link_probe.h"

#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include "link_events.h"
#include "stack_monitor.h"
#include "wifi_mgr.h"

static const char *TAG = "link.probe";

#define PROBE_TCP_TIMEOUT_MS 3000
#define PROBE_STACK          4096

// Public anycast resolvers, contacted by IP so the check needs no DNS. Port 443
// because some networks block other outbound ports. The second is tried only if
// the first fails, so one provider's outage doesn't read as ours.
static const char *const PUBLIC_HOSTS[] = {"1.1.1.1", "8.8.8.8"};
#define PUBLIC_PORT 443

static atomic_bool s_running = ATOMIC_VAR_INIT(false);

// "ok", "refused" (still reached the host), "timeout" or "error".
static const char *connect_public(const char *ip) {
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(PUBLIC_PORT),
    };
    inet_pton(AF_INET, ip, &addr.sin_addr);
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) return "error";
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    int err = 0;
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        err = errno;
        if (err == EINPROGRESS) {
            fd_set writable;
            FD_ZERO(&writable);
            FD_SET(fd, &writable);
            struct timeval tv = {.tv_sec = PROBE_TCP_TIMEOUT_MS / 1000};
            int ready = select(fd + 1, NULL, &writable, NULL, &tv);
            socklen_t len = sizeof(err);
            if (ready == 0) err = ETIMEDOUT;
            else if (ready < 0 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0) err = EIO;
        }
    }
    close(fd);
    return err == 0 ? "ok"
         : err == ECONNREFUSED ? "refused"
         : err == ETIMEDOUT ? "timeout"
         : "error";
}

static bool reached(const char *result) {
    return result[0] == 'o' || result[0] == 'r';  // "ok" or "refused"
}

static void probe_task(void *arg) {
    (void)arg;
    char line[LINK_EVENTS_DETAIL_MAX];
    if (!wifi_mgr_is_connected()) {
        snprintf(line, sizeof(line), "wifi down");
    } else {
        const char *first = connect_public(PUBLIC_HOSTS[0]);
        const char *second = reached(first) ? NULL : connect_public(PUBLIC_HOSTS[1]);
        if (!second || reached(second)) {
            snprintf(line, sizeof(line), "internet ok");
        } else {
            snprintf(line, sizeof(line), "internet unreachable (%s %s, %s %s)",
                     PUBLIC_HOSTS[0], first, PUBLIC_HOSTS[1], second);
        }
    }
    ESP_LOGW(TAG, "stall probe: %s", line);
    link_events_record(LINK_EVENT_STALL_PROBE, "%s", line);

    stack_monitor_record(NULL);
    atomic_store(&s_running, false);
    vTaskDelete(NULL);
}

void link_probe_on_stall(void) {
    bool idle = false;
    if (!atomic_compare_exchange_strong(&s_running, &idle, true)) return;
    if (xTaskCreate(probe_task, "link_probe", PROBE_STACK, NULL, 2, NULL) != pdPASS) {
        ESP_LOGW(TAG, "stall probe task unavailable");
        atomic_store(&s_running, false);
    }
}
