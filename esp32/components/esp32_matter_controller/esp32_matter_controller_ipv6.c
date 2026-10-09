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

// Wi-Fi-side IPv6 for Thread devices behind the home's border router. They
// answer from the border router's on-link prefix (SLAAC) and are reached
// through the route it advertises (CHIP's route hook). lwIP solicits a router
// advertisement when the link comes up and stops at the first answer, which
// can come before the route hook listens; the border router's next unsolicited
// one may be minutes away. So solicit again 1 and 5 seconds after Matter
// starts and after each IPv6 address event.
#include "esp32_matter_controller_ipv6.h"

#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "esp_timer.h"
#include "lwip/netif.h"
#include "lwip/tcpip.h"

static esp_timer_handle_t s_solicit_timer;
static int s_solicits_left;

static esp_netif_t *sta_netif(void) {
    return esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
}

static void solicit_in_tcpip(void *arg) {
    struct netif *netif = arg;
    if (netif) netif->rs_count = LWIP_ND6_MAX_MULTICAST_SOLICIT;
}

static void solicit_now(void *arg) {
    (void)arg;
    esp_netif_t *sta = sta_netif();
    if (sta) tcpip_callback(solicit_in_tcpip, esp_netif_get_netif_impl(sta));
    if (--s_solicits_left > 0) esp_timer_start_once(s_solicit_timer, 4 * 1000 * 1000);
}

static void solicit_later(void) {
    esp_timer_stop(s_solicit_timer);
    s_solicits_left = 2;
    esp_timer_start_once(s_solicit_timer, 1000 * 1000);
}

static void on_got_ip6(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    (void)base;
    (void)id;
    const ip_event_got_ip6_t *event = data;
    if (event && event->esp_netif == sta_netif()) solicit_later();
}

void esp32_matter_controller_ipv6_start(void) {
    if (s_solicit_timer) return;
    const esp_timer_create_args_t args = {.callback = solicit_now, .name = "matter_router_solicit"};
    if (esp_timer_create(&args, &s_solicit_timer) != ESP_OK) return;
    esp_event_handler_register(IP_EVENT, IP_EVENT_GOT_IP6, on_got_ip6, NULL);
    solicit_later();
}
