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

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int64_t now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static bool wait_fd(int fd, bool write, int64_t deadline) {
    for (;;) {
        int64_t left = deadline - now_ms();
        if (left <= 0) { errno = ETIMEDOUT; return false; }
        struct timeval tv = {left / 1000, (left % 1000) * 1000};
        fd_set set;
        FD_ZERO(&set);
        FD_SET(fd, &set);
        int r = select(fd + 1, write ? NULL : &set, write ? &set : NULL, NULL, &tv);
        if (r > 0) return true;
        if (r < 0 && errno == EINTR) continue;
        if (r == 0) errno = ETIMEDOUT;
        return false;
    }
}

static bool transfer(int fd, void *buf, size_t len, bool write, int64_t deadline) {
    unsigned char *p = buf;
    while (len) {
        if (!wait_fd(fd, write, deadline)) return false;
        ssize_t n = write ? send(fd, p, len, 0) : recv(fd, p, len, 0);
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (n <= 0) return false;
        p += n;
        len -= n;
    }
    return true;
}

static void base64(const unsigned char *in, size_t n, char *out) {
    static const char chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t w = 0;
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = (unsigned)in[i] << 16;
        if (i + 1 < n) v |= (unsigned)in[i + 1] << 8;
        if (i + 2 < n) v |= in[i + 2];
        out[w++] = chars[v >> 18];
        out[w++] = chars[(v >> 12) & 63];
        out[w++] = i + 1 < n ? chars[(v >> 6) & 63] : '=';
        out[w++] = i + 2 < n ? chars[v & 63] : '=';
    }
    out[w] = 0;
}

static bool http_connect(int fd, const network_proxy_config_t *p,
                         const char *host, int port, int64_t deadline) {
    char auth[2 * NETWORK_PROXY_AUTH_MAX + 2];
    char encoded[4 * ((sizeof(auth) + 2) / 3) + 1] = {0};
    if (p->username[0]) {
        int n = snprintf(auth, sizeof(auth), "%s:%s", p->username, p->password);
        base64((unsigned char *)auth, n, encoded);
    }
    char request[768];
    bool ipv6 = strchr(host, ':') != NULL;
    int n = snprintf(request, sizeof(request),
        "CONNECT %s%s%s:%d HTTP/1.1\r\nHost: %s%s%s:%d\r\n%s%s%s\r\n",
        ipv6 ? "[" : "", host, ipv6 ? "]" : "", port,
        ipv6 ? "[" : "", host, ipv6 ? "]" : "", port,
        p->username[0] ? "Proxy-Authorization: Basic " : "", encoded,
        p->username[0] ? "\r\n" : "");
    bool ok = n > 0 && (size_t)n < sizeof(request) && transfer(fd, request, n, true, deadline);
    network_proxy_wipe(auth, sizeof(auth));
    network_proxy_wipe(encoded, sizeof(encoded));
    network_proxy_wipe(request, sizeof(request));
    if (!ok) return false;
    // Read exactly through CRLFCRLF: the next byte belongs to the tunnel.
    char status[14] = {0};
    unsigned tail = 0;
    for (unsigned i = 0; i < 4096; i++) {
        unsigned char c;
        if (!transfer(fd, &c, 1, false, deadline)) return false;
        if (i < sizeof(status) - 1) status[i] = c;
        tail = (tail << 8) | c;
        if (tail == 0x0d0a0d0a) {
            return (!memcmp(status, "HTTP/1.0 ", 9) || !memcmp(status, "HTTP/1.1 ", 9))
                && status[9] == '2' && status[10] >= '0' && status[10] <= '9'
                && status[11] >= '0' && status[11] <= '9'
                && (status[12] == ' ' || status[12] == '\r');
        }
    }
    return false;
}

