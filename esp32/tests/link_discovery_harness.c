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

// Host discovery harness: deterministic probes, resource limits, and failure cleanup.
// The runner extracts production discovery code into discovery_all.inc.
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include "cJSON.h"

#define ESP_OK 0
#define ERR_OK 0
#define ERR_MEM -1
#define ERR_IF -12
#define ARP_TABLE_SIZE 16 // Non-default size catches hardcoded ten-entry harvesting.
typedef int err_t;
#define pdMS_TO_TICKS(x) (x)
#define ESP_LOGI(...) ((void)TAG)
#define ESP_LOGW(...) ((void)TAG)
#define IPSTR "%u.%u.%u.%u"
#define IP2STR(x) ((const uint8_t *)&(x)->addr)[0], ((const uint8_t *)&(x)->addr)[1], \
                  ((const uint8_t *)&(x)->addr)[2], ((const uint8_t *)&(x)->addr)[3]
struct netif { int unused; };
struct eth_addr { uint8_t addr[6]; };
typedef struct { uint32_t addr; } ip4_addr_t;
typedef struct { int unused; } esp_netif_t;
typedef struct { ip4_addr_t ip, netmask, gw; } esp_netif_ip_info_t;
static struct netif nif;
static esp_netif_t iface;
static esp_netif_ip_info_t netinfo;
static unsigned requests, batch, harvests, lock_depth, cache_count, request_error;
static uint32_t requested[1024];
static ip4_addr_t cache[ARP_TABLE_SIZE];
static err_t request_error_code;
static unsigned request_mem_every;
static const char *ssdp_reply, *wsd_reply;
static struct eth_addr mac = {{1, 2, 3, 4, 5, 6}};
static bool reply_arp, fail_seen, fail_mdns, large_ssdp;
static size_t seen_bytes, allocations, releases;
static int packets[7], received[7], active_source, sockets_open, next_fd;
static uint16_t bound_port[32];
static int64_t now, receive_tick;
static int json_fail_after = -1;

