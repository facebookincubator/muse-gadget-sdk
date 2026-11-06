/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include "xiao_147_touch.h"

int main(void)
{
    uint8_t report[14] = {0, 1};
    uint16_t x = 999, y = 999;
    assert(xiao_147_touch_decode(report, sizeof(report), &x, &y));
    assert(x == 171 && y == 0);
    report[3] = 171; report[4] = 1; report[5] = 63;
    assert(xiao_147_touch_decode(report, sizeof(report), &x, &y));
    assert(x == 0 && y == 319);
    report[1] = 2;
    assert(xiao_147_touch_decode(report, sizeof(report), &x, &y));
    report[1] = 0;
    assert(!xiao_147_touch_decode(report, sizeof(report), &x, &y));
    report[1] = 3;
    assert(!xiao_147_touch_decode(report, sizeof(report), &x, &y));
    report[1] = 1; report[3] = 172;
    assert(!xiao_147_touch_decode(report, sizeof(report), &x, &y));
    report[3] = 0; report[5] = 64;
    assert(!xiao_147_touch_decode(report, sizeof(report), &x, &y));
    report[2] = 0xff; report[3] = 0xff;
    assert(!xiao_147_touch_decode(report, sizeof(report), &x, &y));
    assert(!xiao_147_touch_decode(report, 5, &x, &y));
    assert(!xiao_147_touch_decode(NULL, 14, &x, &y));
    assert(!xiao_147_touch_decode(report, 14, NULL, &y));
    puts("AXS5106L portrait, release and malformed-report cases passed");
    return 0;
}
