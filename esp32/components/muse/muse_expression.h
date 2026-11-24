/* Presentation clock for the bundled 3D artwork. All times are milliseconds. */
#pragma once

#include "muse_state.h"

typedef enum {
    POSE_BOOT, POSE_IDLE, POSE_LISTENING, POSE_THINKING, POSE_SPEAKING,
    POSE_ERROR, POSE_OFF, POSE_WORKING, POSE_COUNT
} muse_base_t;

enum { MUSE_EXPRESSION_HAPPY = 5, MUSE_EXPRESSION_COUNT };
typedef struct {
    muse_base_t base;
    unsigned variant;       /* zero is the base's normal expression */
} muse_expression_t;

typedef struct {
    muse_expression_t wanted;
    unsigned step;
    uint32_t pet_event;
    int64_t due;
    bool initialized;
} muse_expression_clock_t;

muse_expression_t muse_expression_tick(muse_expression_clock_t *c, muse_mode_t mode, bool working,
                                       uint32_t pet_event, bool pet_active, int64_t now);
void muse_expression_presented(muse_expression_clock_t *c, muse_expression_t shown, int64_t now);
