#include "muse_expression.h"
#include <assert.h>

int main(void)
{
    muse_expression_clock_t c = {0};
    int64_t now = 100;
    unsigned pet = 0;
    for (muse_mode_t mode = MUSE_MODE_BOOT; mode < MUSE_MODE_COUNT; mode++) {
        muse_expression_t expr = muse_expression_tick(&c, mode, false, pet, false, now);
        assert(expr.base == (muse_base_t)mode && expr.variant == 0);
        /* An arbitrarily slow first refresh cannot skip the entry expression. */
        now += 60000;
        assert(muse_expression_tick(&c, mode, false, pet, false, now).variant == 0);
        unsigned expected[5] = {0};
        unsigned holds[5] = {0};
        int steps = 0;
        if (mode == MUSE_MODE_BOOT) {
            for (int i = 0; i < 4; i++) { expected[i] = i+1; holds[i] = 150; }
            steps = 4;
        } else if (mode == MUSE_MODE_SPEAKING || mode == MUSE_MODE_IDLE) {
            unsigned blink[] = {4000, 80, 100, 80};
            for (int i = 0; i < 4; i++) {
                expected[i] = (unsigned[]){1,2,1,0}[i];
                holds[i] = mode == MUSE_MODE_IDLE ? blink[i] : 250;
            }
            steps = 4;
        }
        for (int i = 0; i < steps; i++) {
            muse_expression_presented(&c, expr, now);
            now += holds[i];
            assert(muse_expression_tick(&c, mode, false, pet, false, now-1).variant == expr.variant);
            expr = muse_expression_tick(&c, mode, false, pet, false, now);
            assert(expr.variant == expected[i]);
            now += 60000;
            assert(muse_expression_tick(&c, mode, false, pet, false, now).variant == expected[i]);
        }
        if (!steps || mode == MUSE_MODE_BOOT) {
            muse_expression_presented(&c, expr, now);
            assert(!c.due);
            for (int tick = 0; tick < 10; tick++) {
                now += 400;
                assert(muse_expression_tick(&c, mode, false, pet, false, now).variant == expr.variant);
                muse_expression_presented(&c, expr, now);
                assert(!c.due);
            }
        }
        expr = muse_expression_tick(&c, mode, false, ++pet, true, now);
        assert(expr.variant == (mode == MUSE_MODE_ERROR || mode == MUSE_MODE_OFF ? 0 : MUSE_EXPRESSION_HAPPY));
        expr = muse_expression_tick(&c, mode, false, pet, false, now+1600);
        assert(expr.variant == 0);
    }
    muse_expression_tick(&c, MUSE_MODE_THINKING, false, ++pet, true, now);
    muse_expression_t work = muse_expression_tick(&c, MUSE_MODE_THINKING, true, pet, true, now);
    assert(work.base == POSE_WORKING && !work.variant);
    muse_expression_presented(&c, work, now);
    assert(!c.due);
    assert(muse_expression_tick(&c, MUSE_MODE_THINKING, true, ++pet, true, now).variant == 5);
    assert(muse_expression_tick(&c, MUSE_MODE_THINKING, true, ++pet, true, now+300).variant == 5);
    assert(muse_expression_tick(&c, MUSE_MODE_THINKING, true, pet, false, now+1900).variant == 0);
}