static void core_lock(void) { assert(lock_depth++ == 0); }
static void core_unlock(void) { assert(--lock_depth == 0); }
#define LOCK_TCPIP_CORE() core_lock()
#define UNLOCK_TCPIP_CORE() core_unlock()
static struct netif *esp_netif_get_netif_impl(esp_netif_t *n) { (void)n; return &nif; }
static int esp_netif_get_ip_info(esp_netif_t *n, esp_netif_ip_info_t *i) { (void)n; *i = netinfo; return ESP_OK; }
static esp_netif_t *wifi_mgr_get_netif(void) { return &iface; }
static bool wifi_mgr_is_connected(void) { return true; }
static int64_t esp_timer_get_time(void) { return now; }
static void vTaskDelay(int t) { now += t * 1000; assert(batch <= 8); batch = 0; }
static int etharp_request(struct netif *n, const ip4_addr_t *ip) {
    assert(n == &nif && lock_depth == 1 && requests < 1024);
    requested[requests++] = ntohl(ip->addr);
    batch++;
    if (request_error == requests) return request_error_code;
    if (request_mem_every && requests % request_mem_every == 0) return ERR_MEM;
    if (reply_arp) cache[cache_count++ % ARP_TABLE_SIZE] = *ip;
    return ERR_OK;
}
static int etharp_get_entry(int i, ip4_addr_t **ip, struct netif **n, struct eth_addr **m) {
    assert(lock_depth == 1);
    if (i == 0) harvests++;
    if ((unsigned)i >= cache_count || i >= ARP_TABLE_SIZE) return 0;
    *ip = &cache[i]; *n = &nif; *m = &mac; return 1;
}
static void *host_malloc(size_t n) {
    seen_bytes = n;
    if (fail_seen) return NULL;
    allocations++;
    return malloc(n);
}
static void *host_calloc(size_t n, size_t size) {
    if (fail_mdns) return NULL;
    allocations++;
    return calloc(n, size);
}
static void host_free(void *p) { if (p) releases++; free(p); }
static void *json_malloc(size_t n) {
    if (json_fail_after == 0) { json_fail_after = -1; return NULL; }
    if (json_fail_after > 0) json_fail_after--;
    return malloc(n);
}
static int host_socket(int a, int b, int c) {
    (void)a; (void)b; (void)c; sockets_open++;
    int fd = 10 + next_fd++ % 32;
    bound_port[fd - 10] = 0;
    return fd;
}
static int host_close(int fd) { assert(fd >= 10 && fd < 42 && sockets_open > 0); sockets_open--; return 0; }
static int host_setsockopt(int fd, int level, int option, const void *v, socklen_t n) {
    (void)fd; (void)level; (void)option; (void)v; (void)n; return 0;
}
static uint16_t refuse_bind_port;   // a port some other socket on the device holds
static int host_bind(int fd, const struct sockaddr *addr, socklen_t n) {
    (void)n;
    uint16_t port = ntohs(((const struct sockaddr_in *)addr)->sin_port);
    if (port && port == refuse_bind_port) return -1;
    bound_port[fd - 10] = port;
    return 0;
}
// Vendor replies, each for the socket bound to its port (0: the probe socket).
typedef struct { uint16_t port; const void *data; size_t len; const char *ip; uint16_t from; } vendor_reply_t;
static const vendor_reply_t *vendor_replies;
static size_t vendor_count;
static unsigned vendor_queries_sent;
static uint8_t kasa_query_sent[64];
static size_t kasa_query_len;
static ssize_t host_sendto(int fd, const void *buf, size_t n, int flags,
                           const struct sockaddr *addr, socklen_t len) {
    (void)fd; (void)buf; (void)flags; (void)len;
    unsigned port = ntohs(((const struct sockaddr_in *)addr)->sin_port);
    switch (port) {
        case 5353: active_source = 1; break;
        case 1900: active_source = 2; break;
        case 3702: active_source = 3; break;
        case 161: active_source = 4; break;
        case 137: active_source = 5; break;
        case 9999: case 20002: case 4001: case 54321: case 38899: case 56700: case 1982: case 32108:
            active_source = 6; vendor_queries_sent++;
            if (port == 9999 && n <= sizeof(kasa_query_sent)) { memcpy(kasa_query_sent, buf, n); kasa_query_len = n; }
            break;
        default: assert(false);
    }
    return (ssize_t)n;
}
static ssize_t host_recvfrom(int fd, void *data, size_t size, int flags,
                             struct sockaddr *from, socklen_t *len) {
    (void)flags; (void)len;
    now += receive_tick;
    if (received[active_source] >= packets[active_source]) return 0;
    if (active_source == 6) {
        assert(flags == MSG_DONTWAIT);
        const vendor_reply_t *r = vendor_replies ? &vendor_replies[received[6]] : NULL;
        if (r ? r->port != bound_port[fd - 10] : bound_port[fd - 10] != 0) return -1;
        int sequence = received[6]++;
        struct sockaddr_in *in = (struct sockaddr_in *)from;
        memset(in, 0, sizeof(*in));
        if (r) {
            in->sin_addr.s_addr = inet_addr(r->ip);
            in->sin_port = htons(r->from);
            memcpy(data, r->data, r->len);
            return (ssize_t)r->len;
        }
        // One WiZ bulb per sequence number, each from its own address.
        in->sin_addr.s_addr = htonl(0x0a010000u + (uint32_t)sequence + 1);
        in->sin_port = htons(38899);
        return snprintf(data, size, "{\"method\":\"getPilot\",\"result\":{\"mac\":\"a8bb50%06x\"}}", sequence);
    }
    int sequence = received[active_source]++;
    if (from) {
        memset(from, 0, sizeof(struct sockaddr_in));
        ((struct sockaddr_in *)from)->sin_addr.s_addr = inet_addr("10.1.2.3");
    }
    uint8_t *p = data;
    memset(p, 0, size);
    switch (active_source) {
        case 1: {
            p[7] = 1;
            size_t n = 12;
            p[n++] = 0; p[n++] = 0; p[n++] = 12;
            p[n++] = 0; p[n++] = 1;
            n += 4;
            char instance[32];
            int chars = snprintf(instance, sizeof(instance), "service%d", sequence);
            p[n++] = 0; p[n++] = (uint8_t)(chars + 2);
            p[n++] = (uint8_t)chars;
            memcpy(p + n, instance, (size_t)chars); n += (size_t)chars;
            p[n++] = 0;
            return (ssize_t)n;
        }
        case 2:
            if (ssdp_reply) return snprintf(data, size, "%s", ssdp_reply);
            if (large_ssdp) {
                // Fill all four fields so the shared JSON budget still exhausts
                // with the bounded value copies (one field no longer suffices).
                static const char *headers[] = {"LOCATION", "ST", "USN", "SERVER"};
                size_t n = 0;
                for (unsigned h = 0; h < 4; h++) {
                    n += (size_t)snprintf((char *)p + n, size - n, "%s: ", headers[h]);
                    assert(n + 302 <= size);
                    memset(p + n, '\\', 300); n += 300;
                    memcpy(p + n, "\r\n", 2); n += 2;
                }
                return (ssize_t)n;
            }
            return snprintf(data, size, "LOCATION: http://10.1.2.3/%d\r\nST: test\r\n", sequence);
        case 3:
            if (wsd_reply) return snprintf(data, size, "%s", wsd_reply);
            return snprintf(data, size, "<wsd:Types>camera</wsd:Types>");
        case 4: {
            const uint8_t snmp[] = {6,8,0x2b,6,1,2,1,1,1,0,4,3,'a','b','c'};
            memcpy(p, snmp, sizeof(snmp)); return sizeof(snmp);
        }
        case 5:
            p[12] = 0; p[21] = 0; p[22] = 19; p[23] = 1;
            memcpy(p + 24, "host           ", 15); return 42;
        default: assert(false); return -1;
    }
}
#define malloc host_malloc
#define calloc host_calloc
#define free host_free
#define socket host_socket
#define close host_close
#define setsockopt host_setsockopt
#define bind host_bind
#define sendto host_sendto
#define recvfrom host_recvfrom
#include "discovery_all.inc"
#undef malloc
#undef calloc
#undef free

