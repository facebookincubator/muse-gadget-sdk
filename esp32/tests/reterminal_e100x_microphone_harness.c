#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "reterminal_e100x_microphone.h"
#include "driver/gpio.h"
#include "driver/i2s_pdm.h"

static int s_failure, s_power, s_new, s_deleted, s_enabled, s_disabled, s_reads;
int gpio_config(const gpio_config_t *c) {
    assert(c->pin_bit_mask == (1ULL << 38));
    return s_failure == 1 ? -1 : 0;
}
int gpio_set_level(int gpio, int level) { assert(gpio == 38); s_power = level; return 0; }
void vTaskDelay(int ms) { assert(ms == 20); }
void *heap_caps_malloc(size_t n, int caps) { (void)caps; return s_failure == 8 ? NULL : malloc(n); }
int i2s_new_channel(const i2s_chan_config_t *c, void *tx, i2s_chan_handle_t *out) {
    assert(c->port == 0 && !tx);
    if (s_failure == 2) return -1;
    *out = (void *)1; s_new++;
    return 0;
}
int i2s_channel_init_pdm_rx_mode(i2s_chan_handle_t ch, const i2s_pdm_rx_config_t *c) {
    assert(ch && c->clk_cfg.rate == 16000 && c->slot_cfg.bits == 16);
    assert(c->slot_cfg.mode == I2S_SLOT_MODE_MONO);
    assert(c->gpio_cfg.clk == 42 && c->gpio_cfg.din == 41);
    return s_failure == 3 ? -1 : 0;
}
int i2s_channel_enable(i2s_chan_handle_t ch) {
    assert(ch && s_power == 1);
    if (s_failure == 4) return -1;
    s_enabled++; return 0;
}
int i2s_channel_disable(i2s_chan_handle_t ch) { assert(ch); s_disabled++; return 0; }
int i2s_del_channel(i2s_chan_handle_t ch) { assert(ch); s_deleted++; return 0; }
int i2s_channel_read(i2s_chan_handle_t ch, void *data, size_t bytes, size_t *read, unsigned timeout) {
    assert(ch && timeout > 0);
    s_reads++;
    if ((s_failure == 5 && s_reads == 1) || (s_failure == 6 && s_reads == 2)) {
        *read = 0; return -1;
    }
    memset(data, 0, bytes); *read = bytes; return 0;
}
int mbedtls_base64_encode(unsigned char *out, size_t cap, size_t *len,
                          const unsigned char *input, size_t bytes) {
    assert(input && bytes == 3200); // 100ms PCM16 mono at 16kHz.
    if (s_failure == 7) return -1;
    *len = 4 * ((bytes + 2) / 3);
    assert(cap > *len);
    memset(out, 'A', *len); out[*len] = 0;
    return 0;
}
static cJSON *record(const char *json) {
    cJSON *params = cJSON_Parse(json);
    cJSON *r = reterminal_e100x_microphone_record(params);
    cJSON_Delete(params);
    assert(r);
    return r;
}
int main(void) {
    cJSON *r = record("{\"duration_ms\":100}");
    assert(cJSON_IsTrue(cJSON_GetObjectItem(r, "ok")));
    cJSON *p = cJSON_GetObjectItem(r, "payload");
    assert(cJSON_GetObjectItem(p, "bytes")->valueint == 3200);
    assert(cJSON_GetObjectItem(p, "channels")->valueint == 1);
    assert(!s_power && s_new == s_deleted && s_enabled == s_disabled && s_reads == 2);
    cJSON_Delete(r);
    for (s_failure = 1; s_failure <= 8; s_failure++) {
        s_reads = 0;
        r = record("{\"duration_ms\":100}");
        assert(cJSON_IsFalse(cJSON_GetObjectItem(r, "ok")));
        assert(!s_power && s_new == s_deleted && s_enabled == s_disabled);
        cJSON_Delete(r);
    }
    s_failure = 0;
    int before = s_new;
    r = record("{\"duration_ms\":501}");
    assert(cJSON_IsFalse(cJSON_GetObjectItem(r, "ok")) && s_new == before);
    cJSON_Delete(r);
    r = record("{\"duration_ms\":100.5}");
    assert(cJSON_IsFalse(cJSON_GetObjectItem(r, "ok")) && s_new == before);
    cJSON_Delete(r);
    return 0;
}
