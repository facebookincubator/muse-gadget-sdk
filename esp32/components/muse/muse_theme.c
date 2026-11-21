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

#include "muse_theme.h"
#if CONFIG_MUSE_UI_THEME
#include "muse_settings.h"
#include <stdatomic.h>
#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#endif
static atomic_uint s_mode,s_hint,s_epoch=1;
static atomic_int s_error;
static atomic_bool s_saving;
void muse_theme_start(void) {atomic_store(&s_mode,muse_settings_theme());}
muse_theme_mode_t muse_theme_mode(void) {return (muse_theme_mode_t)atomic_load(&s_mode);}
uint32_t muse_theme_hint(void) {return atomic_load(&s_hint);}
uint32_t muse_theme_epoch(void) {return atomic_load(&s_epoch);}
int muse_theme_save_error(void) {return atomic_load(&s_error);}
void muse_theme_set_hint(bool valid,uint32_t rgb) {
    unsigned hint=valid?(0x1000000u|(rgb&0xffffff)):0;
    if(atomic_exchange(&s_hint,hint)!=hint)atomic_fetch_add(&s_epoch,1);
}
static void kick(void);
static void save_mode(void *unused) {
    (void)unused;
    for(;;) {
        unsigned wanted=atomic_load(&s_mode);
        esp_err_t error=muse_settings_set_theme((muse_theme_mode_t)wanted);
        atomic_store(&s_error,error);
        if(error!=ESP_OK) {
            if(atomic_compare_exchange_strong(&s_mode,&wanted,(unsigned)muse_settings_theme()))atomic_fetch_add(&s_epoch,1);
            break;
        }
        if(wanted==atomic_load(&s_mode))break;
    }
    atomic_store(&s_saving,false);
    if(muse_theme_mode()!=muse_settings_theme())kick();
#ifdef ESP_PLATFORM
    vTaskDeleteWithCaps(NULL);
#endif
}
static void kick(void) {
    bool expected=false;
    if(!atomic_compare_exchange_strong(&s_saving,&expected,true))return;
#ifdef ESP_PLATFORM
    if(xTaskCreateWithCaps(save_mode,"muse_theme",4096,NULL,2,NULL,MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)!=pdPASS) {
        atomic_store(&s_saving,false);atomic_store(&s_error,ESP_ERR_NO_MEM);
        atomic_store(&s_mode,muse_settings_theme());atomic_fetch_add(&s_epoch,1);
    }
#else
    save_mode(NULL);
#endif
}
void muse_theme_request_mode(muse_theme_mode_t mode) {
    if(mode<MUSE_THEME_AUTO || mode>MUSE_THEME_LIGHT)return;
    if(atomic_exchange(&s_mode,(unsigned)mode)!=(unsigned)mode)atomic_fetch_add(&s_epoch,1);
    kick();
}
#endif
