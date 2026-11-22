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

#include "net_discovery.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"

#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/etharp.h"
#include "lwip/ip4_addr.h"
#include "lwip/tcpip.h"

#include "wifi_mgr.h"

static const char *TAG = "link.discover";

// Keep printable ASCII (0x20-0x7E) and tab, and turn newlines into spaces.
// Other hosts on the LAN write these strings, so they must not carry line
// breaks into the agent's input.
// Operates in-place, returns the new length.
static int sanitize_ascii(char *s) {
    char *r = s, *w = s;
    while (*r) {
        unsigned char c = (unsigned char)*r;
        if ((c >= 0x20 && c <= 0x7E) || c == '\t') *w++ = *r;
        else if (c == '\n') *w++ = ' ';
        r++;
    }
    *w = '\0';
    return (int)(w - s);
}

#define SRC_ARP     0x01
#define SRC_MDNS    0x02
#define SRC_SSDP    0x04
#define SRC_WSD     0x08
#define SRC_SNMP    0x10
#define SRC_NETBIOS 0x20
#define SRC_VENDOR  0x40
#define SRC_ALL     0x7F

#define ARP_BATCH_SIZE      8
#define ARP_BATCH_DELAY_MS  150
#define ARP_SETTLE_MS       300
#define MDNS_LISTEN_MS      3000
#define SSDP_LISTEN_MS      3000
#define WSD_LISTEN_MS       3000
#define SNMP_LISTEN_MS      2000
#define NETBIOS_LISTEN_MS   2000
#define VENDOR_LISTEN_MS    6000   // Tuya broadcasts about every 5 s, Bambu every 4 to 6 s
// The vendor queries go again this far in: Wi-Fi loses some of a burst of
// broadcasts (a miIO hello alone was answered 8 times in 8, in the burst 2 in 6).
#define VENDOR_RESEND_MS    1500
#define RECV_BUF            1460
// Includes NUL, matching SNMP's value buffer. Log shortened fields without
// marking source truncation: that flag tracks omitted observations/coverage.
#define DISCOVERY_VALUE_BUF 256

#define MDNS_PORT   5353
#define SSDP_PORT   1900
#define WSD_PORT    3702

#define MAX_ARP     1024
#define MAX_MDNS    256
#define MAX_SSDP    256
#define MAX_WSD     64
#define MAX_SNMP    128
#define MAX_NETBIOS 256
#define MAX_VENDOR  128
#define MAX_HEARD   256   // distinct senders kept for the silent hint

#define DISCOVERY_MAX_JSON_BYTES (256 * 1024)
// Covers network/source metadata and the link.result envelope, including the
// escaped 63-byte request ID. The daemon caps the entire JSON at 256 KiB.
#define DISCOVERY_METADATA_BYTES 4096

// ---- helpers ---------------------------------------------------------------

static void ip_str(uint32_t net_addr, char *buf, int len) {
    uint8_t *b = (uint8_t *)&net_addr;
    snprintf(buf, len, "%d.%d.%d.%d", b[0], b[1], b[2], b[3]);
}

static uint32_t parse_sources(cJSON *params) {
    cJSON *arr = params ? cJSON_GetObjectItem(params, "sources") : NULL;
    if (!cJSON_IsArray(arr)) return SRC_ALL;
    uint32_t m = 0;
    int n = cJSON_GetArraySize(arr);
    for (int i = 0; i < n; i++) {
        cJSON *s = cJSON_GetArrayItem(arr, i);
        if (!cJSON_IsString(s)) continue;
        const char *v = s->valuestring;
        if (!strcmp(v, "arp"))          m |= SRC_ARP;
        else if (!strcmp(v, "mdns"))    m |= SRC_MDNS;
        else if (!strcmp(v, "ssdp"))    m |= SRC_SSDP;
        else if (!strcmp(v, "wsd"))     m |= SRC_WSD;
        else if (!strcmp(v, "snmp"))    m |= SRC_SNMP;
        else if (!strcmp(v, "netbios")) m |= SRC_NETBIOS;
        else if (!strcmp(v, "vendor"))  m |= SRC_VENDOR;
    }
    return m ? m : SRC_ALL;
}

typedef struct {
    int found;
    int cap;
    bool truncated;
    const char *error;
    uint32_t swept_hosts;
    uint32_t subnet_hosts;
} discovery_source_t;

typedef struct {
    cJSON *obs;
    size_t estimated_bytes;
    bool exhausted;
    // Distinct senders of any datagram a scan socket received, used only for
    // the silent hint (NULL: not kept). Partial when one more didn't fit, or
    // when the scan held 5353 without joining the mDNS group (multicast
    // answers miss it).
    uint32_t *heard;
    int heard_count;
    bool heard_partial;
} discovery_budget_t;

static size_t json_string_bound(const char *s) {
    size_t bytes = 2;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        bytes += *p < 0x20 ? 6 : (*p == '"' || *p == '\\' ? 2 : 1);
    return bytes;
}

static void observation_string(cJSON **entry, const char *key, const char *value) {
    if (*entry && !cJSON_AddStringToObject(*entry, key, value)) {
        cJSON_Delete(*entry);
        *entry = NULL;
    }
}

static bool append_observation(discovery_budget_t *budget,
                               discovery_source_t *source, cJSON *entry) {
    if (!entry) { source->error = "out_of_memory"; return false; }
    // Observations contain only strings and numbers; 26 bounds a JSON double.
    size_t bytes = 3;
    for (cJSON *field = entry->child; field; field = field->next) {
        bytes += json_string_bound(field->string) + 2;
        bytes += cJSON_IsString(field) ? json_string_bound(field->valuestring) : 26;
    }
    if (budget->exhausted || bytes > DISCOVERY_MAX_JSON_BYTES - budget->estimated_bytes) {
        budget->exhausted = true;
        source->truncated = true;
        cJSON_Delete(entry);
        return false;
    }
    if (!cJSON_AddItemToArray(budget->obs, entry)) {
        cJSON_Delete(entry);
        source->error = "out_of_memory";
        return false;
    }
    budget->estimated_bytes += bytes;
    source->found++;
    return true;
}

// Called for every datagram a scan socket receives, before it's parsed.
static void note_heard(discovery_budget_t *budget, uint32_t ip) {
    if (!budget->heard) return;
    for (int i = 0; i < budget->heard_count; i++)
        if (budget->heard[i] == ip) return;
    if (budget->heard_count < MAX_HEARD)
        budget->heard[budget->heard_count++] = ip;
    else
        budget->heard_partial = true;
}

// ---- ARP -------------------------------------------------------------------

static void arp_harvest(struct netif *lwip_nif, discovery_budget_t *budget,
                        uint32_t *seen, discovery_source_t *source, int max) {
    LOCK_TCPIP_CORE();
    for (int i = 0; i < ARP_TABLE_SIZE && source->found < max; i++) {
        ip4_addr_t *eip; struct netif *enif; struct eth_addr *emac;
        if (!etharp_get_entry(i, &eip, &enif, &emac)) continue;
        if (enif != lwip_nif || !eip || !emac) continue;
        bool dup = false;
        for (int j = 0; j < source->found; j++)
            if (seen[j] == eip->addr) { dup = true; break; }
        if (dup) continue;
        seen[source->found] = eip->addr;
        cJSON *e = cJSON_CreateObject();
        observation_string(&e, "source", "arp");
        char ib[16]; snprintf(ib, sizeof(ib), IPSTR, IP2STR(eip));
        observation_string(&e, "ip", ib);
        char mb[18];
        snprintf(mb, sizeof(mb), "%02x:%02x:%02x:%02x:%02x:%02x",
                 emac->addr[0], emac->addr[1], emac->addr[2],
                 emac->addr[3], emac->addr[4], emac->addr[5]);
        observation_string(&e, "mac", mb);
        if (!append_observation(budget, source, e)) break;
    }
    UNLOCK_TCPIP_CORE();
}