static bool socks_connect(int fd, const network_proxy_config_t *p,
                          const char *host, int port, int64_t deadline) {
    // Offer only the configured method, so authentication cannot downgrade.
    unsigned char method = p->username[0] ? 2 : 0;
    unsigned char greeting[] = {5, 1, method}, reply[4];
    if (!transfer(fd, greeting, sizeof(greeting), true, deadline)
        || !transfer(fd, reply, 2, false, deadline)
        || reply[0] != 5 || reply[1] != method) return false;
    if (method == 2) {
        unsigned char auth[2 * NETWORK_PROXY_AUTH_MAX + 3];
        size_t u = strlen(p->username), pw = strlen(p->password);
        auth[0] = 1;
        auth[1] = u;
        memcpy(auth + 2, p->username, u);
        auth[2 + u] = pw;
        memcpy(auth + 3 + u, p->password, pw);
        bool ok = transfer(fd, auth, 3 + u + pw, true, deadline);
        network_proxy_wipe(auth, sizeof(auth));
        if (!ok || !transfer(fd, reply, 2, false, deadline)
            || reply[0] != 1 || reply[1] != 0) return false;
    }
    unsigned char req[4 + 1 + 255 + 2] = {5, 1, 0}, address[16];
    size_t n;
    if (inet_pton(AF_INET, host, address) == 1) { req[3] = 1; n = 4; }
    else if (inet_pton(AF_INET6, host, address) == 1) { req[3] = 4; n = 16; }
    else {
        req[3] = 3;
        n = strlen(host);
        req[4] = n;
        memcpy(req + 5, host, n);
        n++;
    }
    if (req[3] != 3) memcpy(req + 4, address, n);
    req[4 + n] = port >> 8;
    req[5 + n] = port & 255;
    if (!transfer(fd, req, 6 + n, true, deadline)
        || !transfer(fd, reply, 4, false, deadline)
        || reply[0] != 5 || reply[1] != 0 || reply[2] != 0) return false;
    if (reply[3] == 1) n = 4;
    else if (reply[3] == 4) n = 16;
    else if (reply[3] == 3) {
        if (!transfer(fd, reply, 1, false, deadline) || !reply[0]) return false;
        n = reply[0];
    } else return false;
    return transfer(fd, req, n + 2, false, deadline);
}

int network_proxy_open(const network_proxy_config_t *p, const char *host,
                       int port, int timeout_ms) {
    if (!p || !host || !*host || strlen(host) > 255 || strpbrk(host, "\r\n /@")
        || port < 1 || port > 65535 || timeout_ms <= 0) return -1;
    for (const unsigned char *c = (const unsigned char *)host; *c; c++) {
        if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z')
              || (*c >= '0' && *c <= '9') || strchr(".-:_", *c))) return -1;
    }
    bool direct = p->type == NETWORK_PROXY_OFF || network_proxy_local_host(host);
    if (!direct && !network_proxy_valid(p)) return -1;
    const char *peer = direct ? host : p->host;
    char service[6];
    snprintf(service, sizeof(service), "%u", direct ? port : p->port);
    struct addrinfo hints = {0}, *addresses = NULL;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    int64_t deadline = now_ms() + timeout_ms;
    if (getaddrinfo(peer, service, &hints, &addresses)) return -1;
    int fd = -1;
    for (struct addrinfo *a = addresses; a && now_ms() < deadline; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) continue;
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) goto next;
        if (connect(fd, a->ai_addr, a->ai_addrlen) != 0) {
            if (errno != EINPROGRESS || !wait_fd(fd, true, deadline)) goto next;
            int err = 0;
            socklen_t len = sizeof(err);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) || err) goto next;
        }
        break;
next:
        close(fd);
        fd = -1;
    }
    freeaddrinfo(addresses);
    if (fd < 0) return -1;
    bool ok = direct || (p->type == NETWORK_PROXY_HTTP
        ? http_connect(fd, p, host, port, deadline)
        : socks_connect(fd, p, host, port, deadline));
    if (ok) {
        int flags = fcntl(fd, F_GETFL, 0);
        struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
        ok = flags >= 0 && fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) == 0
             && setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0
             && setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) == 0;
    }
    if (!ok) { close(fd); return -1; }
    return fd;
}
