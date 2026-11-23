#pragma once

#include "muse_expression.h"

typedef struct {
    int x, y, w, h;
} muse_art_rect_t;

typedef struct {
    muse_art_rect_t rect[2];    /* local coordinates, width/height exclusive */
    unsigned count;
} muse_art_damage_t;

/* Called only between LVGL refreshes, on its timer task. A new request
 * cancels unfinished preparation. Only scale() is called by draw workers. */
muse_art_damage_t muse_3d_update(muse_expression_t wanted);
bool muse_3d_preparing(void);
void muse_3d_prepare_slice(void);