static void arp_probe(discovery_budget_t *budget, esp_netif_t *netif,
                      discovery_source_t *source) {
    struct netif *lwip_nif = esp_netif_get_netif_impl(netif);
    if (!lwip_nif) { source->error = "no_interface"; return; }

    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(netif, &info) != ESP_OK) {
        source->error = "ip_info_failed";
        return;
    }
    uint32_t own = ntohl(info.ip.addr);
    uint32_t net = own & ntohl(info.netmask.addr);
    uint32_t bcast = net | ~ntohl(info.netmask.addr);
    source->subnet_hosts = bcast > net ? bcast - net - 1 : 0;
    int hc = source->subnet_hosts > MAX_ARP ? MAX_ARP : (int)source->subnet_hosts;
    if (hc == 0) return;
    source->truncated = source->subnet_hosts > (uint32_t)hc;
    uint32_t first = net + 1;
    if (source->truncated) {
        first = own - net > (uint32_t)hc / 2 ? own - hc / 2 : net + 1;
        if (first > bcast - hc) first = bcast - hc;
    }

    // In PSRAM where there is any: 4 KB of internal RAM at the scan's peak.
    uint32_t *seen = heap_caps_malloc(hc * sizeof(uint32_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!seen) seen = malloc(hc * sizeof(uint32_t));
    if (!seen) { source->error = "out_of_memory"; return; }

    ESP_LOGI(TAG, "ARP sweep: %d hosts", hc);
    for (int off = 0; off < hc && source->found < hc
         && !budget->exhausted && !source->error; off += ARP_BATCH_SIZE) {
        int cnt = hc - off;
        if (cnt > ARP_BATCH_SIZE) cnt = ARP_BATCH_SIZE;
        // Keep the batch synchronous under the core lock: queued callbacks can
        // outlive the delay and otherwise reuse overwritten batch coordinates.
        LOCK_TCPIP_CORE();
        for (int i = 0; i < cnt; i++) {
            ip4_addr_t target = { .addr = htonl(first + off + i) };
            err_t err = etharp_request(lwip_nif, &target);
            if (err == ERR_OK)
                source->swept_hosts++;
            else if (err == ERR_MEM)
                source->truncated = true; // Skip transient pbuf exhaustion; keep sweeping.
            else
                source->error = "arp_request_failed";
        }
        UNLOCK_TCPIP_CORE();
        vTaskDelay(pdMS_TO_TICKS(ARP_BATCH_DELAY_MS));
        arp_harvest(lwip_nif, budget, seen, source, hc);
    }
    if (source->found < hc && !budget->exhausted && !source->error) {
        vTaskDelay(pdMS_TO_TICKS(ARP_SETTLE_MS));
        arp_harvest(lwip_nif, budget, seen, source, hc);
    }
    if (source->found >= hc && source->swept_hosts < (uint32_t)hc)
        source->truncated = true;
    free(seen);
    ESP_LOGI(TAG, "ARP: %d hosts", source->found);
}

// ---- mDNS ------------------------------------------------------------------

static const char *s_mdns_services[][2] = {
    {"_hap",         "_tcp"},
    {"_googlecast",  "_tcp"},
    {"_airplay",     "_tcp"},
    {"_raop",        "_tcp"},
    {"_sonos",       "_tcp"},
    {"_matter",      "_tcp"},
    {"_hue",         "_tcp"},
    {"_http",        "_tcp"},
    {"_meshcop",     "_udp"},
    {"_smb",         "_tcp"},
};
#define NUM_MDNS_SERVICES (sizeof(s_mdns_services) / sizeof(s_mdns_services[0]))

static int dns_encode_name(const char *name, uint8_t *buf, int buf_len) {
    int pos = 0;
    const char *p = name;
    while (*p) {
        const char *dot = strchr(p, '.');
        int ll = dot ? (int)(dot - p) : (int)strlen(p);
        if (ll > 63 || pos + 1 + ll >= buf_len) return -1;
        buf[pos++] = (uint8_t)ll;
        memcpy(&buf[pos], p, ll);
        pos += ll;
        p += ll + (dot ? 1 : 0);
    }
    if (pos + 1 > buf_len) return -1;
    buf[pos++] = 0;
    return pos;
}

static int dns_decode_name(const uint8_t *pkt, int pkt_len, int pos,
                           char *out, int out_len) {
    int out_pos = 0, start = pos, jumped = 0, bytes = 0, max = 20;
    while (pos < pkt_len && max-- > 0) {
        uint8_t len = pkt[pos];
        if (len == 0) { if (!jumped) bytes = pos - start + 1; break; }
        if ((len & 0xC0) == 0xC0) {
            if (pos + 1 >= pkt_len) return -1;
            if (!jumped) bytes = pos - start + 2;
            pos = ((len & 0x3F) << 8) | pkt[pos + 1];
            jumped = 1; continue;
        }
        pos++;
        if (pos + len > pkt_len) return -1;
        if (out_pos > 0 && out_pos < out_len - 1) out[out_pos++] = '.';
        int c = len;
        if (out_pos + c >= out_len) c = out_len - out_pos - 1;
        if (c > 0) { memcpy(out + out_pos, pkt + pos, c); out_pos += c; }
        pos += len;
    }
    out[out_pos] = '\0';
    return bytes > 0 ? bytes : -1;
}

typedef struct {
    char instance[96];
    char hostname[64];
    char ip_str[16];
    uint16_t port;
    char txt[256];
} mdns_entry_t;

// An A record: which host has which address. When the listen ends, an
// instance gets the address of the host its SRV record names, whichever of
// the two records came first.
typedef struct {
    char name[64];
    char ip_str[16];
} mdns_addr_t;
#define MAX_MDNS_ADDRS 128

// DNS names match case-insensitively, with or without the root's trailing dot.
static bool dns_name_eq(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    if (la && a[la - 1] == '.') la--;
    if (lb && b[lb - 1] == '.') lb--;
    return la == lb && strncasecmp(a, b, la) == 0;
}

static int mdns_find_or_add(mdns_entry_t *arr, int *count, int max,
                            const char *instance) {
    for (int i = 0; i < *count; i++)
        if (dns_name_eq(arr[i].instance, instance)) return i;
    if (*count >= max) return -1;
    int idx = (*count)++;
    memset(&arr[idx], 0, sizeof(arr[idx]));
    snprintf(arr[idx].instance, sizeof(arr[idx].instance), "%s", instance);
    return idx;
}

static void parse_mdns_pkt(const uint8_t *pkt, int len,
                           mdns_entry_t *found, int *fc, int max,
                           mdns_addr_t *addrs, int *ac, bool *addrs_full) {
    if (len < 12) return;
    int qdcount = (pkt[4] << 8) | pkt[5];
    int total = ((pkt[6] << 8) | pkt[7]) + ((pkt[8] << 8) | pkt[9]) +
                ((pkt[10] << 8) | pkt[11]);

    int pos = 12;
    for (int q = 0; q < qdcount && pos < len; q++) {
        char skip[128];
        int n = dns_decode_name(pkt, len, pos, skip, sizeof(skip));
        if (n < 0) return;
        pos += n + 4;
    }

    for (int rr = 0; rr < total && pos < len; rr++) {
        char rname[128];
        int nb = dns_decode_name(pkt, len, pos, rname, sizeof(rname));
        if (nb < 0) break;
        pos += nb;
        if (pos + 10 > len) break;
        uint16_t rtype = (pkt[pos] << 8) | pkt[pos + 1];
        uint16_t rdlen = (pkt[pos + 8] << 8) | pkt[pos + 9];
        pos += 10;
        if (pos + rdlen > len) break;

        if (rtype == 12) {
            char inst[96];
            if (dns_decode_name(pkt, len, pos, inst, sizeof(inst)) > 0)
                mdns_find_or_add(found, fc, max, inst);
        } else if (rtype == 33 && rdlen >= 6) {
            uint16_t port = (pkt[pos + 4] << 8) | pkt[pos + 5];
            // Decoded as long as rname, so a target too long for the host
            // table is dropped instead of cut to another host's name.
            char target[sizeof(rname)];
            if (dns_decode_name(pkt, len, pos + 6, target, sizeof(target)) > 0) {
                for (int i = 0; i < *fc; i++) {
                    if (dns_name_eq(rname, found[i].instance)) {
                        found[i].port = port;
                        if (!found[i].hostname[0] && strlen(target) < sizeof(found[i].hostname))
                            memcpy(found[i].hostname, target, strlen(target) + 1);
                        break;
                    }
                }
            }
        } else if (rtype == 1 && rdlen == 4) {
            // A name too long for an SRV target's buffer couldn't match one.
            size_t nl = strlen(rname);
            int a = 0;
            while (a < *ac && !dns_name_eq(addrs[a].name, rname)) a++;
            if (a == *ac && nl < sizeof(addrs[a].name)) {
                if (a < MAX_MDNS_ADDRS) {
                    memcpy(addrs[a].name, rname, nl + 1);   // a host's first address
                    snprintf(addrs[a].ip_str, sizeof(addrs[a].ip_str), "%d.%d.%d.%d",
                             pkt[pos], pkt[pos+1], pkt[pos+2], pkt[pos+3]);
                    (*ac)++;
                } else {
                    *addrs_full = true;   // an instance may miss its address
                }
            }
        } else if (rtype == 16) {
            int tp = 0; char tb[256]; int to = 0;
            while (tp < rdlen) {
                uint8_t tl = pkt[pos + tp]; tp++;
                if (tp + tl > rdlen) break;
                if (to > 0 && to < (int)sizeof(tb) - 1) tb[to++] = ';';
                int c = tl;
                if (to + c >= (int)sizeof(tb)) c = sizeof(tb) - to - 1;
                if (c > 0) { memcpy(tb + to, pkt + pos + tp, c); to += c; }
                tp += tl;
            }
            tb[to] = '\0';
            for (int i = 0; i < *fc; i++) {
                if (!found[i].txt[0] && dns_name_eq(rname, found[i].instance)) {
                    snprintf(found[i].txt, sizeof(found[i].txt), "%s", tb);
                    break;
                }
            }
        }
        pos += rdlen;
    }
}

