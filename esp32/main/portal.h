/*
 * 配网门户（见 portal.c）。
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

/* 开热点 + 起 HTTP 门户。10 分钟无操作或连上网络后自动关。 */
esp_err_t portal_start(void);

/* 强制走热点模式（设备在访客/隔离网络里、局域网模式够不到时用） */
esp_err_t portal_start_ap(void);
void portal_stop(void);
bool portal_active(void);

/* 串口打印二维码（>portal=qr）：烧录完直接扫，不必先找热点名 */
void portal_print_qr(void);
