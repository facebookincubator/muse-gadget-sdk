#pragma once
#include <stdbool.h>

typedef void (*media_result_cb)(const char *kind, bool ok, const char *message);
void media_configure(media_result_cb callback, const char *directory);
bool media_start(const char *kind, const char *text);
bool media_busy(void);
void media_poll(void);