static void mdns_probe(discovery_budget_t *budget, discovery_source_t *source) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) { source->error = "socket_failed"; return; }

    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in ba = { .sin_family = AF_INET, .sin_port = htons(MDNS_PORT),
                              .sin_addr.s_addr = htonl(INADDR_ANY) };
    // Bound elsewhere (a relay holds 5353), answers to its queries still come
    // by unicast to the port they went from.
    bool bound = bind(sock, (struct sockaddr *)&ba, sizeof(ba)) == 0;

    struct ip_mreq mr = { .imr_multiaddr.s_addr = inet_addr("224.0.0.251"),
                          .imr_interface.s_addr = htonl(INADDR_ANY) };
    if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mr, sizeof(mr)) != 0
        && bound && budget->heard)
        budget->heard_partial = true;   // multicast answers to its queries miss it

    struct sockaddr_in dest = { .sin_family = AF_INET, .sin_port = htons(MDNS_PORT),
                                .sin_addr.s_addr = inet_addr("224.0.0.251") };
    uint8_t qbuf[256];

    ESP_LOGI(TAG, "mDNS: querying %d service types", (int)NUM_MDNS_SERVICES);
    for (int s = 0; s < (int)NUM_MDNS_SERVICES; s++) {
        memset(qbuf, 0, 12);
        qbuf[5] = 1;
        char qn[64];
        snprintf(qn, sizeof(qn), "%s.%s.local",
                 s_mdns_services[s][0], s_mdns_services[s][1]);
        int nl = dns_encode_name(qn, qbuf + 12, sizeof(qbuf) - 16);
        if (nl < 0) continue;
        int off = 12 + nl;
        qbuf[off] = 0; qbuf[off + 1] = 12;
        qbuf[off + 2] = 0x80; qbuf[off + 3] = 1;
        if (sendto(sock, qbuf, off + 4, 0, (struct sockaddr *)&dest, sizeof(dest)) < 0)
            source->truncated = true;   // a query not sent: not complete
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    // One allocation for both tables: alone, the 10 KB address table is small
    // enough to come from internal RAM (CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL).
    mdns_entry_t *found = calloc(1, MAX_MDNS * sizeof(mdns_entry_t)
                                    + MAX_MDNS_ADDRS * sizeof(mdns_addr_t));
    if (!found) { source->error = "out_of_memory"; close(sock); return; }
    mdns_addr_t *addrs = (mdns_addr_t *)(found + MAX_MDNS);
    int fc = 0, ac = 0;
    bool addrs_full = false;

    struct timeval tv = { .tv_sec = MDNS_LISTEN_MS / 1000,
                          .tv_usec = (MDNS_LISTEN_MS % 1000) * 1000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t rbuf[RECV_BUF];
    int64_t deadline = esp_timer_get_time() + MDNS_LISTEN_MS * 1000LL;
    while (esp_timer_get_time() < deadline && fc < MAX_MDNS) {
        struct sockaddr_in from; socklen_t fl = sizeof(from);
        int n = recvfrom(sock, rbuf, sizeof(rbuf), 0, (struct sockaddr *)&from, &fl);
        if (n < 0) break;
        note_heard(budget, from.sin_addr.s_addr);
        if (n == 0) continue;   // an empty datagram, not the end of the answers
        parse_mdns_pkt(rbuf, n, found, &fc, MAX_MDNS, addrs, &ac, &addrs_full);
    }

    // An instance without an SRV record, or whose host sent no A record,
    // keeps no address rather than another host's.
    for (int i = 0; i < fc; i++)
        for (int a = 0; a < ac && found[i].hostname[0]; a++)
            if (dns_name_eq(found[i].hostname, addrs[a].name)) {
                snprintf(found[i].ip_str, sizeof(found[i].ip_str), "%s", addrs[a].ip_str);
                break;
            }

    source->truncated |= fc >= MAX_MDNS || addrs_full;
    for (int i = 0; i < fc; i++) {
        sanitize_ascii(found[i].instance);
        sanitize_ascii(found[i].hostname);
        sanitize_ascii(found[i].txt);
        cJSON *e = cJSON_CreateObject();
        observation_string(&e, "source", "mdns");
        observation_string(&e, "instance", found[i].instance);
        if (found[i].hostname[0])
            observation_string(&e, "hostname", found[i].hostname);
        if (found[i].ip_str[0])
            observation_string(&e, "ip", found[i].ip_str);
        if (e && found[i].port && !cJSON_AddNumberToObject(e, "port", found[i].port)) {
            cJSON_Delete(e);
            e = NULL;
        }
        if (found[i].txt[0])
            observation_string(&e, "txt", found[i].txt);
        if (!append_observation(budget, source, e)) break;
    }

    free(found);
    setsockopt(sock, IPPROTO_IP, IP_DROP_MEMBERSHIP, &mr, sizeof(mr));
    close(sock);
    ESP_LOGI(TAG, "mDNS: %d unique services", fc);
}

// Copies the value of buf's "key: value" line (key case-insensitive, at least
// one byte after the colon), sanitized, into out. False without one.
static bool header_value(char *buf, const char *key, char *out, size_t cap) {
    size_t kl = strlen(key);
    for (char *line = buf; *line; ) {
        char *eol = strstr(line, "\r\n");
        if (!eol) eol = line + strlen(line);
        if ((size_t)(eol - line) > kl + 1
            && line[kl] == ':'
            && strncasecmp(line, key, kl) == 0) {
            char *val = line + kl + 1;
            while (*val == ' ') val++;
            char save = *eol; *eol = '\0';
            int len = snprintf(out, cap, "%s", val);
            *eol = save;
            if (len >= (int)cap)
                ESP_LOGW(TAG, "%s value truncated from %d to %u bytes",
                         key, len, (unsigned)(cap - 1));
            sanitize_ascii(out);
            return true;
        }
        if (!*eol) break;
        line = eol + 2;
    }
    return false;
}

// ---- SSDP ------------------------------------------------------------------

static void ssdp_probe(discovery_budget_t *budget, discovery_source_t *source) {
    static const char msearch[] =
        "M-SEARCH * HTTP/1.1\r\n"
        "HOST: 239.255.255.250:1900\r\n"
        "MAN: \"ssdp:discover\"\r\n"
        "MX: 3\r\n"
        "ST: ssdp:all\r\n"
        "\r\n";

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) { source->error = "socket_failed"; return; }

    struct sockaddr_in dest = { .sin_family = AF_INET, .sin_port = htons(SSDP_PORT),
                                .sin_addr.s_addr = inet_addr("239.255.255.250") };
    if (sendto(sock, msearch, strlen(msearch), 0,
               (struct sockaddr *)&dest, sizeof(dest)) < 0)
        source->truncated = true;

    struct timeval tv = { .tv_sec = SSDP_LISTEN_MS / 1000,
                          .tv_usec = (SSDP_LISTEN_MS % 1000) * 1000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    char buf[RECV_BUF];
    int count = 0;
    int64_t deadline = esp_timer_get_time() + SSDP_LISTEN_MS * 1000LL;

    while (esp_timer_get_time() < deadline && count < MAX_SSDP) {
        struct sockaddr_in from; socklen_t fl = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf) - 1, 0,
                         (struct sockaddr *)&from, &fl);
        if (n < 0) break;
        note_heard(budget, from.sin_addr.s_addr);
        if (n == 0) continue;
        buf[n] = '\0';
        char ib[16]; ip_str(from.sin_addr.s_addr, ib, sizeof(ib));
        cJSON *e = cJSON_CreateObject();
        observation_string(&e, "source", "ssdp");
        observation_string(&e, "ip", ib);

        static const char *hdrs[] = {"LOCATION","ST","USN","SERVER",NULL};
        for (int h = 0; hdrs[h]; h++) {
            // A sanitized copy, so later header scans keep their CRLFs.
            char value[DISCOVERY_VALUE_BUF];
            if (!header_value(buf, hdrs[h], value, sizeof(value))) continue;
            char lk[16];
            size_t kl = strlen(hdrs[h]);
            for (size_t i = 0; i < kl; i++)
                lk[i] = (hdrs[h][i] >= 'A' && hdrs[h][i] <= 'Z')
                         ? hdrs[h][i] + 32 : hdrs[h][i];
            lk[kl] = '\0';
            observation_string(&e, lk, value);
        }

        if (!append_observation(budget, source, e)) break;
        count++;
    }

    source->found = count;
    source->truncated |= count >= MAX_SSDP;
    close(sock);
    ESP_LOGI(TAG, "SSDP: %d responses", count);
}

