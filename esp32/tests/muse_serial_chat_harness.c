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

/* Drives the serial console's "@chat" encoder and line unescaper (muse_chat_text.c)
 * for test_muse_serial_chat.py, which parses the result the way tools/muse/chat.py does.
 *   console    stdin is a reply's text: prints the lines a typed turn sends for it
 *   unescape   stdin is console lines: prints each unescaped, as "<length>:<bytes>"
 *   caption C  stdin is a reply's text: prints it wrapped to C columns, as the
 *              screen pages it (test_muse_caption_wrap.py) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "muse_chat.h"
#include "muse_chat_priv.h"
#include "muse_state.h"

/* Captions page to the screen; the console lines tested here don't. */
static int s_cols = 16, s_lines = 2;

void muse_state_page(bool cjk, int *cols, int *lines)
{
    (void)cjk;
    *cols = s_cols;
    *lines = s_lines;
}

static char *read_all(size_t *len)
{
    size_t cap = 1 << 16, n = 0;
    char *buf = malloc(cap + 1);
    size_t got;
    while (buf && (got = fread(buf + n, 1, cap - n, stdin)) > 0) {
        n += got;
        if (n == cap) {
            cap *= 2;
            buf = realloc(buf, cap + 1);
        }
    }
    if (!buf) {
        exit(2);
    }
    buf[n] = '\0';
    *len = n;
    return buf;
}

int main(int argc, char **argv)
{
    size_t len;
    char *in = read_all(&len);
    if (argc > 1 && !strcmp(argv[1], "console")) {
        muse_hatch_console("sent", NULL, "\"bytes\":%u", 12u);
        muse_hatch_console("busy", NULL, "\"on\":%s", "true");
        muse_hatch_console("text", in, "\"msg\":%d", 0);
        muse_hatch_console("message_done", NULL, "\"msg\":%d,\"bytes\":%u", 0, (unsigned)strlen(in));
        muse_hatch_console("text", "", "\"msg\":%d", 1);
        muse_hatch_console("done", NULL, "\"messages\":%d,\"complete\":true", 2);
        muse_hatch_console("error", "TOO LONG", NULL);
    } else if (argc > 1 && !strcmp(argv[1], "unescape")) {
        for (char *line = in, *end; *line; line = end + 1) {
            end = strchr(line, '\n');
            if (!end) {
                end = line + strlen(line);
            }
            char keep = *end;
            *end = '\0';
            size_t n = muse_hatch_unescape(line);
            printf("%zu:", n);
            fwrite(line, 1, n, stdout);
            if (!keep) {
                break;
            }
        }
    } else if (argc > 2 && !strcmp(argv[1], "caption")) {
        /* One page tall enough for the whole reply: every wrapped line. */
        s_cols = atoi(argv[2]);
        s_lines = 1000;
        static char page[1 << 16];
        if (muse_hatch_caption_at(in, 0, page, sizeof(page))) {
            fputs(page, stdout);
        }
    } else {
        fprintf(stderr, "usage: %s console|unescape|caption COLS < input\n", argv[0]);
        return 2;
    }
    free(in);
    return 0;
}
