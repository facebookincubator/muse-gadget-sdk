/* Exercise the production compositor, including disjoint strips and cancellation. */
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "muse_3d.h"
#include "muse_pixel.h"

int allocations, fail_at, live_allocations;
static uint16_t before[320*320], after[320*320], full[320*320];
static void read_frame(uint16_t *out, int size)
{
    for (int y = 0; y < size; y += 13) {
        int y1 = y+13 < size ? y+12 : size-1;
        for (int x = 0; x < size; x += 19) {
            int x1 = x+19 < size ? x+18 : size-1;
            muse_pixel_scale(out+y*size+x, size, x, x1, y, y1);
        }
    }
}

int main(int argc, char **argv)
{
    if (argc > 1) fail_at = atoi(argv[1]);
    muse_pixel_set_size(320);
    muse_expression_t wanted = {POSE_BOOT, 0};
    if (fail_at) {
        assert(!live_allocations);
        assert(!muse_3d_update(wanted).count && !muse_3d_preparing());
        memset(after, 0xff, sizeof(after));
        read_frame(after,320);
        assert(!memcmp(before,after,sizeof(before)));
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "bad-boot")) {
        muse_expression_t idle = {POSE_IDLE, 0};
        muse_3d_update(idle);
        while (muse_3d_preparing()) muse_3d_prepare_slice();
        assert(muse_3d_update(idle).count);
        read_frame(before,320);
        assert(!muse_3d_update(wanted).count);
        while (muse_3d_preparing()) muse_3d_prepare_slice();
        assert(!muse_3d_update(wanted).count);
        read_frame(after,320);
        assert(!memcmp(before,after,sizeof(before)));
        return 0;
    }
    /* The abandoned partial boot must never appear on the glass. */
    assert(!muse_3d_update(wanted).count);
    muse_3d_prepare_slice();
    const int sizes[] = {320, 256, 128, 173};
    for (size_t layout = 0; layout < sizeof(sizes) / sizeof(sizes[0]); layout++) {
        int size = sizes[layout];
        muse_pixel_set_size(size);
        for (int p = POSE_COUNT-1; p >= 0; p--) {
            wanted = (muse_expression_t){p,0};
            assert(!muse_3d_update(wanted).count);
            while (muse_3d_preparing()) muse_3d_prepare_slice();
            muse_art_damage_t d = muse_3d_update(wanted);
            assert(d.count == 1 && d.rect[0].w == size && d.rect[0].h == size);
            read_frame(before,size);
            int variants = (int[]){4,2,0,3,2,0,0,0}[p];
            for (int i = 0; i < variants+2; i++) {
                if (i == variants && (p == POSE_ERROR || p == POSE_OFF)) continue;
                wanted.variant = i < variants ? i+1 : i == variants ? 5 : 0;
                d = muse_3d_update(wanted);
                assert(!muse_3d_preparing());
                read_frame(after,size);
                muse_pixel_scale(full,size,0,size-1,0,size-1);
                assert(!memcmp(full,after,size*size*2));
                int changed = 0;
                for (int y=0; y<size; y++) for(int x=0; x<size; x++) {
                    if (after[y*size+x] == before[y*size+x]) continue;
                    bool inside = false;
                    for (unsigned n=0; n<d.count; n++) {
                        muse_art_rect_t r = d.rect[n];
                        inside |= x>=r.x && x<r.x+r.w && y>=r.y && y<r.y+r.h;
                    }
                    assert(inside);
                    changed++;
                }
                if (p != POSE_ERROR && p != POSE_OFF) assert(changed);
                /* Python compares every composited pixel against compiled source art. */
                assert(fwrite(after,sizeof(*after),size*size,stdout) == (size_t)(size*size));
                memcpy(before,after,size*size*2);
                assert(!muse_3d_update(wanted).count);
            }
        }
    }
    assert(allocations == 5);
}