// ---- WS-Discovery ----------------------------------------------------------

static void wsd_probe(discovery_budget_t *budget, discovery_source_t *source) {
    static const char probe[] =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<soap:Envelope xmlns:soap=\"http://www.w3.org/2003/05/soap-envelope\""
        " xmlns:wsa=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\""
        " xmlns:wsd=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\">"
        "<soap:Header>"
        "<wsa:Action>http://schemas.xmlsoap.org/ws/2005/04/discovery/Probe"
        "</wsa:Action>"
        "<wsa:MessageID>urn:uuid:00000001-0001-0001-0001-000000000001"
        "</wsa:MessageID>"
        "<wsa:To>urn:schemas-xmlsoap-org:ws:2005:04:discovery</wsa:To>"
        "</soap:Header>"
        "<soap:Body><wsd:Probe/></soap:Body>"
        "</soap:Envelope>";

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) { source->error = "socket_failed"; return; }

    struct sockaddr_in dest = { .sin_family = AF_INET, .sin_port = htons(WSD_PORT),
                                .sin_addr.s_addr = inet_addr("239.255.255.250") };
    if (sendto(sock, probe, strlen(probe), 0,
               (struct sockaddr *)&dest, sizeof(dest)) < 0)
        source->truncated = true;

    struct timeval tv = { .tv_sec = WSD_LISTEN_MS / 1000,
                          .tv_usec = (WSD_LISTEN_MS % 1000) * 1000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    char buf[RECV_BUF];
    int count = 0;
    int64_t deadline = esp_timer_get_time() + WSD_LISTEN_MS * 1000LL;

    while (esp_timer_get_time() < deadline && count < MAX_WSD) {
        struct sockaddr_in from; socklen_t fl = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf) - 1, 0,
                         (struct sockaddr *)&from, &fl);
        if (n < 0) break;
        note_heard(budget, from.sin_addr.s_addr);
        if (n == 0) continue;
        buf[n] = '\0';
        char ib[16]; ip_str(from.sin_addr.s_addr, ib, sizeof(ib));
        cJSON *e = cJSON_CreateObject();
        observation_string(&e, "source", "wsd");
        observation_string(&e, "ip", ib);
        char *types = strstr(buf, "<wsd:Types>");
        if (types) {
            types += 11;
            char *te = strstr(types, "</wsd:Types>");
            if (te) {
                *te = '\0';
                char value[DISCOVERY_VALUE_BUF];
                int len = snprintf(value, sizeof(value), "%s", types);
                if (len >= (int)sizeof(value))
                    ESP_LOGW(TAG, "WSD types value truncated from %d to %u bytes",
                             len, (unsigned)(sizeof(value) - 1));
                sanitize_ascii(value);
                observation_string(&e, "types", value);
            }
        }
        if (!append_observation(budget, source, e)) break;
        count++;
    }

    source->found = count;
    source->truncated |= count >= MAX_WSD;
    close(sock);
    ESP_LOGI(TAG, "WSD: %d responses", count);
}

// ---- SNMP ------------------------------------------------------------------

static bool snmp_extract_value(const uint8_t *data, int len,
                               char *out, int out_len) {
    static const uint8_t oid[] = {0x2b,0x06,0x01,0x02,0x01,0x01,0x01,0x00};
    for (int i = 0; i + (int)sizeof(oid) + 2 < len; i++) {
        if (data[i] == 0x06 && data[i+1] == sizeof(oid) &&
            memcmp(data + i + 2, oid, sizeof(oid)) == 0) {
            int vp = i + 2 + sizeof(oid);
            if (vp + 2 > len) return false;
            vp++;
            int vl = data[vp++];
            if (vl & 0x80) {
                int n = vl & 0x7F;
                if (n > 2 || vp + n > len) return false;
                vl = 0;
                for (int j = 0; j < n; j++) vl = (vl << 8) | data[vp++];
            }
            int c = vl < out_len - 1 ? vl : out_len - 1;
            if (vp + c > len) c = len - vp;
            if (c <= 0) return false;
            memcpy(out, data + vp, c);
            out[c] = '\0';
            return true;
        }
    }
    return false;
}

static void snmp_probe(discovery_budget_t *budget, esp_netif_t *netif,
                          discovery_source_t *source) {
    static const uint8_t pkt[] = {
        0x30,0x26,
          0x02,0x01,0x00,
          0x04,0x06,'p','u','b','l','i','c',
          0xa0,0x19,
            0x02,0x01,0x01,
            0x02,0x01,0x00,
            0x02,0x01,0x00,
            0x30,0x0e,
              0x30,0x0c,
                0x06,0x08,0x2b,0x06,0x01,0x02,0x01,0x01,0x01,0x00,
                0x05,0x00
    };

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) { source->error = "socket_failed"; return; }
    int bc = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &bc, sizeof(bc));

    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(netif, &info) != ESP_OK) {
        source->error = "ip_info_failed"; close(sock); return;
    }
    uint32_t baddr = info.ip.addr | ~info.netmask.addr;

    struct sockaddr_in dest = { .sin_family = AF_INET, .sin_port = htons(161),
                                .sin_addr.s_addr = baddr };
    if (sendto(sock, pkt, sizeof(pkt), 0, (struct sockaddr *)&dest, sizeof(dest)) < 0)
        source->truncated = true;

    struct timeval tv = { .tv_sec = SNMP_LISTEN_MS / 1000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t buf[RECV_BUF];
    int count = 0;
    int64_t deadline = esp_timer_get_time() + SNMP_LISTEN_MS * 1000LL;

    while (esp_timer_get_time() < deadline && count < MAX_SNMP) {
        struct sockaddr_in from; socklen_t fl = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf), 0,
                         (struct sockaddr *)&from, &fl);
        if (n < 0) break;
        note_heard(budget, from.sin_addr.s_addr);
        if (n == 0) continue;
        char val[256];
        if (snmp_extract_value(buf, n, val, sizeof(val))) {
            sanitize_ascii(val);
            char ib[16]; ip_str(from.sin_addr.s_addr, ib, sizeof(ib));
            cJSON *e = cJSON_CreateObject();
            observation_string(&e, "source", "snmp");
            observation_string(&e, "ip", ib);
            observation_string(&e, "raw", val);
            if (!append_observation(budget, source, e)) break;
            count++;
        }
    }

    source->found = count;
    source->truncated |= count >= MAX_SNMP;
    close(sock);
    ESP_LOGI(TAG, "SNMP: %d responses", count);
}

// ---- NetBIOS ---------------------------------------------------------------

static void netbios_probe(discovery_budget_t *budget, esp_netif_t *netif,
                          discovery_source_t *source) {
    static const uint8_t query[] = {
        0x82,0xb2,
        0x00,0x00,
        0x00,0x01, 0x00,0x00, 0x00,0x00, 0x00,0x00,
        0x20,
        'C','K','A','A','A','A','A','A','A','A','A','A','A','A','A','A',
        'A','A','A','A','A','A','A','A','A','A','A','A','A','A','A','A',
        0x00,
        0x00,0x21,
        0x00,0x01,
    };

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) { source->error = "socket_failed"; return; }
    int bc = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &bc, sizeof(bc));

    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(netif, &info) != ESP_OK) {
        source->error = "ip_info_failed"; close(sock); return;
    }
    uint32_t baddr = info.ip.addr | ~info.netmask.addr;

    struct sockaddr_in dest = { .sin_family = AF_INET, .sin_port = htons(137),
                                .sin_addr.s_addr = baddr };
    if (sendto(sock, query, sizeof(query), 0,
               (struct sockaddr *)&dest, sizeof(dest)) < 0)
        source->truncated = true;

    struct timeval tv = { .tv_sec = NETBIOS_LISTEN_MS / 1000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t buf[RECV_BUF];
    int count = 0;
    int64_t deadline = esp_timer_get_time() + NETBIOS_LISTEN_MS * 1000LL;

    while (esp_timer_get_time() < deadline && count < MAX_NETBIOS) {
        struct sockaddr_in from; socklen_t fl = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf), 0,
                         (struct sockaddr *)&from, &fl);
        if (n < 0) break;
        note_heard(budget, from.sin_addr.s_addr);
        if (n == 0) continue;

        int pos = 12;
        while (pos < n) {
            uint8_t lbl = buf[pos];
            if (lbl == 0) { pos++; break; }
            if ((lbl & 0xC0) == 0xC0) { pos += 2; break; }
            pos += 1 + lbl;
        }

        if (pos + 10 > n) continue;
        uint16_t rdlen = (buf[pos + 8] << 8) | buf[pos + 9];
        pos += 10;
        if (pos + rdlen > n || rdlen < 1) continue;

        int nn = buf[pos++];
        char nb[256] = "";
        int bo = 0;
        for (int i = 0; i < nn && pos + 18 <= n; i++) {
            char nm[16];
            memcpy(nm, buf + pos, 15);
            nm[15] = '\0';
            for (int j = 14; j >= 0 && nm[j] == ' '; j--) nm[j] = '\0';
            uint8_t sfx = buf[pos + 15];
            pos += 18;
            if (nm[0] && bo < (int)sizeof(nb) - 24) {
                if (bo > 0) nb[bo++] = ';';
                bo += snprintf(nb + bo, sizeof(nb) - bo, "%s<%02x>", nm, sfx);
            }
        }
        if (!nb[0]) continue;
        sanitize_ascii(nb);
        if (!nb[0]) continue;

        char ib[16]; ip_str(from.sin_addr.s_addr, ib, sizeof(ib));
        cJSON *e = cJSON_CreateObject();
        observation_string(&e, "source", "netbios");
        observation_string(&e, "ip", ib);
        observation_string(&e, "raw", nb);
        if (!append_observation(budget, source, e)) break;
        count++;
    }

    source->found = count;
    source->truncated |= count >= MAX_NETBIOS;
    close(sock);
    ESP_LOGI(TAG, "NetBIOS: %d responses", count);
}

