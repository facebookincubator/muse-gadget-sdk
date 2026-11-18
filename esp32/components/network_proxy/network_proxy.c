/* Copyright (c) Meta Platforms, Inc. and affiliates.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "network_proxy.h"

#include <sys/socket.h>
#include <arpa/inet.h>
#include <string.h>
#include <strings.h>

void network_proxy_wipe(void *data, unsigned len) {
    volatile unsigned char *p = data;
    while (len--) *p++ = 0;
}

static void wipe_json(cJSON *item) {
    for (cJSON *v = item; v; v = v->next) {
        if (cJSON_IsString(v)) network_proxy_wipe(v->valuestring, strlen(v->valuestring));
        if (v->child) wipe_json(v->child);
    }
}

void network_proxy_delete_json(cJSON *root) {
    wipe_json(root);
    cJSON_Delete(root);
}

static bool host_valid(const char *host) {
    size_t n = strnlen(host, NETWORK_PROXY_HOST_MAX + 1);
    if (!n || n > NETWORK_PROXY_HOST_MAX) return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = host[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
              || (c >= '0' && c <= '9') || strchr(".-:_", c))) return false;
    }
    return true;
}

bool network_proxy_valid(const network_proxy_config_t *p) {
    if (!p) return false;
    if (p->type == NETWORK_PROXY_OFF) return true;
    return (p->type == NETWORK_PROXY_HTTP || p->type == NETWORK_PROXY_SOCKS5)
        && host_valid(p->host) && p->port
        && strnlen(p->username, sizeof(p->username)) <= NETWORK_PROXY_AUTH_MAX
        && strnlen(p->password, sizeof(p->password)) <= NETWORK_PROXY_AUTH_MAX
        && (!p->password[0] || p->username[0])
        && (p->type != NETWORK_PROXY_HTTP || !strchr(p->username, ':'));
}

bool network_proxy_equal(const network_proxy_config_t *a, const network_proxy_config_t *b) {
    return a->type == b->type && (a->type == NETWORK_PROXY_OFF
        || (a->port == b->port && !strcmp(a->host, b->host)
            && !strcmp(a->username, b->username) && !strcmp(a->password, b->password)));
}

static bool string_field(const cJSON *o, const char *name, char *out, size_t cap) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, name);
    if (!v) return true;
    if (!cJSON_IsString(v) || strlen(v->valuestring) >= cap) return false;
    strcpy(out, v->valuestring);
    return true;
}

bool network_proxy_parse(const cJSON *o, network_proxy_config_t *out) {
    memset(out, 0, sizeof(*out));
    if (!o) return true;
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(o, "type");
    const cJSON *port = cJSON_GetObjectItemCaseSensitive(o, "port");
    if (!cJSON_IsObject(o) || !cJSON_IsString(type)) goto invalid;
    if (!strcmp(type->valuestring, "off")) return true;
    if (!strcmp(type->valuestring, "http")) out->type = NETWORK_PROXY_HTTP;
    else if (!strcmp(type->valuestring, "socks5")) out->type = NETWORK_PROXY_SOCKS5;
    else goto invalid;
    if (!cJSON_IsNumber(port) || port->valuedouble < 1 || port->valuedouble > 65535
        || port->valuedouble != port->valueint) goto invalid;
    out->port = (uint16_t)port->valueint;
    if (!string_field(o, "host", out->host, sizeof(out->host))
        || !string_field(o, "username", out->username, sizeof(out->username))
        || !string_field(o, "password", out->password, sizeof(out->password))
        || !network_proxy_valid(out)) goto invalid;
    return true;
invalid:
    network_proxy_wipe(out, sizeof(*out));
    out->type = NETWORK_PROXY_INVALID;
    return false;
}

cJSON *network_proxy_json(const network_proxy_config_t *p) {
    cJSON *o = cJSON_CreateObject();
    const char *type = p->type == NETWORK_PROXY_OFF ? "off"
                     : p->type == NETWORK_PROXY_HTTP ? "http"
                     : p->type == NETWORK_PROXY_SOCKS5 ? "socks5" : "invalid";
    bool ok = o && cJSON_AddStringToObject(o, "type", type);
    if (p->type != NETWORK_PROXY_OFF) {
        ok = ok && cJSON_AddStringToObject(o, "host", p->host)
             && cJSON_AddNumberToObject(o, "port", p->port)
             && cJSON_AddStringToObject(o, "username", p->username)
             && cJSON_AddStringToObject(o, "password", p->password);
    }
    if (!ok) { cJSON_Delete(o); return NULL; }
    return o;
}

bool network_proxy_local_host(const char *host) {
    if (!host || !*host) return false;
    struct in_addr v4;
    if (inet_pton(AF_INET, host, &v4) == 1) {
        uint32_t a = ntohl(v4.s_addr);
        return (a >> 24) == 127 || (a >> 24) == 10
            || (a >> 20) == 0xac1 || (a >> 16) == 0xc0a8
            || (a >> 16) == 0xa9fe;
    }
    struct in6_addr v6;
    if (inet_pton(AF_INET6, host, &v6) == 1) {
        // lwIP's IN6_IS_* macros cast to ip6_addr_t (which includes a zone on
        // IDF); inspect the wire bytes to stay within struct in6_addr.
        static const unsigned char zero[16] = {0};
        const unsigned char *a = v6.s6_addr;
        if ((!memcmp(a, zero, 15) && a[15] == 1)
            || (a[0] == 0xfe && (a[1] & 0xc0) == 0x80)
            || (a[0] & 0xfe) == 0xfc) return true;
        if (!memcmp(a, zero, 10) && a[10] == 0xff && a[11] == 0xff) {
            char text[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &v6.s6_addr[12], text, sizeof(text));
            return network_proxy_local_host(text);
        }
        return false;
    }
    size_t n = strlen(host);
    if (n && host[n - 1] == '.') n--;
    return (n == 9 && !strncasecmp(host, "localhost", n))
        || (n > 6 && !strncasecmp(host + n - 6, ".local", 6))
        || (!memchr(host, '.', n) && !strchr(host, ':'));
}
