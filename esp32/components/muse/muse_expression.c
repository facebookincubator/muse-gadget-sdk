#include "muse_expression.h"

static muse_base_t base_for(muse_mode_t mode, bool working)
{
    static const muse_base_t bases[MUSE_MODE_COUNT] = {
        [MUSE_MODE_BOOT] = POSE_BOOT, [MUSE_MODE_IDLE] = POSE_IDLE,
        [MUSE_MODE_LISTENING] = POSE_LISTENING, [MUSE_MODE_THINKING] = POSE_THINKING,
        [MUSE_MODE_SPEAKING] = POSE_SPEAKING, [MUSE_MODE_ERROR] = POSE_ERROR,
        [MUSE_MODE_OFF] = POSE_OFF,
    };
    if (mode == MUSE_MODE_THINKING && working) return POSE_WORKING;
    return (unsigned)mode < MUSE_MODE_COUNT ? bases[mode] : POSE_IDLE;
}

void muse_expression_presented(muse_expression_clock_t *c, muse_expression_t shown, int64_t now)
{
    if (shown.base != c->wanted.base || shown.variant != c->wanted.variant) return;
    unsigned hold = 0;
    if (shown.variant != MUSE_EXPRESSION_HAPPY) {
        switch (shown.base) {
        case POSE_BOOT:     hold = c->step < 4 ? 150 : 0; break;
        case POSE_IDLE:     hold = (unsigned[]){4000, 80, 100, 80}[c->step]; break;
        case POSE_SPEAKING: hold = 250; break;
        default: break;
        }
    }
    c->due = hold ? now + hold : 0;
}

muse_expression_t muse_expression_tick(muse_expression_clock_t *c, muse_mode_t mode, bool working,
                                       uint32_t pet_event, bool pet_active, int64_t now)
{
    muse_base_t base = base_for(mode, working);
    bool pet = pet_event != c->pet_event && pet_active && base != POSE_ERROR && base != POSE_OFF;
    c->pet_event = pet_event;
    if (!c->initialized || base != c->wanted.base ||
        (c->wanted.variant == MUSE_EXPRESSION_HAPPY && !pet_active)) {
        c->initialized = true;
        c->wanted = (muse_expression_t){base, 0};
        c->step = 0;
        c->due = 0;
    }
    if (pet) {
        c->wanted.variant = MUSE_EXPRESSION_HAPPY;
        c->due = 0;
    } else if (c->due && now >= c->due) {
        c->due = 0;  /* next hold starts only when the panel finishes this one */
        c->step = base == POSE_BOOT ? c->step + 1 : (c->step + 1) % 4;
        c->wanted.variant = c->step;
        if ((base == POSE_IDLE || base == POSE_SPEAKING) && c->step == 3)
            c->wanted.variant = 1;
    }
    return c->wanted;
}