// ---- Vendor probes ---------------------------------------------------------
// The read-only "who's there" packet each vendor's own app sends, answered by
// unicast to the asking port (Govee answers on 4002), and the broadcasts Tuya
// (6666 plain, 6667 and 7000 encrypted) and Bambu (2021) devices send by
// themselves, heard during the same window.

#define GOVEE_REPLY_PORT 4002

static const uint16_t s_vendor_listen[] = {0, GOVEE_REPLY_PORT, 6666, 6667, 7000, 2021};
#define VENDOR_SOCKETS (sizeof(s_vendor_listen) / sizeof(s_vendor_listen[0]))

enum { V_KASA, V_TPLINK, V_GOVEE, V_MIIO, V_WIZ, V_LIFX, V_YEELIGHT, V_CS2, V_TUYA, V_BAMBU };
static const char *const s_vendor_names[] = {
    "kasa", "tplink", "govee", "miio", "wiz", "lifx", "yeelight", "cs2", "tuya", "bambu",
};

typedef struct {
    uint32_t ip;
    int kind;
} vendor_seen_t;

// Kasa's cipher: each byte XORed with the previous ciphertext byte, from 171.
static void kasa_xor(uint8_t *b, int n, bool encrypt) {
    uint8_t key = 171;
    for (int i = 0; i < n; i++) {
        uint8_t in = b[i];
        b[i] = key ^ in;
        key = encrypt ? b[i] : in;
    }
}

// False when the packet couldn't be sent.
static bool vendor_send(int sock, const char *group, uint16_t port,
                        const void *msg, size_t len) {
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(port),
                              .sin_addr.s_addr = group ? inet_addr(group)
                                                       : htonl(INADDR_BROADCAST) };
    return sendto(sock, msg, len, 0, (struct sockaddr *)&to, sizeof(to)) >= 0;
}

// Sends every query; false when any of them couldn't be sent.
static bool vendor_queries(int sock, int govee) {
    bool sent = true;
    uint8_t kasa[] = "{\"system\":{\"get_sysinfo\":{}}}";
    kasa_xor(kasa, sizeof(kasa) - 1, true);
    sent &= vendor_send(sock, NULL, 9999, kasa, sizeof(kasa) - 1);
    static const uint8_t tdp[16] = {0x02, 0, 0, 0x01, 0, 0, 0, 0,
                                    0, 0, 0, 0, 0x46, 0x3c, 0xb5, 0xd3};
    sent &= vendor_send(sock, NULL, 20002, tdp, sizeof(tdp));
    static const char govee_scan[] =
        "{\"msg\":{\"cmd\":\"scan\",\"data\":{\"account_topic\":\"reserve\"}}}";
    sent &= vendor_send(govee, "239.255.255.250", 4001, govee_scan, strlen(govee_scan));
    uint8_t miio[32];
    memset(miio, 0xff, sizeof(miio));
    miio[0] = 0x21; miio[1] = 0x31; miio[2] = 0x00; miio[3] = 0x20;
    sent &= vendor_send(sock, NULL, 54321, miio, sizeof(miio));
    static const char wiz[] = "{\"method\":\"getPilot\",\"params\":{}}";
    sent &= vendor_send(sock, NULL, 38899, wiz, strlen(wiz));
    // LIFX GetService: 36-byte header, tagged, protocol 1024, message type 2.
    static const uint8_t lifx[36] = {36, 0, 0x00, 0x34, 'M', 'u', 's', 'e', [32] = 2};
    sent &= vendor_send(sock, NULL, 56700, lifx, sizeof(lifx));
    static const char yeelight[] =
        "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1982\r\n"
        "MAN: \"ssdp:discover\"\r\nST: wifi_bulb\r\n";
    sent &= vendor_send(sock, "239.255.255.250", 1982, yeelight, strlen(yeelight));
    static const uint8_t cs2[4] = {0xf1, 0x30, 0x00, 0x00};   // CS2/PPPP LAN search
    sent &= vendor_send(sock, NULL, 32108, cs2, sizeof(cs2));
    return sent;
}

// Which vendor sent b, from the port it came to and its bytes. -1: none.
static int vendor_kind(const uint8_t *b, int n, uint16_t listen) {
    if (listen == GOVEE_REPLY_PORT) return V_GOVEE;
    if (listen == 2021) return V_BAMBU;
    if (listen) return V_TUYA;
    if (n >= 12 && b[0] == 0xf1 && b[1] == 0x41 && (b[2] << 8 | b[3]) == n - 4) return V_CS2;
    if (n >= 32 && b[0] == 0x21 && b[1] == 0x31 && (b[2] << 8 | b[3]) == n) return V_MIIO;
    if (n >= 36 && (b[0] | b[1] << 8) == n && ((b[2] | b[3] << 8) & 0x0fff) == 1024
        && b[32] == 3 && b[33] == 0) return V_LIFX;
    if (n > 16 && b[0] == 0x02 && b[16] == '{') return V_TPLINK;
    if (b[0] == '{') return strstr((const char *)b, "\"getPilot\"") ? V_WIZ : -1;
    if (b[0] == 0xd0) return V_KASA;   // '{' under Kasa's cipher
    if (!strncmp((const char *)b, "HTTP/1.1 200", 12) && strstr((const char *)b, "yeelight://"))
        return V_YEELIGHT;
    return -1;
}

static void vendor_json_field(cJSON **e, const cJSON *obj, const char *key, const char *as) {
    const cJSON *v = cJSON_GetObjectItem(obj, key);
    if (!*e || !cJSON_IsString(v) || cJSON_GetObjectItem(*e, as)) return;
    char value[DISCOVERY_VALUE_BUF];
    snprintf(value, sizeof(value), "%s", v->valuestring);
    if (sanitize_ascii(value)) observation_string(e, as, value);
}

static void vendor_header_field(cJSON **e, char *text, const char *key, const char *as) {
    char value[DISCOVERY_VALUE_BUF];
    if (header_value(text, key, value, sizeof(value)) && value[0])
        observation_string(e, as, value);
}

static void vendor_hex_field(cJSON **e, const uint8_t *b, int n, const char *as) {
    char hex[13];
    for (int i = 0; i < n && i < 6; i++) snprintf(hex + 2 * i, 3, "%02x", b[i]);
    observation_string(e, as, hex);
}

// The JSON object between b's first '{' and last '}'.
static cJSON *vendor_json(const uint8_t *b, int n) {
    const char *start = memchr(b, '{', n), *end = NULL;
    for (int i = n - 1; i >= 0 && !end; i--) if (b[i] == '}') end = (const char *)b + i;
    return start && end > start ? cJSON_ParseWithLength(start, end - start + 1) : NULL;
}