static void reset(const char *ip, const char *mask) {
    assert(allocations == releases && sockets_open == 0 && lock_depth == 0);
    netinfo.ip.addr = inet_addr(ip); netinfo.netmask.addr = inet_addr(mask);
    netinfo.gw.addr = inet_addr("10.0.0.1");
    requests = batch = harvests = cache_count = request_error = request_mem_every = 0;
    request_error_code = ERR_IF;
    ssdp_reply = wsd_reply = NULL;
    fail_seen = fail_mdns = large_ssdp = reply_arp = false;
    seen_bytes = 0; now = 0; receive_tick = 1000;
    memset(packets, 0, sizeof(packets)); memset(received, 0, sizeof(received));
    vendor_replies = NULL; vendor_count = 0; vendor_queries_sent = 0;
    refuse_bind_port = 0;
}
static cJSON *run_source(const char *name) {
    cJSON *params = cJSON_CreateObject();
    cJSON *sources = cJSON_AddArrayToObject(params, "sources");
    cJSON_AddItemToArray(sources, cJSON_CreateString(name));
    cJSON *result = net_discovery_run(params);
    cJSON_Delete(params); assert(result); return result;
}
static cJSON *report(cJSON *r, const char *name) { return cJSON_GetObjectItem(cJSON_GetObjectItem(r, "sources"), name); }
static int number(cJSON *r, const char *name) { return cJSON_GetObjectItem(r, name)->valueint; }
static bool flag(cJSON *r, const char *name) { return cJSON_IsTrue(cJSON_GetObjectItem(r, name)); }
static void check_counts(cJSON *result) {
    cJSON *reports = cJSON_GetObjectItem(result, "sources"), *obs = cJSON_GetObjectItem(result, "observations");
    int total = 0;
    for (cJSON *r = reports->child; r; r = r->next) {
        int found = 0;
        for (cJSON *o = obs->child; o; o = o->next)
            found += strcmp(cJSON_GetObjectItem(o, "source")->valuestring, r->string) == 0;
        assert(found == number(r, "found")); total += found;
    }
    assert(total == cJSON_GetArraySize(obs));
}
static void check_arp(void) {
    const char *ips[] = {"10.20.100.100", "10.20.0.1", "10.20.255.254"};
    for (unsigned i = 0; i < 3; i++) {
        reset(ips[i], "255.255.0.0"); reply_arp = true;
        cJSON *r = run_source("arp"), *a = report(r, "arp");
        assert(requests == 1024 && seen_bytes == 4096 && number(a, "found") == 1024);
        assert(number(a, "swept_hosts") == 1024 && number(a, "subnet_hosts") == 65534);
        assert(flag(a, "truncated") && !flag(r, "complete") && flag(r, "ok"));
        assert(harvests == 128 && number(a, "cap") == 1024);
        for (unsigned j = 1; j < requests; j++) assert(requested[j] == requested[j - 1] + 1);
        uint32_t own = ntohl(netinfo.ip.addr);
        assert(requested[0] <= own && requested[1023] >= own);
        assert(requested[0] >= 0x0a140001 && requested[1023] <= 0x0a14fffe);
        if (i == 0) assert(requested[0] == own - 512);
        check_counts(r); cJSON_Delete(r);
    }
    reset("192.168.1.4", "255.255.255.0"); reply_arp = true;
    cJSON *r = run_source("arp");
    assert(requests == 254 && seen_bytes == 254 * 4 && number(report(r,"arp"),"found") == 254);
    assert(flag(r,"complete") && !flag(report(r,"arp"),"truncated")); cJSON_Delete(r);
    reset("10.20.30.40", "0.0.0.0");
    r = run_source("arp");
    assert(cJSON_GetObjectItem(report(r,"arp"),"subnet_hosts")->valuedouble == 4294967294.0);
    assert(requests == 1024); cJSON_Delete(r);
    for (int suffix = 254; suffix <= 255; suffix++) {
        char mask[32]; snprintf(mask,sizeof(mask),"255.255.255.%d",suffix);
        reset("10.0.0.1",mask); r = run_source("arp");
        assert(!requests && !seen_bytes && flag(r,"complete")); cJSON_Delete(r);
    }
    reset("10.0.0.4", "255.255.255.0"); request_error = 9;
    r = run_source("arp");
    assert(requests == 16 && number(report(r,"arp"),"swept_hosts") == 15);
    assert(!flag(r,"ok") && !flag(r,"complete"));
    assert(!strcmp(cJSON_GetObjectItem(report(r,"arp"),"error")->valuestring,"arp_request_failed"));
    cJSON_Delete(r);
    const unsigned periods[] = {0, 8, 1}; // One failure, recurring pressure, no requests accepted.
    for (unsigned i = 0; i < sizeof(periods) / sizeof(periods[0]); i++) {
        reset("10.0.0.4", "255.255.255.0"); reply_arp = true;
        request_mem_every = periods[i];
        if (!request_mem_every) { request_error = 9; request_error_code = ERR_MEM; }
        int accepted = 254 - (request_mem_every ? 254 / request_mem_every : 1);
        r = run_source("arp"); cJSON *a = report(r,"arp");
        assert(requests == 254 && number(a,"swept_hosts") == accepted);
        assert(number(a,"found") == accepted && number(a,"subnet_hosts") == 254);
        assert(flag(a,"truncated") && !flag(r,"complete") && flag(r,"ok"));
        assert(!cJSON_GetObjectItem(a,"error") && harvests == 33);
        assert(requested[0] == 0x0a000001 && requested[253] == 0x0a0000fe);
        for (unsigned j = 1; j < requests; j++) assert(requested[j] == requested[j - 1] + 1);
        check_counts(r); cJSON_Delete(r);
    }
}
static void check_caps(void) {
    const char *names[] = {"mdns","ssdp","wsd","snmp","netbios","vendor"};
    int caps[] = {256,256,64,128,256,128};
    for (int i = 0; i < 6; i++) for (int delta = -1; delta <= 1; delta++) {
        reset("10.0.0.4", "255.255.255.0"); packets[i + 1] = caps[i] + delta;
        cJSON *r = run_source(names[i]), *s = report(r,names[i]);
        assert(number(s,"found") == caps[i] + (delta < 0 ? delta : 0));
        assert(number(s,"cap") == caps[i] && number(s,"listen_ms") > 0);
        assert(flag(s,"truncated") == (delta >= 0));
        assert(flag(r,"complete") == (delta < 0) && flag(r,"ok"));
        check_counts(r); cJSON_Delete(r);
    }
    reset("10.0.0.4", "255.255.255.0"); packets[2] = 1000; receive_tick = 1000000;
    cJSON *r = run_source("ssdp");
    assert(number(report(r,"ssdp"),"found") == 3 && flag(r,"complete")); cJSON_Delete(r);
    reset("10.0.0.4", "255.255.255.0"); r = net_discovery_run(NULL);
    assert(flag(r,"complete")); check_counts(r); cJSON_Delete(r);
}
static void check_network_strings(void) {
    reset("10.0.0.4", "255.255.255.0");
    ssdp_reply = "HTTP/1.1 200 OK\r\n"
                 "LOCATION: http://10.1.2.3/\xffrouter\x01\r\n"
                 "ST: urn:\x80test:device\r\n"
                 "USN: uuid:\xfeid\r\n"
                 "SERVER: brand\xe9 \"box\"\\\t1\nRole: trusted\r\n\r\n";
    wsd_reply = "<wsd:Types>\xff" "camera\r\x01\tprinter\n\x80</wsd:Types>";
    packets[2] = packets[3] = 1;
    cJSON *r = net_discovery_run(NULL);
    assert(flag(r,"ok") && flag(r,"complete")); check_counts(r);
    assert(number(report(r,"ssdp"),"found") == 1 && number(report(r,"wsd"),"found") == 1);
    cJSON *obs = cJSON_GetObjectItem(r,"observations");
    cJSON *ssdp = cJSON_GetArrayItem(obs,0), *wsd = cJSON_GetArrayItem(obs,1);
    assert(!strcmp(cJSON_GetObjectItem(ssdp,"location")->valuestring,"http://10.1.2.3/router"));
    assert(!strcmp(cJSON_GetObjectItem(ssdp,"st")->valuestring,"urn:test:device"));
    assert(!strcmp(cJSON_GetObjectItem(ssdp,"usn")->valuestring,"uuid:id"));
    assert(!strcmp(cJSON_GetObjectItem(ssdp,"server")->valuestring,
                   "brand \"box\"\\\t1 Role: trusted"));
    assert(!strcmp(cJSON_GetObjectItem(wsd,"types")->valuestring,"camera\tprinter "));
    cJSON *envelope = cJSON_CreateObject();
    cJSON_AddStringToObject(envelope,"method","link.result");
    cJSON_AddStringToObject(envelope,"id","invalid-network-bytes");
    cJSON_AddBoolToObject(envelope,"ok",true);
    cJSON_AddItemToObject(envelope,"payload",r);
    char *json = cJSON_PrintUnformatted(envelope); assert(json);
    // cJSON's parser does not validate UTF-8; assert ASCII separately.
    for (const unsigned char *p = (const unsigned char *)json; *p; p++) assert(*p < 0x80);
    cJSON *parsed = cJSON_Parse(json); assert(parsed); cJSON_Delete(parsed);
    free(json); cJSON_Delete(envelope);
}
static void check_oom(void) {
    const char *names[] = {"arp","mdns"};
    for (int i = 0; i < 2; i++) {
        reset("10.0.0.4", "255.255.255.0"); fail_seen = i == 0; fail_mdns = i == 1;
        cJSON *r = run_source(names[i]), *s = report(r,names[i]);
        assert(!strcmp(cJSON_GetObjectItem(s,"error")->valuestring,"out_of_memory"));
        assert(!flag(r,"ok") && !flag(r,"complete") && !flag(s,"truncated"));
        assert(number(s,"found") == 0); cJSON_Delete(r);
    }
    reset("10.0.0.4", "255.255.255.0");
    cJSON_Hooks hooks = {.malloc_fn=json_malloc, .free_fn=free}; cJSON_InitHooks(&hooks);
    cJSON *e = cJSON_CreateObject();
    json_fail_after = 0; observation_string(&e,"source","arp"); assert(!e);
    discovery_source_t source = {0};
    discovery_budget_t budget = {.obs=cJSON_CreateArray(), .estimated_bytes=DISCOVERY_METADATA_BYTES};
    assert(!append_observation(&budget,&source,e) && source.error && !source.truncated);
    cJSON_Delete(budget.obs); cJSON_InitHooks(NULL);
}
static void check_budget(void) {
    for (int remaining = 9; remaining <= 10; remaining++) {
        discovery_source_t s = {0};
        discovery_budget_t b = {.obs=cJSON_CreateArray(), .estimated_bytes=DISCOVERY_MAX_JSON_BYTES-(size_t)remaining};
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e,"x","y");
        // {"x":"y"} is 9 bytes; the conservative bound with separators is 11.
        assert(!append_observation(&b,&s,e) && s.truncated);
        cJSON_Delete(b.obs);
    }
    {
        discovery_source_t s = {0};
        discovery_budget_t b = {.obs=cJSON_CreateArray(), .estimated_bytes=DISCOVERY_MAX_JSON_BYTES-11};
        cJSON *e = cJSON_CreateObject(); cJSON_AddStringToObject(e,"x","y");
        assert(append_observation(&b,&s,e) && b.estimated_bytes == DISCOVERY_MAX_JSON_BYTES);
        assert(!s.truncated && !b.exhausted);
        e = cJSON_CreateObject(); cJSON_AddStringToObject(e,"x","y");
        assert(!append_observation(&b,&s,e) && s.found == 1 && s.truncated);
        cJSON_Delete(b.obs);
    }
    for (int i = 0; i < 7; i++) {
        reset("10.0.0.4", "255.255.255.0"); reply_arp = true; packets[i] = 1;
        discovery_source_t s = {0};
        discovery_budget_t b = {.obs=cJSON_CreateArray(), .estimated_bytes=DISCOVERY_MAX_JSON_BYTES-1};
        switch (i) {
            case 0: arp_probe(&b,&iface,&s); break;
            case 1: mdns_probe(&b,&s); break;
            case 2: ssdp_probe(&b,&s); break;
            case 3: wsd_probe(&b,&s); break;
            case 4: snmp_probe(&b,&iface,&s); break;
            case 5: netbios_probe(&b,&iface,&s); break;
            case 6: vendor_probe(&b,&s); break;
        }
        assert(s.truncated && b.exhausted && s.found == 0 && !s.error);
        assert(cJSON_GetArraySize(b.obs) == 0); cJSON_Delete(b.obs);
    }
    reset("10.0.0.4", "255.255.255.0"); reply_arp = true; large_ssdp = true;
    for (int i = 1; i < 7; i++) packets[i] = 1000;
    cJSON *r = net_discovery_run(NULL);
    assert(flag(r,"ok") && !flag(r,"complete"));
    assert(number(report(r,"ssdp"),"found") > 0 && number(report(r,"ssdp"),"found") < MAX_SSDP);
    for (const cJSON *s = report(r,"ssdp"); s; s = s->next) assert(flag((cJSON *)s,"truncated"));
    assert(!received[3] && !received[4] && !received[5] && !received[6]); check_counts(r);
    char id[64]; memset(id,1,63); id[63] = 0;
    cJSON *envelope = cJSON_CreateObject();
    cJSON_AddStringToObject(envelope,"method","link.result");
    cJSON_AddStringToObject(envelope,"id",id); cJSON_AddBoolToObject(envelope,"ok",true);
    cJSON_AddItemToObject(envelope,"payload",r);
    char *json = cJSON_PrintUnformatted(envelope); assert(json);
    assert(strlen(json) <= DISCOVERY_MAX_JSON_BYTES && strlen(json) > 240 * 1024);
    cJSON *parsed = cJSON_Parse(json); assert(parsed); cJSON_Delete(parsed);
    printf("PASS full result envelope: %zu bytes <= 262144, escaped single-host flood bounded\n",strlen(json));
    free(json); cJSON_Delete(envelope);
    reset("10.0.0.4", "255.255.255.0");
}
static cJSON *vendor_find(cJSON *r, const char *protocol, const char *ip) {
    for (cJSON *o = cJSON_GetObjectItem(r, "observations")->child; o; o = o->next)
        if (!strcmp(cJSON_GetObjectItem(o, "protocol")->valuestring, protocol)
            && !strcmp(cJSON_GetObjectItem(o, "ip")->valuestring, ip)) return o;
    assert(false);
    return NULL;
}
static const char *field(cJSON *o, const char *name) {
    cJSON *v = cJSON_GetObjectItem(o, name);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}
