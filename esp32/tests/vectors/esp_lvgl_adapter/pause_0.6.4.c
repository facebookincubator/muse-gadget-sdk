/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * esp_lv_adapter_pause() from espressif/esp_lvgl_adapter 0.6.4
 * (src/adapter/esp_lv_adapter.c, esp-iot-solution commit 5d75f3f0), verbatim:
 * tests/test_lvgl_adapter_fix.py applies cmake/esp_lvgl_adapter_fix.cmake's
 * edits to it and runs the result against the adapter's worker loop.
 */

esp_err_t esp_lv_adapter_pause(int32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(s_ctx.inited, ESP_ERR_INVALID_STATE, TAG, "Adapter not initialized");
    if (s_ctx.paused) {
        return ESP_OK;
    }

    bool called_from_worker = (s_ctx.task && xTaskGetCurrentTaskHandle() == s_ctx.task);

    s_ctx.pause_ack = false;
    s_ctx.paused = true;

    if (called_from_worker) {
        /* Avoid deadlock when pause is requested from the LVGL worker itself */
        s_ctx.pause_ack = true;
        if (s_ctx.pause_done_sem) {
            xSemaphoreGive(s_ctx.pause_done_sem);
        }
        /* Stop the tick timer so tickless light sleep can enter naturally. */
        (void)adapter_stop_tick_timer();
        return ESP_OK;
    }

    if (!s_ctx.task || !s_ctx.pause_done_sem) {
        s_ctx.pause_ack = true;
        (void)adapter_stop_tick_timer();
        return ESP_OK;
    }

    xSemaphoreTake(s_ctx.pause_done_sem, 0); /* clear pending */
    TickType_t ticks = (timeout_ms < 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    if (xSemaphoreTake(s_ctx.pause_done_sem, ticks) != pdTRUE) {
        s_ctx.paused = false;
        return ESP_ERR_TIMEOUT;
    }
    (void)adapter_stop_tick_timer();
    return ESP_OK;
}