static cJSON *vendor_observation(uint8_t *b, int n, int kind, uint16_t listen,
                                 uint32_t ip, uint16_t from_port) {
    cJSON *e = cJSON_CreateObject();
    observation_string(&e, "source", "vendor");
    observation_string(&e, "protocol", s_vendor_names[kind]);
    char ib[16]; ip_str(ip, ib, sizeof(ib));
    observation_string(&e, "ip", ib);
    if (e && !cJSON_AddNumberToObject(e, "port", listen ? listen : from_port)) {
        cJSON_Delete(e);
        return NULL;
    }
    cJSON *j = NULL;
    switch (kind) {
        case V_KASA: {
            kasa_xor(b, n, false);
            j = vendor_json(b, n);
            const cJSON *info = cJSON_GetObjectItem(cJSON_GetObjectItem(j, "system"), "get_sysinfo");
            vendor_json_field(&e, info, "model", "model");
            vendor_json_field(&e, info, "alias", "name");
            vendor_json_field(&e, info, "mac", "mac");
            vendor_json_field(&e, info, "mic_mac", "mac");
            break;
        }
        case V_TPLINK: {
            j = vendor_json(b + 16, n - 16);
            const cJSON *r = cJSON_GetObjectItem(j, "result");
            vendor_json_field(&e, r, "device_model", "model");
            vendor_json_field(&e, r, "device_type", "type");
            vendor_json_field(&e, r, "mac", "mac");
            vendor_json_field(&e, cJSON_GetObjectItem(r, "mgt_encrypt_schm"), "encrypt_type", "encryption");
            break;
        }
        case V_GOVEE: {
            j = vendor_json(b, n);
            const cJSON *d = cJSON_GetObjectItem(cJSON_GetObjectItem(j, "msg"), "data");
            vendor_json_field(&e, d, "sku", "model");
            vendor_json_field(&e, d, "device", "id");
            break;
        }
        case V_MIIO:
            vendor_hex_field(&e, b + 8, 4, "id");   // the device id; never the token field
            break;
        case V_WIZ:
            j = vendor_json(b, n);
            vendor_json_field(&e, cJSON_GetObjectItem(j, "result"), "mac", "mac");
            break;
        case V_LIFX:
            vendor_hex_field(&e, b + 8, 6, "mac");
            break;
        case V_YEELIGHT:
            vendor_header_field(&e, (char *)b, "model", "model");
            vendor_header_field(&e, (char *)b, "id", "id");
            vendor_header_field(&e, (char *)b, "name", "name");
            break;
        case V_CS2: {
            // The device id's vendor prefix; its serial and check code stay out.
            char prefix[9];
            snprintf(prefix, sizeof(prefix), "%.8s", (const char *)b + 4);
            if (sanitize_ascii(prefix)) observation_string(&e, "id_prefix", prefix);
            break;
        }
        case V_TUYA:
            if (listen == 6666 && n > 20 && !memcmp(b, "\x00\x00\x55\xaa", 4)) {
                j = vendor_json(b + 16, n - 16);
                vendor_json_field(&e, j, "gwId", "id");
                vendor_json_field(&e, j, "productKey", "model");
                vendor_json_field(&e, j, "version", "version");
            }
            break;
        case V_BAMBU:
            vendor_header_field(&e, (char *)b, "DevModel.bambu.com", "model");
            vendor_header_field(&e, (char *)b, "DevName.bambu.com", "name");
            break;
    }
    cJSON_Delete(j);
    return e;
}

// Whether an answer is the protocol's own: the plaintext ones must carry one
// of its fields; Tuya's encrypted broadcasts (6667, 7000) a whole frame, from
// its prefix to its suffix (55AA, or 6699 from newer devices). Anything else
// is ignored, so it can't take a device's one observation or the cap.
static bool vendor_answer_valid(const cJSON *e, int kind, uint16_t listen,
                                const uint8_t *b, int n) {
    if (kind == V_MIIO || kind == V_LIFX || kind == V_CS2) return true;   // vendor_kind() checked the frame
    if (kind == V_TUYA && listen != 6666)
        return n >= 28 && ((!memcmp(b, "\x00\x00\x55\xaa", 4)
                            && !memcmp(b + n - 4, "\x00\x00\xaa\x55", 4))
                           || (!memcmp(b, "\x00\x00\x66\x99", 4)
                               && !memcmp(b + n - 4, "\x00\x00\x99\x66", 4)));
    return cJSON_GetArraySize(e) > 4;   // a field past source, protocol, ip, port
}

