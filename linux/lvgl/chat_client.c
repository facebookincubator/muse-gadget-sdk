#define _POSIX_C_SOURCE 200809L
#include "chat_client.h"
#include <errno.h>
#include <netdb.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t worker;
static bool running;
static bool cancelled;
static int active_fd = -1;
static chat_update_t latest;
typedef struct { char *host, *port, *token, *session, *prompt; } request_t;

static bool transfer(int fd, void *buffer, size_t bytes, bool writing)
{
    char *p = buffer;
    while (bytes) {
        ssize_t n = writing ? send(fd, p, bytes, 0) : recv(fd, p, bytes, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        p += n; bytes -= (size_t)n;
    }
    return true;
}

static void publish(char type, const char *text)
{
    pthread_mutex_lock(&lock);
    if (type == 'R') {
        size_t len = strlen(text);
        if (len > CHAT_TEXT_MAX) {
            len = CHAT_TEXT_MAX - 32;
            while (len && ((unsigned char)text[len] & 0xc0) == 0x80) len--;
            memcpy(latest.text, text, len);
            strcpy(latest.text + len, "\n[Reply display limit reached]");
        } else memcpy(latest.text, text, len + 1);
        snprintf(latest.status, sizeof(latest.status), "Muse is replying...");
    } else if (type == 'S') snprintf(latest.status, sizeof(latest.status), "%s", text);
    else {
        latest.done = true;
        latest.failed = type == 'E';
        snprintf(latest.status, sizeof(latest.status), "%s", type == 'D' ? "Ready to talk" : text);
    }
    latest.version++;
    pthread_mutex_unlock(&lock);
}

static void *conversation(void *value)
{
    request_t *r = value;
    int fd = -1;
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM}, *addresses = NULL;
    if (getaddrinfo(r->host, r->port, &hints, &addresses) != 0) goto failed;
    for (struct addrinfo *a = addresses; a; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) continue;
        struct timeval timeout = {.tv_sec = 210};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        struct timeval send_timeout = {.tv_sec = 10};
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout));
        pthread_mutex_lock(&lock);
        if (cancelled) { pthread_mutex_unlock(&lock); close(fd); fd = -1; break; }
        active_fd = fd;
        pthread_mutex_unlock(&lock);
        if (connect(fd, a->ai_addr, a->ai_addrlen) == 0) break;
        pthread_mutex_lock(&lock); active_fd = -1; pthread_mutex_unlock(&lock);
        close(fd); fd = -1;
    }
    freeaddrinfo(addresses);
    if (fd < 0) goto failed;
    char header[160];
    int h = snprintf(header, sizeof(header), "%s\n%s\n", r->token, r->session);
    if (h < 0 || h >= (int)sizeof(header)) goto failed;
    size_t n = strlen(r->prompt);
    unsigned char length[4] = {n & 255, (n >> 8) & 255, (n >> 16) & 255, (n >> 24) & 255};
    if (!transfer(fd, header, (size_t)h, true) || !transfer(fd, length, 4, true) ||
        !transfer(fd, r->prompt, n, true)) goto failed;
    for (;;) {
        unsigned char frame[5];
        if (!transfer(fd, frame, 5, false)) goto failed;
        uint32_t bytes = (uint32_t)frame[1] | ((uint32_t)frame[2] << 8) |
            ((uint32_t)frame[3] << 16) | ((uint32_t)frame[4] << 24);
        if (bytes > 1024 * 1024 || !strchr("SRDE", frame[0]) || frame[0] == 0) goto failed;
        char *text = malloc((size_t)bytes + 1);
        if (!text) goto failed;
        bool ok = transfer(fd, text, bytes, false);
        text[bytes] = 0;
        if (ok) publish((char)frame[0], text);
        free(text);
        if (!ok) goto failed;
        if (frame[0] == 'D' || frame[0] == 'E') break;
    }
    goto finished;
failed:
    publish('E', "Connection ended. Check that your Muse connector is online, then try again.");
finished:
    pthread_mutex_lock(&lock);
    active_fd = -1;
    if (fd >= 0) close(fd);
    pthread_mutex_unlock(&lock);
    free(r->host); free(r->port); free(r->token); free(r->session); free(r->prompt); free(r);
    return NULL;
}

void chat_stop(void)
{
    if (!running) return;
    pthread_mutex_lock(&lock);
    cancelled = true;
    if (active_fd >= 0) shutdown(active_fd, SHUT_RDWR);
    pthread_mutex_unlock(&lock);
    pthread_join(worker, NULL);
    running = false;
}

bool chat_start(const char *host, const char *port, const char *token,
                const char *session, const char *prompt)
{
    chat_stop();
    request_t *r = calloc(1, sizeof(*r));
    if (!r) return false;
    r->host = strdup(host); r->port = strdup(port); r->token = strdup(token);
    r->session = strdup(session); r->prompt = strdup(prompt);
    if (!r->host || !r->port || !r->token || !r->session || !r->prompt) {
        free(r->host); free(r->port); free(r->token); free(r->session); free(r->prompt); free(r); return false;
    }
    pthread_mutex_lock(&lock);
    uint64_t version = latest.version + 1;
    memset(&latest, 0, sizeof(latest)); latest.version = version; cancelled = false;
    strcpy(latest.status, "Waiting for Muse...");
    pthread_mutex_unlock(&lock);
    if (pthread_create(&worker, NULL, conversation, r) != 0) {
        free(r->host); free(r->port); free(r->token); free(r->session); free(r->prompt); free(r); return false;
    }
    running = true;
    return true;
}

bool chat_poll(chat_update_t *update)
{
    pthread_mutex_lock(&lock);
    bool changed = latest.version != update->version;
    if (changed) *update = latest;
    pthread_mutex_unlock(&lock);
    return changed;
}
