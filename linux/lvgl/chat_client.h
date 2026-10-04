#pragma once
#include <stdbool.h>
#include <stdint.h>

#define CHAT_TEXT_MAX 65536
typedef struct {
    uint64_t version;
    bool done;
    bool failed;
    char status[160];
    char text[CHAT_TEXT_MAX + 1];
} chat_update_t;

bool chat_start(const char *host, const char *port, const char *token,
                const char *session, const char *prompt);
bool chat_poll(chat_update_t *update);
void chat_stop(void);