static void vendor_probe(discovery_budget_t *budget, discovery_source_t *source) {
    int socks[VENDOR_SOCKETS];
    for (size_t i = 0; i < VENDOR_SOCKETS; i++) {
        socks[i] = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socks[i] < 0) {
            source->truncated = true;   // not heard this time, so not complete
            continue;
        }
        int on = 1;
        setsockopt(socks[i], SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
        setsockopt(socks[i], SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
        struct sockaddr_in ba = { .sin_family = AF_INET,
                                  .sin_port = htons(s_vendor_listen[i]),
                                  .sin_addr.s_addr = htonl(INADDR_ANY) };
        if (s_vendor_listen[i] && bind(socks[i], (struct sockaddr *)&ba, sizeof(ba)) != 0) {
            close(socks[i]);   // a port in use on the device: not heard this time
            socks[i] = -1;
            source->truncated = true;
        }
    }
    uint8_t *buf = malloc(RECV_BUF + 1);
    vendor_seen_t *seen = malloc(MAX_VENDOR * sizeof(vendor_seen_t));
    if (socks[0] < 0) source->error = "socket_failed";
    else if (!buf || !seen) source->error = "out_of_memory";
    else if (!vendor_queries(socks[0], socks[1] >= 0 ? socks[1] : socks[0]))
        source->truncated = true;   // a query not sent: not complete

    int64_t start = esp_timer_get_time(), deadline = start + VENDOR_LISTEN_MS * 1000LL;
    bool resent = false;
    while (!source->error && !budget->exhausted && source->found < MAX_VENDOR
           && esp_timer_get_time() < deadline) {
        if (!resent && esp_timer_get_time() >= start + VENDOR_RESEND_MS * 1000LL) {
            if (!vendor_queries(socks[0], socks[1] >= 0 ? socks[1] : socks[0]))
                source->truncated = true;
            resent = true;
        }
        bool heard = false;
        for (size_t i = 0; i < VENDOR_SOCKETS && !budget->exhausted
             && source->found < MAX_VENDOR; i++) {
            if (socks[i] < 0) continue;
            struct sockaddr_in from; socklen_t fl = sizeof(from);
            int n = recvfrom(socks[i], buf, RECV_BUF, MSG_DONTWAIT,
                             (struct sockaddr *)&from, &fl);
            if (n < 0) continue;
            note_heard(budget, from.sin_addr.s_addr);
            if (n == 0) continue;
            heard = true;
            buf[n] = '\0';
            int kind = vendor_kind(buf, n, s_vendor_listen[i]);
            bool dup = kind < 0;
            for (int k = 0; k < source->found && !dup; k++)
                dup = seen[k].ip == from.sin_addr.s_addr && seen[k].kind == kind;
            if (dup) continue;
            cJSON *e = vendor_observation(buf, n, kind, s_vendor_listen[i],
                                          from.sin_addr.s_addr, ntohs(from.sin_port));
            if (e && !vendor_answer_valid(e, kind, s_vendor_listen[i], buf, n)) {
                cJSON_Delete(e);
                continue;
            }
            seen[source->found] = (vendor_seen_t){ from.sin_addr.s_addr, kind };
            append_observation(budget, source, e);
        }
        if (!heard) vTaskDelay(pdMS_TO_TICKS(20));
    }
    source->truncated |= source->found >= MAX_VENDOR;
    for (size_t i = 0; i < VENDOR_SOCKETS; i++) if (socks[i] >= 0) close(socks[i]);
    free(buf);
    free(seen);
    ESP_LOGI(TAG, "vendor: %d devices", source->found);
}

// ---- Host hints ------------------------------------------------------------
// Added to ARP observations after every source has run, from what the scan saw:
// `maker` for an Espressif-made MAC, `silent` for a host where nothing but ARP
// answered and no scan socket received anything from it, `mac_batch` for MACs
// with one OUI close enough to be one product's batch.

// Espressif's MA-L blocks, from the IEEE registry (https://standards-oui.ieee.org/oui/oui.txt).
static const uint32_t s_espressif_oui[] = {
    0x004b12, 0x007007, 0x00fb4a, 0x048308, 0x04b247, 0x083a8d, 0x083af2, 0x089272,
    0x08a6f7, 0x08ad0a, 0x08b61f, 0x08d1f9, 0x08f9e0, 0x0c4ea0, 0x0c8b95, 0x0cb815,
    0x0cdc7e, 0x10003b, 0x10061c, 0x1020ba, 0x1051db, 0x10521c, 0x1091a8, 0x1097bd,
    0x10a85b, 0x10b41d, 0x10bda3, 0x140808, 0x142b2f, 0x14335c, 0x146393, 0x14c19f,
    0x188b0e, 0x18fe34, 0x1c2904, 0x1c6465, 0x1c6920, 0x1c8b84, 0x1c8f57, 0x1c9dc2,
    0x1cc3ab, 0x1cdbd4, 0x1ce4cb, 0x202565, 0x2043a8, 0x20500d, 0x206ef1, 0x209ba9,
    0x20d5c2, 0x20e5a4, 0x20e7c8, 0x240ac4, 0x244cab, 0x24587c, 0x2462ab, 0x246f28,
    0x24a160, 0x24b2de, 0x24d7eb, 0x24dcc3, 0x24ec4a, 0x2805a5, 0x28372f, 0x28562f,
    0x288485, 0x2c3ae8, 0x2cbcbb, 0x2cf432, 0x3030f9, 0x3076f5, 0x308398, 0x30aea4,
    0x30c6f7, 0x30c922, 0x30e8b9, 0x30eda0, 0x345f45, 0x348518, 0x34865d, 0x349454,
    0x34987a, 0x34ab95, 0x34b472, 0x34b7da, 0x34c934, 0x34cdb0, 0x38182b, 0x383e51,
    0x3844be, 0x3c0d0d, 0x3c0f02, 0x3c6105, 0x3c71bf, 0x3c8427, 0x3c8a1f, 0x3cdc75,
    0x3ce255, 0x3ce90e, 0x4022d8, 0x402bd6, 0x404cca, 0x409151, 0x40f520, 0x441793,
    0x441bf6, 0x441d64, 0x447b30, 0x44b176, 0x44bd8d, 0x4827e2, 0x4831b7, 0x483fda,
    0x485519, 0x489d31, 0x48aff3, 0x48ca43, 0x48e729, 0x48f6ee, 0x4c11ae, 0x4c7525,
    0x4c8421, 0x4cc382, 0x4cebd6, 0x500291, 0x50787d, 0x5416a5, 0x542b88, 0x543204,
    0x5443b2, 0x545aa6, 0x549dea, 0x582abd, 0x582bd3, 0x588c81, 0x58b145, 0x58bf25,
    0x58cf79, 0x58e6c5, 0x5c013b, 0x5ccf7f, 0x600194, 0x6055f9, 0x648914, 0x64b708,
    0x64e833, 0x680947, 0x6825dd, 0x686725, 0x689dd2, 0x68b6b3, 0x68c63a, 0x68ee8f,
    0x68fe71, 0x6c2fa0, 0x6c3dd8, 0x6cb456, 0x6cc4af, 0x6cc840, 0x6ce210, 0x70039f,
    0x70041d, 0x704bca, 0x70af09, 0x70b8f6, 0x744dbd, 0x748927, 0x781c3c, 0x782184,
    0x78421c, 0x78e36d, 0x78ee4c, 0x7c0c5f, 0x7c2c67, 0x7c4fad, 0x7c7398, 0x7c87ce,
    0x7c9ebd, 0x7cd544, 0x7cdfa1, 0x7ce8b1, 0x80456b, 0x8053e0, 0x80646f, 0x806599,
    0x807d3a, 0x80b54e, 0x80f1b2, 0x80f3da, 0x840d8e, 0x841fe8, 0x84c7bb, 0x84cca8,
    0x84f3eb, 0x84f703, 0x84fce6, 0x8813bf, 0x8856a6, 0x885721, 0x88f155, 0x8c4598,
    0x8c4b14, 0x8c4f00, 0x8c8c29, 0x8c94df, 0x8caab5, 0x8cbfea, 0x8cce4e, 0x8cfd49,
    0x901506, 0x90380c, 0x90649b, 0x907069, 0x9097d5, 0x90b339, 0x90da72, 0x90e5b1,
    0x943cc6, 0x9451dc, 0x9454c5, 0x94a990, 0x94b555, 0x94b97e, 0x94e686, 0x983dae,
    0x9888e0, 0x98a316, 0x98c377, 0x98cdac, 0x98f4ab, 0x9c139e, 0x9c96d5, 0x9c9c1f,
    0x9c9e6e, 0x9ccc01, 0x9ce087, 0xa020a6, 0xa06025, 0xa0764e, 0xa085e3, 0xa0a3b3,
    0xa0b765, 0xa0dd6c, 0xa0f262, 0xa47b9d, 0xa4cb8f, 0xa4cf12, 0xa4e57c, 0xa4e972,
    0xa4f00f, 0xa8032a, 0xa842e3, 0xa84674, 0xa848fa, 0xa8fd07, 0xac0bfb, 0xac1518,
    0xac276e, 0xac67b2, 0xaca26a, 0xaca704, 0xacd074, 0xacebe6, 0xb03fd3, 0xb08184,
    0xb0a604, 0xb0a732, 0xb0b21c, 0xb0cbd8, 0xb43a45, 0xb48a0a, 0xb4a64a, 0xb4bfe9,
    0xb4e62d, 0xb4feb5, 0xb81f3f, 0xb87b4d, 0xb8bb11, 0xb8d61a, 0xb8f009, 0xb8f862,
    0xbc1926, 0xbcddc2, 0xbcff4d, 0xc049ef, 0xc04e30, 0xc05d89, 0xc0cdd6, 0xc44f33,
    0xc45bbe, 0xc49a90, 0xc49e7e, 0xc4d8d5, 0xc4dd57, 0xc4dee2, 0xc82b96, 0xc82e18,
    0xc88541, 0xc88a7b, 0xc8c9a3, 0xc8da29, 0xc8ea71, 0xc8ed8b, 0xc8f09e, 0xcc50e3,
    0xcc68c7, 0xcc7b5c, 0xcc7e1f, 0xcc8da2, 0xccba97, 0xccdba7, 0xcce732, 0xd09aaf,
    0xd0cf13, 0xd0ef76, 0xd40592, 0xd48afc, 0xd48c49, 0xd4d4da, 0xd4e9f4, 0xd4f98d,
    0xd8132a, 0xd82332, 0xd83bda, 0xd885ac, 0xd8a01d, 0xd8bc38, 0xd8bfc0, 0xd8f15b,
    0xdc0675, 0xdc0a69, 0xdc1ed5, 0xdc4f22, 0xdc5475, 0xdc55b1, 0xdcb4d9, 0xdcda0c,
    0xe05a1b, 0xe072a1, 0xe08cfe, 0xe09806, 0xe0e2e6, 0xe465b8, 0xe4b063, 0xe4b323,
    0xe80690, 0xe831cd, 0xe83dc1, 0xe868e7, 0xe86bea, 0xe89f6d, 0xe8db84, 0xe8edbd,
    0xe8f60a, 0xec6260, 0xec64c9, 0xec94cb, 0xecc9ff, 0xecd61b, 0xecda3b, 0xece334,
    0xecfabc, 0xf008d1, 0xf0161d, 0xf024f9, 0xf09e9e, 0xf0f5bd, 0xf412fa, 0xf42dc9,
    0xf4650b, 0xf4cfa2, 0xf80c4e, 0xf843ee, 0xf85b1b, 0xf8b3b7, 0xfc012c, 0xfcb467,
    0xfce8c0, 0xfcf5c4,
};

// Consecutive MACs this close with the same OUI (first three bytes) chain into a batch.
#define MAC_BATCH_GAP 0x10000

typedef struct {
    uint64_t mac;
    cJSON *obs;
} hint_host_t;

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// "aa:bb:cc:dd:ee:ff" as a 48-bit number.
static bool parse_mac(const char *s, uint64_t *out) {
    uint64_t v = 0;
    for (int i = 0; i < 6; i++) {
        int hi = hex_digit(s[3 * i]);
        if (hi < 0) return false;
        int lo = hex_digit(s[3 * i + 1]);
        if (lo < 0 || s[3 * i + 2] != (i < 5 ? ':' : '\0')) return false;
        v = v << 8 | (uint64_t)(hi << 4 | lo);
    }
    *out = v;
    return true;
}

// Neither multicast nor locally administered (a phone's random MAC).
static bool mac_universal(uint64_t mac) {
    return !((mac >> 40) & 0x03);
}

static int cmp_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

static int cmp_host(const void *a, const void *b) {
    uint64_t x = ((const hint_host_t *)a)->mac, y = ((const hint_host_t *)b)->mac;
    return (x > y) - (x < y);
}

static bool add_hint(discovery_budget_t *budget, cJSON *obs, const char *key, cJSON *value) {
    if (!value) return false;
    size_t bytes = json_string_bound(key) + 2
                   + (cJSON_IsString(value) ? json_string_bound(value->valuestring) : 6);
    if (budget->exhausted || bytes > DISCOVERY_MAX_JSON_BYTES - budget->estimated_bytes) {
        budget->exhausted = true;
        cJSON_Delete(value);
        return false;
    }
    if (!cJSON_AddItemToObject(obs, key, value)) {
        cJSON_Delete(value);
        return false;
    }
    budget->estimated_bytes += bytes;
    return true;
}

// `silent_known`: every source ran and none but ARP was cut short, so an ARP
// host is `silent` when nothing but ARP answered and no scan socket received
// anything from it (no other source named its address, and it isn't in `heard`).
static void add_host_hints(discovery_budget_t *budget, bool silent_known) {
    int hosts = 0, named = 0;
    cJSON *o;
    cJSON_ArrayForEach(o, budget->obs) {
        const char *src = cJSON_GetStringValue(cJSON_GetObjectItem(o, "source"));
        if (!src || !cJSON_GetStringValue(cJSON_GetObjectItem(o, "ip"))) continue;
        if (!strcmp(src, "arp")) hosts++;
        else named++;
    }
    if (!hosts) return;
    size_t bytes = hosts * sizeof(hint_host_t) + named * sizeof(uint32_t);
    hint_host_t *host = heap_caps_calloc(1, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!host) host = calloc(1, bytes);
    if (!host) {
        ESP_LOGW(TAG, "no memory for host hints");
        return;
    }
    uint32_t *ips = (uint32_t *)(host + hosts);
    int h = 0, n = 0;
    cJSON_ArrayForEach(o, budget->obs) {
        const char *src = cJSON_GetStringValue(cJSON_GetObjectItem(o, "source"));
        const char *ip = cJSON_GetStringValue(cJSON_GetObjectItem(o, "ip"));
        if (!src || !ip) continue;
        if (strcmp(src, "arp")) {
            ips[n++] = inet_addr(ip);
            continue;
        }
        const char *mac = cJSON_GetStringValue(cJSON_GetObjectItem(o, "mac"));
        if (mac && parse_mac(mac, &host[h].mac)) host[h++].obs = o;
    }
    qsort(ips, n, sizeof(*ips), cmp_u32);
    int hc = budget->heard ? budget->heard_count : 0;
    if (hc) qsort(budget->heard, hc, sizeof(uint32_t), cmp_u32);

    for (int i = 0; i < h && !budget->exhausted; i++) {
        uint32_t oui = (uint32_t)(host[i].mac >> 24);
        if (mac_universal(host[i].mac)
            && bsearch(&oui, s_espressif_oui, sizeof(s_espressif_oui) / sizeof(s_espressif_oui[0]),
                       sizeof(s_espressif_oui[0]), cmp_u32))
            add_hint(budget, host[i].obs, "maker", cJSON_CreateString("espressif"));
        uint32_t ip = inet_addr(cJSON_GetStringValue(cJSON_GetObjectItem(host[i].obs, "ip")));
        if (silent_known && !bsearch(&ip, ips, n, sizeof(*ips), cmp_u32)
            && !(hc && bsearch(&ip, budget->heard, hc, sizeof(uint32_t), cmp_u32)))
            add_hint(budget, host[i].obs, "silent", cJSON_CreateTrue());
    }

    qsort(host, h, sizeof(*host), cmp_host);
    for (int start = 0, end; start < h && !budget->exhausted; start = end) {
        end = start + 1;
        // Equal MACs (one host's aliases) stay in the run, but don't make a batch alone.
        while (end < h && host[end].mac - host[end - 1].mac <= MAC_BATCH_GAP
               && host[end].mac >> 24 == host[end - 1].mac >> 24)
            end++;
        if (host[end - 1].mac == host[start].mac || !mac_universal(host[start].mac)) continue;
        const char *first = cJSON_GetStringValue(cJSON_GetObjectItem(host[start].obs, "mac"));
        for (int i = start; i < end; i++)
            if (!add_hint(budget, host[i].obs, "mac_batch", cJSON_CreateString(first))) break;
    }
    free(host);
}

// ---- Public API ------------------------------------------------------------

void net_discovery_init(void) {
    ESP_LOGI(TAG, "network discovery initialized");
}

cJSON *net_discovery_run(cJSON *params) {
    esp_netif_t *netif = wifi_mgr_get_netif();
    if (!netif || !wifi_mgr_is_connected()) {
        cJSON *r = cJSON_CreateObject();
        cJSON_AddBoolToObject(r, "ok", false);
        cJSON *err = cJSON_CreateObject();
        cJSON_AddStringToObject(err, "code", "no_network");
        cJSON_AddStringToObject(err, "message", "WiFi not connected");
        cJSON_AddItemToObject(r, "error", err);
        return r;
    }

    uint32_t sources = parse_sources(params);

    esp_netif_ip_info_t ip_info = {0};
    esp_netif_get_ip_info(netif, &ip_info);

    cJSON *result = cJSON_CreateObject();
    if (!result || !cJSON_AddBoolToObject(result, "ok", true)) {
        cJSON_Delete(result); return NULL;
    }

    cJSON *network = cJSON_AddObjectToObject(result, "network");
    char ib[16], nb[16], gb[16];
    snprintf(ib, sizeof(ib), IPSTR, IP2STR(&ip_info.ip));
    snprintf(nb, sizeof(nb), IPSTR, IP2STR(&ip_info.netmask));
    snprintf(gb, sizeof(gb), IPSTR, IP2STR(&ip_info.gw));
    if (!network || !cJSON_AddStringToObject(network, "ip", ib)
        || !cJSON_AddStringToObject(network, "netmask", nb)
        || !cJSON_AddStringToObject(network, "gateway", gb)) {
        cJSON_Delete(result); return NULL;
    }

    cJSON *obs = cJSON_AddArrayToObject(result, "observations");
    cJSON *reports = cJSON_AddObjectToObject(result, "sources");
    if (!obs || !reports) { cJSON_Delete(result); return NULL; }
    static const struct {
        const char *name;
        uint32_t mask;
        int cap;
        int listen_ms;
    } specs[] = {
        {"arp", SRC_ARP, MAX_ARP, 0},
        {"mdns", SRC_MDNS, MAX_MDNS, MDNS_LISTEN_MS},
        {"ssdp", SRC_SSDP, MAX_SSDP, SSDP_LISTEN_MS},
        {"wsd", SRC_WSD, MAX_WSD, WSD_LISTEN_MS},
        {"snmp", SRC_SNMP, MAX_SNMP, SNMP_LISTEN_MS},
        {"netbios", SRC_NETBIOS, MAX_NETBIOS, NETBIOS_LISTEN_MS},
        {"vendor", SRC_VENDOR, MAX_VENDOR, VENDOR_LISTEN_MS},
    };
    discovery_budget_t budget = { .obs = obs, .estimated_bytes = DISCOVERY_METADATA_BYTES };
    bool complete = true, failed = false;
    bool silent_known = (sources & SRC_ALL) == SRC_ALL;
    if (silent_known) {
        // 1 KB: in PSRAM where there is any.
        budget.heard = heap_caps_malloc(MAX_HEARD * sizeof(uint32_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!budget.heard) budget.heard = malloc(MAX_HEARD * sizeof(uint32_t));
        if (!budget.heard) silent_known = false;
    }
    for (size_t i = 0; i < sizeof(specs) / sizeof(specs[0]); i++) {
        if (!(sources & specs[i].mask)) continue;
        discovery_source_t source = { .cap = specs[i].cap };
        if (budget.exhausted) {
            source.truncated = true;
        } else switch (specs[i].mask) {
            case SRC_ARP: arp_probe(&budget, netif, &source); break;
            case SRC_MDNS: mdns_probe(&budget, &source); break;
            case SRC_SSDP: ssdp_probe(&budget, &source); break;
            case SRC_WSD: wsd_probe(&budget, &source); break;
            case SRC_SNMP: snmp_probe(&budget, netif, &source); break;
            case SRC_NETBIOS: netbios_probe(&budget, netif, &source); break;
            case SRC_VENDOR: vendor_probe(&budget, &source); break;
        }
        cJSON *report = cJSON_AddObjectToObject(reports, specs[i].name);
        if (!report
            || !cJSON_AddNumberToObject(report, "found", source.found)
            || !cJSON_AddNumberToObject(report, "cap", source.cap)
            || !cJSON_AddBoolToObject(report, "truncated", source.truncated)
            || (specs[i].mask == SRC_ARP
                ? (!cJSON_AddNumberToObject(report, "swept_hosts", source.swept_hosts)
                   || !cJSON_AddNumberToObject(report, "subnet_hosts", source.subnet_hosts))
                : !cJSON_AddNumberToObject(report, "listen_ms", specs[i].listen_ms))
            || (source.error && !cJSON_AddStringToObject(report, "error", source.error))) {
            free(budget.heard);
            cJSON_Delete(result);
            return NULL;
        }
        if (source.truncated || source.error) {
            complete = false;
            if (specs[i].mask != SRC_ARP) silent_known = false;
        }
        if (source.error) failed = true;
    }
    if (budget.heard_partial) silent_known = false;
    add_host_hints(&budget, silent_known);
    free(budget.heard);
    if (!cJSON_AddBoolToObject(result, "complete", complete)) {
        cJSON_Delete(result);
        return NULL;
    }
    if (failed) {
        cJSON_SetBoolValue(cJSON_GetObjectItem(result, "ok"), false);
        cJSON *error = cJSON_AddObjectToObject(result, "error");
        if (!error || !cJSON_AddStringToObject(error, "code", "discovery_failed")
            || !cJSON_AddStringToObject(error, "message", "one or more discovery sources failed")) {
            cJSON_Delete(result);
            return NULL;
        }
    }

    ESP_LOGI(TAG, "discovery complete: %d observations",
             cJSON_GetArraySize(obs));
    return result;
}
