// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stdint.h>

// Link-side operation count for on-demand power diagnostics.
typedef enum {
    LINK_POWER_DISPLAY,
    LINK_POWER_HARDWARE_COMMAND,
    LINK_POWER_SD,
    LINK_POWER_MICROPHONE,
    LINK_POWER_PROVISIONING,
    LINK_POWER_SETUP_CONTROL,
    LINK_POWER_BUZZER,
    LINK_POWER_OWNER_COUNT,
} link_power_owner_t;

typedef enum {
    LINK_PM_PANEL,
    LINK_PM_STORAGE,
    LINK_PM_MICROPHONE,
    LINK_PM_BUZZER,
    LINK_PM_LOCK_COUNT,
} link_pm_operation_t;

typedef struct {
    uint32_t active_owners;
} link_power_snapshot_t;

void link_power_init(void);
bool link_power_acquire(link_power_owner_t owner);
void link_power_release(link_power_owner_t owner);
link_power_snapshot_t link_power_snapshot(void);

// Online mode: ESP-IDF DFS + automatic Light Sleep. PM locks cover peripheral
// work, never network waits.
bool link_power_enable_online_pm(void);
// Call after wifi_mgr_init(), which creates and starts the STA driver.
bool link_power_enable_wifi_modem_sleep(void);
bool link_power_pm_enter(link_pm_operation_t operation);
void link_power_pm_leave(link_pm_operation_t operation);
// Reads ESP-IDF's opt-in PM profiler only when explicitly queried. Returns
// false when profiling is disabled or a snapshot cannot be obtained.
// raw_dump is caller-owned (free it after serializing). On failure, reason
// distinguishes a missing profiler from capture/parse failures.
bool link_power_light_sleep_stats(uint32_t *count, uint64_t *total_us,
                                  uint32_t *rejected, char **raw_dump,
                                  const char **reason);
