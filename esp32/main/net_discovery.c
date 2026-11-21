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
    bind(sock, (struct sockaddr *)&ba, sizeof(ba));

    struct ip_mreq mr = { .imr_multiaddr.s_addr = inet_addr("224.0.0.251"),
                          .imr_interface.s_addr = htonl(INADDR_ANY) };
    setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mr, sizeof(mr));

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
        sendto(sock, qbuf, off + 4, 0, (struct sockaddr *)&dest, sizeof(dest));
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
        int n = recvfrom(sock, rbuf, sizeof(rbuf), 0, NULL, NULL);
        if (n <= 0) break;
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

    source->truncated = fc >= MAX_MDNS || addrs_full;
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
    sendto(sock, msearch, strlen(msearch), 0,
           (struct sockaddr *)&dest, sizeof(dest));

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
        if (n <= 0) break;
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
    sendto(sock, probe, strlen(probe), 0,
           (struct sockaddr *)&dest, sizeof(dest));

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
        if (n <= 0) break;
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
    sendto(sock, pkt, sizeof(pkt), 0, (struct sockaddr *)&dest, sizeof(dest));

    struct timeval tv = { .tv_sec = SNMP_LISTEN_MS / 1000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t buf[RECV_BUF];
    int count = 0;
    int64_t deadline = esp_timer_get_time() + SNMP_LISTEN_MS * 1000LL;

    while (esp_timer_get_time() < deadline && count < MAX_SNMP) {
        struct sockaddr_in from; socklen_t fl = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf), 0,
                         (struct sockaddr *)&from, &fl);
        if (n <= 0) break;
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
    sendto(sock, query, sizeof(query), 0,
           (struct sockaddr *)&dest, sizeof(dest));

    struct timeval tv = { .tv_sec = NETBIOS_LISTEN_MS / 1000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t buf[RECV_BUF];
    int count = 0;
    int64_t deadline = esp_timer_get_time() + NETBIOS_LISTEN_MS * 1000LL;

    while (esp_timer_get_time() < deadline && count < MAX_NETBIOS) {
        struct sockaddr_in from; socklen_t fl = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf), 0,
                         (struct sockaddr *)&from, &fl);
        if (n <= 0) break;

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

static void vendor_send(int sock, const char *group, uint16_t port,
                        const void *msg, size_t len) {
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(port),
                              .sin_addr.s_addr = group ? inet_addr(group)
                                                       : htonl(INADDR_BROADCAST) };
    sendto(sock, msg, len, 0, (struct sockaddr *)&to, sizeof(to));
}

static void vendor_queries(int sock, int govee) {
    uint8_t kasa[] = "{\"system\":{\"get_sysinfo\":{}}}";
    kasa_xor(kasa, sizeof(kasa) - 1, true);
    vendor_send(sock, NULL, 9999, kasa, sizeof(kasa) - 1);
    static const uint8_t tdp[16] = {0x02, 0, 0, 0x01, 0, 0, 0, 0,
                                    0, 0, 0, 0, 0x46, 0x3c, 0xb5, 0xd3};
    vendor_send(sock, NULL, 20002, tdp, sizeof(tdp));
    static const char govee_scan[] =
        "{\"msg\":{\"cmd\":\"scan\",\"data\":{\"account_topic\":\"reserve\"}}}";
    vendor_send(govee, "239.255.255.250", 4001, govee_scan, strlen(govee_scan));
    uint8_t miio[32];
    memset(miio, 0xff, sizeof(miio));
    miio[0] = 0x21; miio[1] = 0x31; miio[2] = 0x00; miio[3] = 0x20;
    vendor_send(sock, NULL, 54321, miio, sizeof(miio));
    static const char wiz[] = "{\"method\":\"getPilot\",\"params\":{}}";
    vendor_send(sock, NULL, 38899, wiz, strlen(wiz));
    // LIFX GetService: 36-byte header, tagged, protocol 1024, message type 2.
    static const uint8_t lifx[36] = {36, 0, 0x00, 0x34, 'M', 'u', 's', 'e', [32] = 2};
    vendor_send(sock, NULL, 56700, lifx, sizeof(lifx));
    static const char yeelight[] =
        "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1982\r\n"
        "MAN: \"ssdp:discover\"\r\nST: wifi_bulb\r\n";
    vendor_send(sock, "239.255.255.250", 1982, yeelight, strlen(yeelight));
    static const uint8_t cs2[4] = {0xf1, 0x30, 0x00, 0x00};   // CS2/PPPP LAN search
    vendor_send(sock, NULL, 32108, cs2, sizeof(cs2));
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
    else vendor_queries(socks[0], socks[1] >= 0 ? socks[1] : socks[0]);

    int64_t start = esp_timer_get_time(), deadline = start + VENDOR_LISTEN_MS * 1000LL;
    bool resent = false;
    while (!source->error && !budget->exhausted && source->found < MAX_VENDOR
           && esp_timer_get_time() < deadline) {
        if (!resent && esp_timer_get_time() >= start + VENDOR_RESEND_MS * 1000LL) {
            vendor_queries(socks[0], socks[1] >= 0 ? socks[1] : socks[0]);
            resent = true;
        }
        bool heard = false;
        for (size_t i = 0; i < VENDOR_SOCKETS && !budget->exhausted
             && source->found < MAX_VENDOR; i++) {
            if (socks[i] < 0) continue;
            struct sockaddr_in from; socklen_t fl = sizeof(from);
            int n = recvfrom(socks[i], buf, RECV_BUF, MSG_DONTWAIT,
                             (struct sockaddr *)&from, &fl);
            if (n <= 0) continue;
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
            cJSON_Delete(result);
            return NULL;
        }
        if (source.truncated || source.error) complete = false;
        if (source.error) failed = true;
    }
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
