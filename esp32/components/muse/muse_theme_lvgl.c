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

#include "muse_theme_lvgl.h"
#if CONFIG_MUSE_UI_THEME
/* LVGL 9.5 is pinned by this SDK; as in the image decoder, use its private
 * object layout to replace only styles this module owns. User data, app
 * styles, local geometry, and input state remain untouched. */
#include "src/core/lv_obj_private.h"
#include "src/core/lv_obj_style_private.h"
#include <stdlib.h>
#include <stdatomic.h>
typedef struct entry {
    lv_style_t style;
    uint32_t legacy,surface;
    lv_style_prop_t prop;
    bool foreground;
    struct entry *next;
} entry_t;
static entry_t *s_styles;
static muse_theme_palette_t s_palette={false,0,true};
static atomic_uint s_background;
static uint32_t s_epoch;
static bool s_light,s_applied;
static bool foreground(lv_style_prop_t prop,lv_style_selector_t sel) {
    lv_part_t part=lv_obj_style_get_selector_part(sel);
    return prop!=LV_STYLE_BG_COLOR || part==LV_PART_INDICATOR || part==LV_PART_KNOB;
}
static void update(entry_t *e) {
    uint32_t color=e->prop==LV_STYLE_TEXT_COLOR?muse_theme_text_on(&s_palette,e->legacy,e->surface):muse_theme_map(&s_palette,e->legacy,e->foreground);
    lv_style_value_t v={.color=lv_color_hex(color)};
    lv_style_set_prop(&e->style,e->prop,v);
}
/* Text on a filled control needs that control's ink contrast, rather than
 * the screen's. Keep the original surface key so both colors update together. */
static uint32_t text_surface(lv_obj_t *obj) {
    for(lv_obj_t *p=obj;p;p=lv_obj_get_parent(p)) {
        if(lv_obj_get_style_bg_opa(p,LV_PART_MAIN)<LV_OPA_50)continue;
        for(unsigned i=0;i<p->style_cnt;i++) {
            if(lv_obj_style_get_selector_part(p->styles[i].selector)!=LV_PART_MAIN)continue;
            lv_state_t required=lv_obj_style_get_selector_state(p->styles[i].selector);
            if((required&lv_obj_get_state(p))!=required)continue;
            for(entry_t *e=s_styles;e;e=e->next)if(&e->style==p->styles[i].style && e->prop==LV_STYLE_BG_COLOR)return e->legacy;
        }
        return lv_color_to_u32(lv_obj_get_style_bg_color(p,LV_PART_MAIN))&0xffffff;
    }
    return 0;
}
static void bind(lv_obj_t *obj,lv_style_prop_t prop,lv_color_t color,lv_style_selector_t selector) {
    uint32_t legacy=lv_color_to_u32(color)&0xffffff;
    bool fg=foreground(prop,selector);
    uint32_t surface=prop==LV_STYLE_TEXT_COLOR?text_surface(obj):0;
    entry_t *target=s_styles;
    while(target && (target->legacy!=legacy || target->prop!=prop || target->foreground!=fg || target->surface!=surface))target=target->next;
    if(!target) {
        target=lv_malloc_zeroed(sizeof(*target));
        if(!target) {lv_style_value_t v={.color=lv_color_hex(muse_theme_map(&s_palette,legacy,fg))};lv_obj_set_local_style_prop(obj,prop,v,selector);return;}
        target->legacy=legacy;target->prop=prop;target->foreground=fg;target->surface=surface;
        lv_style_init(&target->style);update(target);target->next=s_styles;s_styles=target;
    }
    /* One owned style per property/selector. Repeated meter/state updates
     * neither grow the style list nor invalidate an unchanged object. */
    for(unsigned i=0;i<obj->style_cnt;i++) {
        if(obj->styles[i].selector!=selector)continue;
        if(obj->styles[i].style==&target->style)return;
        for(entry_t *e=s_styles;e;e=e->next)if(obj->styles[i].style==&e->style && e->prop==prop) {
            lv_obj_remove_style(obj,&e->style,selector);goto removed;
        }
    }
removed:
    lv_obj_remove_local_style_prop(obj,prop,selector);
    lv_obj_add_style(obj,&target->style,selector);
}
uint32_t muse_theme_background(void) {return atomic_load(&s_background);}
bool muse_theme_lvgl_tick(void) {
    if(!lv_display_get_default())return false;
    uint32_t epoch=muse_theme_epoch();
    if(epoch==s_epoch)return false;
    s_epoch=epoch;uint32_t hint=muse_theme_hint();
    s_palette=muse_theme_palette(muse_theme_mode(),(hint&0x1000000u)!=0,hint);
    atomic_store(&s_background,s_palette.background);
#if LV_USE_THEME_DEFAULT
    if(!s_applied || s_palette.light!=s_light) {
        lv_obj_t *screen=lv_screen_active();
        lv_theme_t *theme=lv_theme_default_init(lv_display_get_default(),lv_theme_get_color_primary(screen),
            lv_theme_get_color_secondary(screen),!s_palette.light,lv_theme_get_font_normal(screen));
        lv_display_set_theme(lv_display_get_default(),theme);
    }
#endif
    s_light=s_palette.light;s_applied=true;
    for(entry_t *e=s_styles;e;e=e->next) {update(e);lv_obj_report_style_change(&e->style);}
    return true;
}
void muse_theme_lvgl_init(void) {muse_theme_start();muse_theme_lvgl_tick();}
void muse_theme_set_style_bg_color(lv_obj_t *o,lv_color_t c,lv_style_selector_t s) {bind(o,LV_STYLE_BG_COLOR,c,s);}
void muse_theme_set_style_text_color(lv_obj_t *o,lv_color_t c,lv_style_selector_t s) {bind(o,LV_STYLE_TEXT_COLOR,c,s);}
void muse_theme_set_style_arc_color(lv_obj_t *o,lv_color_t c,lv_style_selector_t s) {bind(o,LV_STYLE_ARC_COLOR,c,s);}
void muse_theme_set_style_border_color(lv_obj_t *o,lv_color_t c,lv_style_selector_t s) {bind(o,LV_STYLE_BORDER_COLOR,c,s);}
void muse_theme_set_style_outline_color(lv_obj_t *o,lv_color_t c,lv_style_selector_t s) {bind(o,LV_STYLE_OUTLINE_COLOR,c,s);}
void muse_theme_set_style_line_color(lv_obj_t *o,lv_color_t c,lv_style_selector_t s) {bind(o,LV_STYLE_LINE_COLOR,c,s);}
void muse_theme_set_style_image_recolor(lv_obj_t *o,lv_color_t c,lv_style_selector_t s) {bind(o,LV_STYLE_IMAGE_RECOLOR,c,s);}
#endif