static void check_vendor(void) {
    reset("10.0.0.4", "255.255.255.0");
    uint8_t kasa[256];
    int kasa_len = snprintf((char *)kasa, sizeof(kasa),
        "{\"system\":{\"get_sysinfo\":{\"model\":\"HS200(US)\",\"alias\":\"Desk Lamp\","
        "\"mac\":\"AA:BB:CC:00:11:22\",\"latitude_i\":111111,\"longitude_i\":-222222}}}");
    kasa_xor(kasa, kasa_len, true);
    uint8_t tdp[256] = {0x02, 0x00, 0x00, 0x01};
    int tdp_len = 16 + snprintf((char *)tdp + 16, sizeof(tdp) - 16,
        "{\"error_code\":0,\"result\":{\"device_type\":\"SMART.KASASWITCH\","
        "\"device_model\":\"HS200(US)\",\"mac\":\"AA-BB-CC-00-11-33\",\"owner\":\"0123abcd\","
        "\"mgt_encrypt_schm\":{\"encrypt_type\":\"KLAP\",\"http_port\":80}}}");
    static const char govee[] = "{\"msg\":{\"cmd\":\"scan\",\"data\":{\"ip\":\"10.1.2.54\","
        "\"device\":\"AA:BB:CC:DD:EE:FF:00:11\",\"sku\":\"H6008\"}}}";
    static const uint8_t miio[32] = {0x21, 0x31, 0x00, 0x20, 0, 0, 0, 0, 0x03, 0xae, 0x5a, 0x42,
        0, 0, 0x10, 0, 0xde, 0xad, 0xbe, 0xef, 0xde, 0xad, 0xbe, 0xef,
        0xde, 0xad, 0xbe, 0xef, 0xde, 0xad, 0xbe, 0xef};
    static const char wiz[] = "{\"method\":\"getPilot\",\"env\":\"pro\",\"result\":{\"mac\":\"a8bb50112233\",\"state\":true}}";
    static const uint8_t lifx[41] = {41, 0, 0x00, 0x14, 1, 2, 3, 4, 0xd0, 0x73, 0xd5, 0x01, 0x02, 0x03, 0, 0,
        [32] = 3, [36] = 1, 0x7c, 0xdd, 0, 0};
    static const char yeelight[] = "HTTP/1.1 200 OK\r\nCache-Control: max-age=3600\r\n"
        "Location: yeelight://10.1.2.7:55443\r\nid: 0x0000000000c0ffee\r\nmodel: color\r\nname: Desk\r\n";
    static const uint8_t cs2[24] = {0xf1, 0x41, 0x00, 0x14, 'H', 'D', 'P', 'P', 0, 0, 0, 0,
        0x00, 0x01, 0xe2, 0x40, 'A', 'B', 'C', 'D', 'E', 0, 0, 0};
    uint8_t tuya[256] = {0, 0, 0x55, 0xaa, 0, 0, 0, 0, 0, 0, 0, 0x13};
    int tuya_len = 20 + snprintf((char *)tuya + 20, sizeof(tuya) - 28,
        "{\"ip\":\"10.1.2.100\",\"gwId\":\"bf12ab34cd\",\"active\":2,"
        "\"productKey\":\"keym557nqw3p8p7m\",\"version\":\"3.1\"}");
    memcpy(tuya + tuya_len, "\x12\x34\x56\x78\x00\x00\xaa\x55", 8); tuya_len += 8;
    static const uint8_t tuya_enc[40] = {0, 0, 0x55, 0xaa, 0, 0, 0, 0, 0, 0, 0, 0x13, 0, 0, 0, 0x1c,
        0, 0, 0, 0, 0x9a, 0x7b, 0x11, 0xc4, '{', 0x80, [32] = 0x12, 0x34, 0x56, 0x78, 0, 0, 0xaa, 0x55};
    static const char bambu[] = "NOTIFY * HTTP/1.1\r\nHOST: 239.255.255.250:1990\r\nLocation: 10.1.2.94\r\n"
        "USN: 01S00C123456789\r\nDevModel.bambu.com: O1C2\r\nDevName.bambu.com: workshop\r\n\r\n";
    static const uint8_t tuya_bare[20] = {0, 0, 0x55, 0xaa};
    /* Newer devices' frame on 7000: 6699 prefix and suffix, the payload opaque. */
    static const uint8_t tuya_6699[48] = {0, 0, 0x66, 0x99, [9] = 1, [13] = 0x13, [17] = 0x1e,
        [44] = 0, 0, 0x99, 0x66};
    static const uint8_t tuya_6699_cut[30] = {0, 0, 0x66, 0x99, [9] = 1, [13] = 0x13, [17] = 0x1e};
    static const uint8_t miio_bad[32] = {0x21, 0x31, 0x00, 0x40};
    static const uint8_t cs2_bad[12] = {0xf1, 0x41, 0x00, 0x00};
    static const uint8_t tuya_empty[28] = {0, 0, 0x55, 0xaa, [11] = 0x13, [20] = 'x', [24] = 0, 0, 0xaa, 0x55};
    static const uint8_t tdp_cut[] = {0x02, 0, 0, 0x01, [16] = '{', '"', 'r', 'e', 's', 'u', 'l', 't', '"', ':'};
    const vendor_reply_t replies[] = {
        /* Junk that looks like a vendor's, or arrives on a vendor's port: ignored,
           so it takes neither a device's one observation nor the cap. */
        {0, "\xd0", 1, "10.1.2.64", 9999},
        {0, tdp_cut, sizeof(tdp_cut), "10.1.2.86", 20002},
        {0, "{\"getPilot\"}", 12, "10.1.2.72", 38899},
        {4002, "x", 1, "10.1.2.55", 4001},
        {6667, "\x01\x02\x03", 3, "10.1.2.102", 49154},
        {2021, "junk", 4, "10.1.2.95", 2021},
        {6667, tuya_bare, sizeof(tuya_bare), "10.1.2.102", 49153},   /* a prefix, no frame */
        {7000, tuya_6699_cut, sizeof(tuya_6699_cut), "10.1.2.103", 49155},
        {0, miio_bad, sizeof(miio_bad), "10.1.2.89", 54321},          /* lengths that don't match */
        {0, cs2_bad, sizeof(cs2_bad), "10.1.2.109", 32108},
        {6666, tuya_empty, sizeof(tuya_empty), "10.1.2.100", 49152}, /* a frame, no fields */
        {0, kasa, (size_t)kasa_len, "10.1.2.64", 9999},
        {0, "hello", 5, "10.1.2.9", 1234},                     /* not a vendor's: ignored */
        {0, tdp, (size_t)tdp_len, "10.1.2.87", 20002},
        {4002, govee, sizeof(govee) - 1, "10.1.2.54", 4001},
        {0, miio, sizeof(miio), "10.1.2.88", 54321},
        {0, wiz, sizeof(wiz) - 1, "10.1.2.70", 38899},
        {0, wiz, sizeof(wiz) - 1, "10.1.2.70", 38899},         /* the same bulb again: once */
        {0, lifx, sizeof(lifx), "10.1.2.71", 56700},
        {0, yeelight, sizeof(yeelight) - 1, "10.1.2.7", 51234},
        {0, cs2, sizeof(cs2), "10.1.2.108", 32108},
        {6666, tuya, (size_t)tuya_len, "10.1.2.100", 49152},
        {6667, tuya_enc, sizeof(tuya_enc), "10.1.2.101", 49153},
        {2021, bambu, sizeof(bambu) - 1, "10.1.2.94", 2021},
        {7000, tuya_6699, sizeof(tuya_6699), "10.1.2.104", 49156},
    };
    vendor_replies = replies; vendor_count = sizeof(replies) / sizeof(replies[0]);
    packets[6] = (int)vendor_count;
    cJSON *r = run_source("vendor"), *v = report(r, "vendor");
    assert(received[6] == (int)vendor_count && vendor_queries_sent == 16);   /* each query twice */
    kasa_xor(kasa_query_sent, (int)kasa_query_len, false);
    assert(kasa_query_len == 29 && !memcmp(kasa_query_sent, "{\"system\":{\"get_sysinfo\":{}}}", 29));
    assert(number(v, "found") == 12 && !flag(v, "truncated") && flag(r, "complete"));
    check_counts(r);
    cJSON *o = vendor_find(r, "kasa", "10.1.2.64");
    assert(!strcmp(field(o, "model"), "HS200(US)") && !strcmp(field(o, "name"), "Desk Lamp"));
    assert(!strcmp(field(o, "mac"), "AA:BB:CC:00:11:22") && number(o, "port") == 9999);
    o = vendor_find(r, "tplink", "10.1.2.87");
    assert(!strcmp(field(o, "model"), "HS200(US)") && !strcmp(field(o, "type"), "SMART.KASASWITCH"));
    assert(!strcmp(field(o, "encryption"), "KLAP") && !field(o, "owner"));
    o = vendor_find(r, "govee", "10.1.2.54");
    assert(!strcmp(field(o, "model"), "H6008") && !strcmp(field(o, "id"), "AA:BB:CC:DD:EE:FF:00:11"));
    assert(number(o, "port") == 4002);
    assert(!strcmp(field(vendor_find(r, "miio", "10.1.2.88"), "id"), "03ae5a42"));
    assert(!strcmp(field(vendor_find(r, "wiz", "10.1.2.70"), "mac"), "a8bb50112233"));
    assert(!strcmp(field(vendor_find(r, "lifx", "10.1.2.71"), "mac"), "d073d5010203"));
    o = vendor_find(r, "yeelight", "10.1.2.7");
    assert(!strcmp(field(o, "model"), "color") && !strcmp(field(o, "name"), "Desk"));
    assert(!strcmp(field(o, "id"), "0x0000000000c0ffee"));
    o = vendor_find(r, "cs2", "10.1.2.108");
    assert(!strcmp(field(o, "id_prefix"), "HDPP"));
    o = vendor_find(r, "tuya", "10.1.2.100");
    assert(!strcmp(field(o, "id"), "bf12ab34cd") && !strcmp(field(o, "model"), "keym557nqw3p8p7m"));
    assert(!strcmp(field(o, "version"), "3.1") && number(o, "port") == 6666);
    o = vendor_find(r, "tuya", "10.1.2.101");
    assert(number(o, "port") == 6667 && !field(o, "model") && !field(o, "id"));
    o = vendor_find(r, "tuya", "10.1.2.104");
    assert(number(o, "port") == 7000 && !field(o, "id"));
    o = vendor_find(r, "bambu", "10.1.2.94");
    assert(!strcmp(field(o, "model"), "O1C2") && !strcmp(field(o, "name"), "workshop"));
    // Location, the miIO token, the camera's serial and check code stay out.
    char *json = cJSON_PrintUnformatted(r);
    assert(!strstr(json, "111111") && !strstr(json, "deadbeef") && !strstr(json, "123456")
           && !strstr(json, "ABCDE") && !strstr(json, "0123abcd"));
    free(json); cJSON_Delete(r);
    // A listener that can't open (another socket holds Tuya's port): the scan
    // goes on, but doesn't call itself complete.
    reset("10.0.0.4", "255.255.255.0");
    refuse_bind_port = 6667;
    packets[6] = 1;
    r = run_source("vendor"); v = report(r, "vendor");
    assert(number(v, "found") == 1 && flag(v, "truncated") && !flag(r, "complete"));
    cJSON_Delete(r);
    reset("10.0.0.4", "255.255.255.0");
}
int main(void) {
    check_arp(); check_caps(); check_network_strings(); check_oom(); check_budget(); check_vendor();
    puts("PASS discovery: clamp and subnet edges, synchronous 8-request batches/16-entry cache, transient ARP pressure, sanitized network strings, all seven source caps and budgets, vendor probes and their fields, shared envelope limit, allocation failures and cleanup");
    return 0;
}
