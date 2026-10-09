// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "muse_pixel.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define W MUSE_PX_W
#define H MUSE_PX_H
#define TAU 6.2831853f

/* ---------------------------------------------------------------------------
 * Palette
 * ------------------------------------------------------------------------- */

enum {
    C_BG = 0,
    C_OUT,       /* outline */
    C_OUT2,      /* soft outline where the hood tucks around the face */
    C_BD,        /* fur dark */
    C_BM,        /* fur mid */
    C_BL,        /* fur light */
    C_BH,        /* fur highlight */
    C_RIM,       /* state-tinted rim light */
    C_SKIND,     /* face panel shade */
    C_SKIN,
    C_SKINL,
    C_IRIS,      /* bead eyes */
    C_SHINE,
    C_BROW,
    C_BLUSH,
    C_BLUSHD,
    C_MOUTH,
    C_TONGUE,
    C_G0,        /* state glow ramp, bright ... */
    C_G1,
    C_G2,
    C_G3,        /* ... deep */
    C_AURA1,
    C_AURA2,
    C_SPK,
    C_ACC,
    C_SHADOW,
    C_HEART,
    C_WHITE,
    C_COUNT,
};

typedef struct {
    float r, g, b;
} rgb_t;

/* Per-mode glow ramp (bright -> deep) and accent. */
typedef struct {
    uint32_t f[4];
    uint32_t acc;
} scheme_t;

static const scheme_t SCHEMES[MUSE_MODE_COUNT] = {
    [MUSE_MODE_BOOT]      = { { 0xffffff, 0xcfe0ff, 0x8fa8ff, 0x5a5fe0 }, 0xa9c0ff },
    [MUSE_MODE_IDLE]      = { { 0xf4e8ff, 0xc7a4ff, 0x9a6bff, 0x5b3fd9 }, 0xa77dff },
    [MUSE_MODE_LISTENING] = { { 0xe8faff, 0x8fdcff, 0x3fa2ff, 0x2a5bd7 }, 0x5cb8ff },
    [MUSE_MODE_THINKING]  = { { 0xffe6ff, 0xff9cf0, 0xd35bff, 0x7a2bd9 }, 0xe07bff },
    [MUSE_MODE_SPEAKING]  = { { 0xeafff4, 0x9ff5cf, 0x3fd9a0, 0x1f9a7a }, 0x6ff0bf },
    [MUSE_MODE_READING]   = { { 0xfff4dc, 0xffdf9e, 0xe0b264, 0x8a6228 }, 0xf2c46d },
    [MUSE_MODE_ERROR]     = { { 0xffd6d6, 0xff6b6b, 0xc7304a, 0x6b1a3a }, 0xff5c5c },
    [MUSE_MODE_OFF]       = { { 0xd8d4ff, 0x8f86d9, 0x5a4fb0, 0x2e2870 }, 0x7c72d0 },
};

/* Cream fur and a peach face. */
static const uint32_t FIXED[C_COUNT] = {
    [C_BG] = 0x000000,
    [C_OUT] = 0x3a2b22,
    [C_OUT2] = 0x8c7560,
    [C_BD] = 0xae987e,
    [C_BM] = 0xcfbc9f,
    [C_BL] = 0xe6d7bd,
    [C_BH] = 0xf8eedc,
    [C_SKIND] = 0xe9cba4,
    [C_SKIN] = 0xf6dfbd,
    [C_SKINL] = 0xfdeed6,
    [C_IRIS] = 0x120d0b,
    [C_SHINE] = 0xffffff,
    [C_BROW] = 0x6b5444,
    [C_BLUSH] = 0xf4aaa0,
    [C_BLUSHD] = 0xea8f8e,
    [C_MOUTH] = 0x3a1f1a,
    [C_TONGUE] = 0xe86a7a,
    [C_SHADOW] = 0x16101f,
    [C_HEART] = 0xff4f8b,
    [C_WHITE] = 0xffffff,
};

static rgb_t s_scheme[5];      /* live, blended: f0..f3, acc */
static bool s_scheme_init;
static uint16_t s_pal[C_COUNT];
static uint16_t s_pal_dim[C_COUNT];

static uint8_t s_fb[W * H];
static uint8_t s_mask[W * H];

static const uint8_t BAYER4[4][4] = {
    { 0, 8, 2, 10 },
    { 12, 4, 14, 6 },
    { 3, 11, 1, 9 },
    { 15, 7, 13, 5 },
};

/*
 * The per-pixel work is fixed point (Q12: ONE = 1.0), with tables for the
 * powers and roots: chips without an FPU (ESP32-C6) emulate float in
 * software, which made a frame take 250 ms. At 64 px the quantisation is
 * invisible.
 */
#define Q 12
#define ONE (1 << Q)
#define QF(v) ((int32_t)((v) * ONE))
#define POW_LUT_N 256
#define POW_LUT_MAX_Q QF(1.2f)          /* body_field() bails out beyond this */
#define SQRT_LUT_N 1024                 /* indexed by x >> 2 */
static int16_t s_pow_dome[POW_LUT_N + 1];   /* |u|^2.7 */
static int16_t s_pow_base[POW_LUT_N + 1];   /* |u|^3.6 */
static int16_t s_sqrt[SQRT_LUT_N + 1];

static void init_luts(void)
{
    for (int i = 0; i <= POW_LUT_N; i++) {
        float u = (float)i * POW_LUT_MAX_Q / POW_LUT_N / ONE;
        s_pow_dome[i] = (int16_t)(powf(u, 2.7f) * ONE);
        s_pow_base[i] = (int16_t)(powf(u, 3.6f) * ONE);
    }
    for (int i = 0; i <= SQRT_LUT_N; i++) {
        s_sqrt[i] = (int16_t)(sqrtf((float)i / SQRT_LUT_N) * ONE);
    }
}

/* a in [0, POW_LUT_MAX_Q] */
static inline int32_t pow_q(const int16_t *lut, int32_t a)
{
    return lut[(a * (POW_LUT_N * 65536 / POW_LUT_MAX_Q)) >> 16];
}

/* sqrt of x in [0, ONE] */
static inline int32_t sqrt_q(int32_t x)
{
    return s_sqrt[x >> 2];
}

static inline int32_t bayer_q(int x, int y)
{
    return BAYER4[y & 3][x & 3] * (ONE / 16) + ONE / 32;
}

static inline float bayer(int x, int y)
{
    return (BAYER4[y & 3][x & 3] + 0.5f) / 16.0f;
}

static inline rgb_t hex_rgb(uint32_t c)
{
    return (rgb_t){ (float)((c >> 16) & 0xff), (float)((c >> 8) & 0xff), (float)(c & 0xff) };
}

static inline rgb_t mix(rgb_t a, rgb_t b, float t)
{
    return (rgb_t){ a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t };
}

static inline rgb_t scale_rgb(rgb_t a, float k)
{
    return (rgb_t){ a.r * k, a.g * k, a.b * k };
}

static inline uint16_t to565(rgb_t c)
{
    int r = (int)(c.r + 0.5f), g = (int)(c.g + 0.5f), b = (int)(c.b + 0.5f);
    r = r < 0 ? 0 : (r > 255 ? 255 : r);
    g = g < 0 ? 0 : (g > 255 ? 255 : g);
    b = b < 0 ? 0 : (b > 255 ? 255 : b);
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

uint32_t muse_pixel_accent(muse_mode_t mode)
{
    return SCHEMES[mode < MUSE_MODE_COUNT ? mode : MUSE_MODE_IDLE].acc;
}

static void update_palette(const scheme_t *target, float dt)
{
    rgb_t tgt[5];
    for (int i = 0; i < 4; i++) {
        tgt[i] = hex_rgb(target->f[i]);
    }
    tgt[4] = hex_rgb(target->acc);

    float k = s_scheme_init ? 1.0f - expf(-dt * 7.0f) : 1.0f;
    for (int i = 0; i < 5; i++) {
        s_scheme[i] = mix(s_scheme[i], tgt[i], k);
    }
    s_scheme_init = true;

    rgb_t pal[C_COUNT];
    for (int i = 0; i < C_COUNT; i++) {
        pal[i] = hex_rgb(FIXED[i]);
    }
    rgb_t acc = s_scheme[4];
    pal[C_G0] = s_scheme[0];
    pal[C_G1] = s_scheme[1];
    pal[C_G2] = s_scheme[2];
    pal[C_G3] = s_scheme[3];
    pal[C_ACC] = acc;
    pal[C_RIM] = mix(pal[C_BL], acc, 0.45f);
    pal[C_AURA1] = scale_rgb(acc, 0.16f);
    pal[C_AURA2] = scale_rgb(acc, 0.34f);
    pal[C_SPK] = mix(acc, pal[C_WHITE], 0.45f);

    for (int i = 0; i < C_COUNT; i++) {
        s_pal[i] = to565(pal[i]);
        /* The block edge shade gives the enlarged pixels a faint grid texture. */
        s_pal_dim[i] = to565(scale_rgb(pal[i], 0.72f));
    }
}

/* ---------------------------------------------------------------------------
 * Primitive helpers
 * ------------------------------------------------------------------------- */

static inline void px(int x, int y, uint8_t c)
{
    if ((unsigned)x < W && (unsigned)y < H) {
        s_fb[y * W + x] = c;
    }
}

static inline uint8_t get_px(int x, int y)
{
    if ((unsigned)x < W && (unsigned)y < H) {
        return s_fb[y * W + x];
    }
    return C_BG;
}

static inline int iround(float v)
{
    return (int)floorf(v + 0.5f);
}

static inline float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline float fracf(float v)
{
    return v - floorf(v);
}

/* Cheap deterministic pseudo-random for idle behaviour. */
static uint32_t s_rng = 0x9e3779b9u;
static float frand(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return (float)(s_rng & 0xffffff) / (float)0x1000000;
}

/* Draw a small bitmap given as rows of '.'/'#'/'o' characters. */
static void stamp(const char *const *rows, int nrows, int x0, int y0, uint8_t fill, uint8_t alt)
{
    for (int r = 0; r < nrows; r++) {
        for (int c = 0; rows[r][c]; c++) {
            char ch = rows[r][c];
            if (ch == '#') {
                px(x0 + c, y0 + r, fill);
            } else if (ch == 'o') {
                px(x0 + c, y0 + r, alt);
            }
        }
    }
}

/* ---------------------------------------------------------------------------
 * Idle behaviour: blinking and gaze
 * ------------------------------------------------------------------------- */

typedef struct {
    float next_blink;
    float blink_start;
    float next_gaze;
    float gx, gy;          /* current gaze, -1..1 */
    float tgx, tgy;        /* target gaze */
    float last_t;
} eyes_t;

static eyes_t s_eyes = { .next_blink = 1.5f, .blink_start = -10, .next_gaze = 1.0f };

static float eyes_update(const muse_pose_t *p, float dt)
{
    eyes_t *e = &s_eyes;

    if (p->t >= e->next_blink) {
        e->blink_start = p->t;
        /* Occasionally double-blink. */
        e->next_blink = p->t + (frand() < 0.2f ? 0.28f : 2.2f + frand() * 3.0f);
    }

    if (p->t >= e->next_gaze) {
        e->next_gaze = p->t + 1.2f + frand() * 2.4f;
        if (frand() < 0.35f) {
            e->tgx = 0;
            e->tgy = 0;
        } else {
            e->tgx = frand() * 2 - 1;
            e->tgy = (frand() * 2 - 1) * 0.6f;
        }
    }

    float tgx = e->tgx, tgy = e->tgy;
    switch (p->mode) {
    case MUSE_MODE_LISTENING:
        tgx = 0;
        tgy = 0.1f;
        break;
    case MUSE_MODE_THINKING:
        tgx = 0.75f * sinf(p->mode_t * 1.3f) + 0.25f;
        tgy = -0.85f;
        break;
    case MUSE_MODE_SPEAKING:
        tgx *= 0.3f;
        tgy = 0;
        break;
    default:
        break;
    }
    float k = 1.0f - expf(-dt * 14.0f);
    e->gx += (tgx - e->gx) * k;
    e->gy += (tgy - e->gy) * k;

    /* Blink curve: 0 = open, 1 = shut. */
    float bt = (p->t - e->blink_start) / 0.16f;
    if (bt < 0 || bt > 1) {
        return 0;
    }
    return 1.0f - fabsf(bt * 2 - 1);
}

/* ---------------------------------------------------------------------------
 * Scene layers
 * ------------------------------------------------------------------------- */

static void draw_aura(float cx, float cy, float radius, float strength)
{
    int x0 = (int)(cx - radius - 1), x1 = (int)(cx + radius + 1);
    int y0 = (int)(cy - radius - 1), y1 = (int)(cy + radius + 1);
    /* Distances in 1/16 px; the root of d2 / r2 comes from the table. */
    int32_t cx16 = (int32_t)(cx * 16), r2 = (int32_t)(radius * radius * 256);
    int32_t to_idx = (int32_t)((float)SQRT_LUT_N * 65536 / r2);
    int32_t str = QF(strength);
    for (int y = y0; y <= y1; y++) {
        int32_t dy = (int32_t)((y + 0.5f - cy) * 1.1f * 16);
        int32_t dy2 = dy * dy;
        if (dy2 >= r2) {
            continue;
        }
        for (int x = x0; x <= x1; x++) {
            int32_t dx = x * 16 + 8 - cx16;
            int32_t d2 = dx * dx + dy2;
            if (d2 >= r2) {
                continue;
            }
            int32_t i = ((ONE - s_sqrt[(d2 * to_idx) >> 16]) * str) >> Q;
            int32_t b = bayer_q(x, y);
            if (i > QF(0.55f) + ((b * QF(0.35f)) >> Q)) {
                px(x, y, C_AURA2);
            } else if (i > (b * QF(0.9f)) >> Q) {
                px(x, y, C_AURA1);
            }
        }
    }
}

/* Expanding dotted rings (listening / speaking). */
static void draw_rings(float cx, float cy, float t, float level, float speed)
{
    for (int k = 0; k < 2; k++) {
        float ph = fracf(t * speed + k * 0.5f);
        float r = 20 + ph * 11;
        float fade = (1 - ph) * (0.35f + level);
        int n = (int)(r * 2.2f);
        for (int i = 0; i < n; i++) {
            float a = i * TAU / n;
            int x = iround(cx + cosf(a) * r);
            int y = iround(cy + sinf(a) * r * 0.92f);
            if (get_px(x, y) == C_BG || get_px(x, y) == C_AURA1) {
                if (bayer(x, y) < fade) {
                    px(x, y, fade > 0.6f ? C_ACC : C_AURA2);
                }
            }
        }
    }
}

static void draw_shadow(float cx, float y, float half_w)
{
    for (int row = 0; row < 3; row++) {
        float hw = half_w * (row == 1 ? 1.0f : 0.72f);
        for (int x = iround(cx - hw); x <= iround(cx + hw); x++) {
            float edge = fabsf(x + 0.5f - cx) / hw;
            if (bayer(x, (int)y + row) > edge * 0.8f) {
                px(x, (int)y + row, C_SHADOW);
            }
        }
    }
}

/* Stable per-position hash (0..65535) so fur tufts don't shimmer as the avatar moves. */
static inline int32_t hash16(int x, int y)
{
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return (int32_t)((h ^ (h >> 16)) & 0xffff);
}

/*
 * The avatar: a tall furry hood shaped like a rounded bullet, with a smooth peach
 * face panel, bead eyes, rosy cheeks, stubby arms and two little feet.
 */
typedef struct {
    float cx, cy;   /* body centre */
    float a, b;     /* half width, half height */
    float fx, fy;   /* face panel centre */
    float fa, fb;   /* face panel half extents */
} avatar_t;

/* Per-row parts of the body and face fields, in Q12. */
typedef struct {
    int32_t v;          /* body: normalised y */
    int32_t inv_a;      /* 1 / half width at this row */
    const int16_t *lut;
    bool body;          /* |v| inside the table */
    int32_t v_pow;      /* |v|^n */
    int32_t fv, fv4;    /* face: normalised y, and its 4th power */
} row_t;

static inline void row_setup(const avatar_t *j, float y, row_t *r)
{
    float v = (y - j->cy) / j->b;
    float a = j->a * (1 + 0.07f * clampf(v, -1, 1));   /* widens a touch at the base */
    r->v = QF(v);
    r->inv_a = QF(1.0f / a);
    r->lut = v < 0 ? s_pow_dome : s_pow_base;
    r->body = abs(r->v) <= POW_LUT_MAX_Q;
    r->v_pow = r->body ? pow_q(r->lut, abs(r->v)) : 0;
    float fv = clampf((y - j->fy) / j->fb, -8, 8);
    r->fv = QF(fv);
    r->fv4 = QF(fminf(fv * fv * fv * fv, 16));
}

/* Superellipse field for the body: round dome on top, squarer bottom. */
static inline int32_t body_field(const row_t *r, int32_t dx, int32_t *ux)
{
    int32_t u = (dx * r->inv_a) >> Q;
    *ux = u;
    if (!r->body || abs(u) > POW_LUT_MAX_Q) {
        return 2 * ONE;
    }
    return pow_q(r->lut, abs(u)) + r->v_pow;
}

/* Fur tone from a surface normal, with vertical streaks for texture. */
static uint8_t fur(int32_t nx, int32_t ny, int x, int y, int ox, int oy)
{
    int32_t r2 = (nx * nx + ny * ny) >> Q;
    int32_t nz = r2 >= ONE ? 0 : sqrt_q(ONE - r2);
    int32_t l = (-QF(0.40f) * nx - QF(0.50f) * ny + QF(0.76f) * nz) >> Q;
    int32_t b = bayer_q(x, y);
    int rx = x - ox, ry = y - oy;
    int32_t streak = (hash16(rx, (ry + (rx & 1) * 2) / 3) >> 4) - ONE / 2;
    int32_t lv = l + (((b - ONE / 2) * QF(0.2f)) >> Q) + ((streak * QF(0.22f)) >> Q);
    if (r2 > QF(0.86f) && ((nx * QF(0.6f) + ny * QF(0.8f)) >> Q) > QF(0.72f) && b < QF(0.4f)) {
        return C_RIM;
    }
    return lv > QF(0.95f) ? C_BH : lv > QF(0.62f) ? C_BL : lv > QF(0.28f) ? C_BM : C_BD;
}

typedef struct {
    float x, y;
    float angle;    /* radians; positive tips the bottom outward to the right */
} limb_t;

/* A limb ellipse, set up for Q12 hit tests. */
typedef struct {
    int32_t x, y, c, s, inv_rx, inv_ry, r;
} limb_q_t;

static void limb_setup(const limb_t *l, float rx, float ry, limb_q_t *q)
{
    q->x = QF(l->x);
    q->y = QF(l->y);
    q->c = QF(cosf(l->angle));
    q->s = QF(sinf(l->angle));
    q->inv_rx = QF(1.0f / rx);
    q->inv_ry = QF(1.0f / ry);
    q->r = QF(rx > ry ? rx : ry);
}

static inline bool in_limb(const limb_q_t *l, int32_t x, int32_t y, int32_t *lx, int32_t *ly)
{
    int32_t dx = x - l->x, dy = y - l->y;
    if (abs(dx) > l->r || abs(dy) > l->r) {
        return false;
    }
    int32_t u = ((((dx * l->c) >> Q) + ((dy * l->s) >> Q)) * l->inv_rx) >> Q;
    int32_t v = ((((dy * l->c) >> Q) - ((dx * l->s) >> Q)) * l->inv_ry) >> Q;
    *lx = u;
    *ly = v;
    return u * u + v * v <= ONE * ONE;
}

enum { M_NONE, M_BODY, M_ARM, M_FOOT, M_FACE };

static void draw_avatar(const avatar_t *j, const limb_t arms[2], const limb_t feet[2])
{
    memset(s_mask, 0, sizeof(s_mask));
    limb_q_t arm_q[2], foot_q[2];
    for (int i = 0; i < 2; i++) {
        limb_setup(&arms[i], 2.9f, 5.2f, &arm_q[i]);
        limb_setup(&feet[i], 4.4f, 2.6f, &foot_q[i]);
    }
    /* Only rows/columns that can hold the avatar are shaded. */
    int x0 = (int)(j->cx - j->a * 1.1f - 8), x1 = (int)(j->cx + j->a * 1.1f + 8);
    int y0 = (int)(j->cy - j->b - 8), y1 = (int)(j->cy + j->b + 5);
    x0 = x0 < 0 ? 0 : x0;
    y0 = y0 < 0 ? 0 : y0;
    x1 = x1 >= W ? W - 1 : x1;
    y1 = y1 >= H ? H - 1 : y1;
    int ox = iround(j->cx), oy = iround(j->cy);
    int32_t cx = QF(j->cx), fcx = QF(j->fx), inv_fa = QF(1.0f / j->fa);

    for (int y = y0; y <= y1; y++) {
        row_t row;
        row_setup(j, y + 0.5f, &row);
        int32_t fy = y * ONE + ONE / 2;
        for (int x = x0; x <= x1; x++) {
            int32_t fx = x * ONE + ONE / 2;
            int32_t lx, ly;

            /* Arms sit in front of the body. */
            bool arm = false;
            for (int a = 0; a < 2 && !arm; a++) {
                if (in_limb(&arm_q[a], fx, fy, &lx, &ly)) {
                    s_mask[y * W + x] = M_ARM;
                    int32_t nx = ((lx * QF(0.85f)) >> Q) + (a ? QF(0.25f) : -QF(0.25f));
                    px(x, y, fur(nx, (ly * QF(0.8f)) >> Q, x, y, ox, oy));
                    arm = true;
                }
            }
            if (arm) {
                continue;
            }

            int32_t ux, uy = row.v;
            int32_t v = body_field(&row, fx - cx, &ux);
            /* Fuzzy silhouette: tufts poke in and out along the edge. */
            int32_t tuft = ((hash16(x - ox, y - oy) - 32768) * QF(0.16f)) >> 16;
            if (v <= ONE + tuft) {
                int32_t fu = ((fx - fcx) * inv_fa) >> Q;
                fu = fu > 3 * ONE ? 3 * ONE : fu < -3 * ONE ? -3 * ONE : fu;
                int32_t fu2 = (fu * fu) >> Q;
                int32_t ff = ((fu2 * fu2) >> Q) + row.fv4;
                int32_t b = bayer_q(x, y);
                if (ff <= ONE) {
                    s_mask[y * W + x] = M_FACE;
                    int32_t fv = row.fv, fv1 = fv + QF(0.15f);
                    uint8_t c = C_SKIN;
                    if (fv < -QF(0.55f) && b < ((-fv - QF(0.45f)) * QF(1.8f)) >> Q) {
                        c = C_SKIND;   /* the hood shades the top of the face */
                    } else if (((fu2 * QF(1.4f)) >> Q) + ((fv1 * fv1) >> Q) < QF(0.32f) && b < QF(0.55f)) {
                        c = C_SKINL;
                    } else if (fv > QF(0.75f) && b < QF(0.4f)) {
                        c = C_SKIND;
                    }
                    px(x, y, c);
                } else {
                    s_mask[y * W + x] = M_BODY;
                    /* Fur darkens where it tucks around the face. */
                    if (ff < QF(1.75f) && (ff < QF(1.3f) || b < ((QF(1.75f) - ff) * QF(1.4f)) >> Q)) {
                        px(x, y, ff < QF(1.3f) ? C_OUT2 : C_BD);
                    } else {
                        px(x, y, fur((ux * QF(0.95f)) >> Q, (uy * QF(0.95f)) >> Q, x, y, ox, oy));
                    }
                }
                continue;
            }

            for (int f = 0; f < 2; f++) {
                if (in_limb(&foot_q[f], fx, fy, &lx, &ly)) {
                    s_mask[y * W + x] = M_FOOT;
                    px(x, y, ly < -QF(0.2f) ? C_BM : C_BD);
                    break;
                }
            }
        }
    }

    /* Hard outline on the silhouette, and seams where parts overlap. */
    static const int8_t N4[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            uint8_t m = s_mask[y * W + x];
            if (m == M_NONE || m == M_FACE) {
                continue;
            }
            for (int k = 0; k < 4; k++) {
                int xx = x + N4[k][0], yy = y + N4[k][1];
                uint8_t n = ((unsigned)xx < W && (unsigned)yy < H) ? s_mask[yy * W + xx] : M_NONE;
                if (n == M_NONE || (m == M_ARM && (n == M_BODY || n == M_FACE)) || (m == M_BODY && n == M_FOOT)) {
                    px(x, y, C_OUT);
                    break;
                }
            }
        }
    }
}

typedef enum {
    EYES_NORMAL,
    EYES_WIDE,
    EYES_HAPPY,
    EYES_X,
} eye_style_t;

/* The eyes are small glossy black beads. */
static void draw_eye(float ex, float ey, float openness, eye_style_t style, float gx, float gy)
{
    int cx = iround(ex + gx * 0.8f), cy = iround(ey + gy * 0.7f);

    if (style == EYES_HAPPY) {
        static const char *const HAPPY[] = { ".##.", "#..#" };
        stamp(HAPPY, 2, iround(ex) - 2, iround(ey), C_IRIS, C_IRIS);
        return;
    }
    if (style == EYES_X) {
        static const char *const XS[] = { "#..#", ".##.", ".##.", "#..#" };
        stamp(XS, 4, iround(ex) - 2, iround(ey) - 1, C_IRIS, C_IRIS);
        return;
    }
    if (openness < 0.3f) {
        static const char *const SHUT[] = { "#..#", ".##." };
        stamp(SHUT, 2, iround(ex) - 2, iround(ey) + 1, C_IRIS, C_IRIS);
        return;
    }

    static const char *const BEAD[] = { ".##.", "#o##", "####", ".##." };
    static const char *const BIG[] = { ".##.", "#o##", "#o##", "####", ".##." };
    const char *const *rows = style == EYES_WIDE ? BIG : BEAD;
    int n = style == EYES_WIDE ? 5 : 4;
    /* Lids close from the top: skip the upper rows as openness drops. */
    int skip = iround((1 - openness) * (n - 1));
    stamp(rows + skip, n - skip, cx - 2, cy - 2 + skip, C_IRIS, skip ? C_IRIS : C_SHINE);
}

static void draw_blush(int x, int y, float strength)
{
    static const char *const CHEEK[] = { ".##.", "####", ".##." };
    for (int j = 0; j < 3; j++) {
        for (int i = 0; i < 4; i++) {
            if (CHEEK[j][i] != '#') {
                continue;
            }
            float b = bayer(x + i, y + j);
            if (b < strength) {
                px(x - 2 + i, y + j, (j == 1 && b < strength * 0.5f) ? C_BLUSHD : C_BLUSH);
            }
        }
    }
}

typedef enum {
    MOUTH_SMILE,
    MOUTH_O,
    MOUTH_HMM,
    MOUTH_TALK,
    MOUTH_GRIN,
    MOUTH_FLAT,
} mouth_t;

static void draw_mouth(int x, int y, mouth_t m, float open)
{
    switch (m) {
    case MOUTH_SMILE: {
        static const char *const S[] = { "#..#", ".##." };
        stamp(S, 2, x - 2, y, C_MOUTH, C_MOUTH);
        break;
    }
    case MOUTH_O: {
        static const char *const S[] = { ".##.", "#oo#", ".##." };
        stamp(S, 3, x - 2, y - 1, C_MOUTH, C_TONGUE);
        break;
    }
    case MOUTH_HMM: {
        static const char *const S[] = { "..#", "##." };
        stamp(S, 2, x - 1, y, C_MOUTH, C_MOUTH);
        break;
    }
    case MOUTH_TALK: {
        int h = 1 + iround(clampf(open, 0, 1) * 3.0f);
        int w = h > 2 ? 4 : 3;
        for (int j = 0; j < h; j++) {
            int inset = (j == 0 || j == h - 1) && h > 2 ? 1 : 0;
            for (int i = inset; i < w - inset; i++) {
                bool tongue = h >= 3 && j == h - 2 && i > inset && i < w - inset - 1;
                px(x - w / 2 + i, y + j, tongue ? C_TONGUE : C_MOUTH);
            }
        }
        break;
    }
    case MOUTH_GRIN: {
        static const char *const S[] = { "#####", ".#o#.", "..#.." };
        stamp(S, 3, x - 2, y, C_MOUTH, C_TONGUE);
        break;
    }
    case MOUTH_FLAT: {
        static const char *const S[] = { "##" };
        stamp(S, 1, x - 1, y + 1, C_MOUTH, C_MOUTH);
        break;
    }
    }
}

static void draw_sparkle(int x, int y, float twinkle, bool front)
{
    uint8_t arm = front ? C_ACC : C_AURA2;
    uint8_t core = front ? C_WHITE : C_SPK;
    if (twinkle > 0.8f) {
        px(x, y, core);
        for (int k = 1; k <= 2; k++) {
            uint8_t c = k == 1 ? (front ? C_SPK : arm) : arm;
            px(x + k, y, c);
            px(x - k, y, c);
            px(x, y + k, c);
            px(x, y - k, c);
        }
    } else if (twinkle > 0.45f) {
        px(x, y, front ? C_SPK : arm);
        px(x + 1, y, arm);
        px(x - 1, y, arm);
        px(x, y + 1, arm);
        px(x, y - 1, arm);
    } else if (twinkle > 0.15f) {
        px(x, y, arm);
    }
}

static void draw_sparkles(const muse_pose_t *p, float cx, float cy, bool front, float speed, int count)
{
    for (int i = 0; i < count; i++) {
        float a = p->t * speed + i * TAU / count;
        float s = sinf(a);
        if ((s > 0) != front) {
            continue;
        }
        float rr = 25.0f + 2.0f * sinf(i * 1.9f + p->t * 0.7f);
        int x = iround(cx + cosf(a) * rr);
        int y = iround(cy - 3 + s * rr * 0.42f);
        float tw = 0.5f + 0.5f * sinf(p->t * 5.0f + i * 1.7f);
        draw_sparkle(x, y, tw, front);
    }
}

/* Sound waves either side of the head. */
static void draw_waves(float cx, float cy, float body_rx, float level, float t)
{
    int n = 1 + (int)(clampf(level, 0, 1) * 3.2f);
    if (n > 3) {
        n = 3;
    }
    for (int k = 0; k < n; k++) {
        float r = body_rx + 5.0f + k * 3.0f;
        int span = 2 + k;
        float flick = 0.5f + 0.5f * sinf(t * 12.0f - k * 1.4f);
        for (int side = -1; side <= 1; side += 2) {
            for (int j = -span; j <= span; j++) {
                float xo = r - (float)(j * j) / (2.0f * r) * 3.0f;
                int x = iround(cx + side * xo);
                int y = iround(cy - 2 + j);
                if (k == 0 || bayer(x, y) < 0.35f + flick * 0.65f) {
                    px(x, y, k == 0 ? C_ACC : (k == 1 ? C_G1 : C_G2));
                }
            }
        }
    }
}

static void draw_thought_dots(float x, float y, float t)
{
    int active = (int)(fracf(t * 1.6f) * 3.0f);
    for (int i = 0; i < 3; i++) {
        int bx = iround(x + i * 4);
        int by = iround(y - i * 3) - (i == active ? 1 : 0);
        uint8_t c = i == active ? C_G0 : C_ACC;
        px(bx, by, c);
        px(bx + 1, by, c);
        px(bx, by + 1, c);
        px(bx + 1, by + 1, i == active ? C_G1 : C_G2);
    }
}

static void draw_hearts(float cx, float top, float t, float amount)
{
    static const char *const HEART[] = { ".#.#.", "#o###", "#####", ".###.", "..#.." };
    for (int i = 0; i < 2; i++) {
        float ph = fracf(t * 0.9f + i * 0.5f);
        if (ph > amount) {
            continue;
        }
        int hx = iround(cx + (i ? 13 : -18) + sinf(ph * TAU + i) * 2);
        int hy = iround(top - ph * 10);
        stamp(HEART, 5, hx, hy, C_HEART, C_WHITE);
    }
}

static void draw_alert(int x, int y)
{
    static const char *const BANG[] = { ".##.", ".##.", ".##.", ".##.", "....", ".##." };
    stamp(BANG, 6, x - 2, y, C_ACC, C_ACC);
}

/* ---------------------------------------------------------------------------
 * Frame
 * ------------------------------------------------------------------------- */

/*
 * Screen pixel -> grid cell, with 0x80 set on a cell's last screen pixel. When
 * cells are 3+ pixels, that edge is drawn dimmer so the pixel grid shows.
 */
#define MAP_MAX 512
static uint8_t s_map[MAP_MAX];
static int s_size;

void muse_pixel_set_size(int px)
{
    s_size = px < MAP_MAX ? px : MAP_MAX;
    bool grid = s_size >= 3 * W;
    for (int i = 0; i < s_size; i++) {
        int cell = i * W / s_size;
        bool edge = grid && (i + 1) * W / s_size != cell;
        s_map[i] = (uint8_t)(cell | (edge ? 0x80 : 0));
    }
}

void muse_pixel_scale(uint16_t *dst, int stride_px, int x0, int x1, int y0, int y1)
{
    int n = x1 - x0 + 1;
    const uint8_t *xmap = &s_map[x0];
    const uint16_t *prev = NULL;
    uint8_t prev_m = 0;
    for (int y = y0; y <= y1; y++, dst += stride_px) {
        uint8_t m = s_map[y];
        if (prev && m == prev_m) {
            memcpy(dst, prev, n * sizeof(uint16_t));
            continue;
        }
        const uint8_t *row = &s_fb[(m & 0x7f) * W];
        if (m & 0x80) {
            for (int i = 0; i < n; i++) {
                dst[i] = s_pal_dim[row[xmap[i] & 0x7f]];
            }
        } else {
            for (int i = 0; i < n; i++) {
                uint8_t xm = xmap[i];
                uint8_t c = row[xm & 0x7f];
                dst[i] = xm & 0x80 ? s_pal_dim[c] : s_pal[c];
            }
        }
        prev = dst;
        prev_m = m;
    }
}

void muse_pixel_render(const muse_pose_t *p)
{
    static bool s_luts;
    if (!s_luts) {
        init_luts();
        s_luts = true;
    }
    float dt = s_eyes.last_t > 0 ? clampf(p->t - s_eyes.last_t, 0, 0.2f) : 0.04f;
    s_eyes.last_t = p->t;

    muse_mode_t mode = p->mode;
    /* Powering down: the avatar waves, then nods off as the glow fades. */
    float fade = mode == MUSE_MODE_OFF ? clampf(1.0f - p->mode_t / 1.3f, 0, 1) : 1.0f;
    float happy = p->happy;
    float level = p->level;
    float t = p->t;

    update_palette(&SCHEMES[mode], dt);
    float blink = eyes_update(p, dt);

    memset(s_fb, C_BG, sizeof(s_fb));

    /* ---- body motion ---- */
    float bob, breathe_rate = 2.0f, lean = 0, hop = 0;
    switch (mode) {
    case MUSE_MODE_LISTENING:
        bob = sinf(t * 3.0f) * 0.6f;
        break;
    case MUSE_MODE_THINKING:
        bob = sinf(t * 2.4f) * 0.8f;
        lean = sinf(t * 1.3f) * 1.2f;
        break;
    case MUSE_MODE_SPEAKING:
        bob = sinf(t * 5.0f) * 0.6f - level * 1.5f;
        break;
    case MUSE_MODE_ERROR:
        bob = 1.0f;
        lean = sinf(t * 18.0f) * (p->mode_t < 0.6f ? 1.0f : 0.0f);
        break;
    default:
        bob = sinf(t * 1.8f) * 1.0f;
        break;
    }
    if (happy > 0) {
        hop = fabsf(sinf(t * 9.0f)) * 3.0f * happy;
    }

    /* Boot: the avatar pops up from a squash, then opens their eyes. */
    float boot = mode == MUSE_MODE_BOOT ? clampf(p->mode_t / 1.4f, 0, 1) : 1.0f;
    float pop = mode == MUSE_MODE_BOOT ? clampf(p->mode_t / 0.6f, 0, 1) : 1.0f;
    float squash = 1.0f - (1.0f - pop) * 0.35f + sinf(pop * 3.1416f) * 0.06f;

    float breathe = sinf(t * breathe_rate + 1.0f) * 0.03f;
    avatar_t j;
    j.a = 16.0f * (1 + breathe) * (2.0f - squash) + level * 0.8f;
    j.b = 23.0f * (1 - breathe) * squash;
    j.cx = 32.0f + lean;
    j.cy = 56.5f - j.b + bob * 0.5f - hop;   /* feet stay near the ground */
    j.fa = j.a * 0.66f;
    j.fb = 7.4f * squash;
    j.fx = j.cx + lean * 0.3f;
    j.fy = j.cy - j.b * 0.30f + bob * 0.3f;

    /* ---- background layers ---- */
    float aura_r = 29.0f + level * 4.0f + sinf(t * 1.5f) * 1.0f;
    float aura_s = (0.75f * boot + level * 0.4f) * fade;
    draw_aura(j.cx, j.cy - 3, aura_r, aura_s);
    if (mode == MUSE_MODE_LISTENING) {
        draw_rings(j.cx, j.fy + 2, t, level, 0.9f);
    } else if (mode == MUSE_MODE_SPEAKING) {
        draw_rings(j.cx, j.fy + 2, t, level, 0.6f);
    }
    draw_shadow(j.cx, 58.5f, 13.0f - hop * 0.8f);

    float spk_speed = mode == MUSE_MODE_THINKING ? 2.8f : mode == MUSE_MODE_LISTENING ? 1.2f
                    : mode == MUSE_MODE_SPEAKING ? 1.5f : 0.6f;
    int spk_count = mode == MUSE_MODE_BOOT ? (int)(boot * 6) : (int)(6 * fade);
    draw_sparkles(p, j.cx, j.cy, false, spk_speed, spk_count);

    /* ---- limbs ---- */
    float base = j.cy + j.b;
    limb_t feet[2];
    float step = mode == MUSE_MODE_SPEAKING ? sinf(t * 5.0f) * 0.6f : 0.0f;
    feet[0] = (limb_t){ j.cx - 7.0f, base - 0.5f + (happy > 0 ? hop * 0.3f : step), -0.15f };
    feet[1] = (limb_t){ j.cx + 7.0f, base - 0.5f + (happy > 0 ? hop * 0.3f : -step), 0.15f };

    limb_t arms[2];
    float adx = j.a + 0.3f;
    float ay = j.cy + 4.0f;
    switch (mode) {
    case MUSE_MODE_LISTENING:
        /* Hands raised beside the face, like cupping an ear. */
        arms[0] = (limb_t){ j.cx - adx + 1.0f, j.fy + 5.0f, 0.55f };
        arms[1] = (limb_t){ j.cx + adx - 1.0f, j.fy + 5.0f, -0.55f };
        break;
    case MUSE_MODE_THINKING:
        /* One paw up to the chin. */
        arms[0] = (limb_t){ j.cx - adx, ay, -0.35f };
        arms[1] = (limb_t){ j.cx + 7.5f, j.fy + j.fb + 3.5f, -1.1f };
        break;
    case MUSE_MODE_OFF: {
        float w = sinf(t * 12.0f) * 0.35f * fade;
        arms[0] = (limb_t){ j.cx - adx, ay, -0.3f };
        arms[1] = (limb_t){ j.cx + adx + 1.0f, j.cy - 4.0f, -2.3f - w };
        break;
    }
    case MUSE_MODE_SPEAKING: {
        float w = sinf(t * 7.0f) * (0.25f + level * 0.45f);
        arms[0] = (limb_t){ j.cx - adx, ay - 1.0f, -0.4f - w };
        arms[1] = (limb_t){ j.cx + adx, ay - 1.0f, 0.4f - w };
        break;
    }
    default: {
        float sway = sinf(t * 1.8f + 0.6f) * 0.08f;
        if (happy > 0) {
            /* Arms up and wiggling. */
            float wig = sinf(t * 14.0f) * 0.25f;
            arms[0] = (limb_t){ j.cx - adx - 1.0f, j.cy - 4.0f, 2.4f + wig };
            arms[1] = (limb_t){ j.cx + adx + 1.0f, j.cy - 4.0f, -2.4f - wig };
        } else {
            arms[0] = (limb_t){ j.cx - adx, ay, -0.3f + sway };
            arms[1] = (limb_t){ j.cx + adx, ay, 0.3f - sway };
        }
        break;
    }
    }
    draw_avatar(&j, arms, feet);

    /* ---- face ---- */
    float eye_y = j.fy - 0.5f;
    float eye_dx = j.fa * 0.48f;
    eye_style_t style = EYES_NORMAL;
    float open = 1.0f - blink;
    mouth_t mouth = MOUTH_SMILE;
    float mouth_open = 0;

    switch (mode) {
    case MUSE_MODE_BOOT:
        open = p->mode_t < 0.9f ? 0.0f : clampf((p->mode_t - 0.9f) / 0.3f, 0, 1);
        break;
    case MUSE_MODE_LISTENING:
        style = EYES_WIDE;
        mouth = MOUTH_O;
        break;
    case MUSE_MODE_THINKING:
        open *= 0.85f;
        mouth = MOUTH_HMM;
        break;
    case MUSE_MODE_SPEAKING:
        mouth = MOUTH_TALK;
        mouth_open = level * 1.3f + 0.1f * (0.5f + 0.5f * sinf(t * 22.0f));
        break;
    case MUSE_MODE_ERROR:
        style = EYES_X;
        mouth = MOUTH_FLAT;
        break;
    case MUSE_MODE_OFF:
        open = clampf((1.0f - p->mode_t / 1.0f) * 1.5f, 0, 1);
        mouth = MOUTH_SMILE;
        break;
    default:
        break;
    }
    if (happy > 0.2f && mode != MUSE_MODE_ERROR) {
        style = EYES_HAPPY;
        mouth = MOUTH_GRIN;
    }

    draw_eye(j.fx - eye_dx, eye_y, open, style, s_eyes.gx, s_eyes.gy);
    draw_eye(j.fx + eye_dx, eye_y, open, style, s_eyes.gx, s_eyes.gy);

    /* Tiny brows for the expressive states. */
    int bl = iround(j.fx - eye_dx), br = iround(j.fx + eye_dx), by = iround(eye_y) - 4;
    if (mode == MUSE_MODE_THINKING) {
        px(bl - 1, by + 1, C_BROW); px(bl, by + 1, C_BROW);
        px(br - 1, by, C_BROW); px(br, by - 1, C_BROW);
    } else if (mode == MUSE_MODE_LISTENING) {
        px(bl - 1, by - 1, C_BROW); px(bl, by - 1, C_BROW);
        px(br - 1, by - 1, C_BROW); px(br, by - 1, C_BROW);
    }

    float blush = 0.55f + happy * 0.45f + (mode == MUSE_MODE_SPEAKING ? 0.15f : 0.0f);
    draw_blush(iround(j.fx - j.fa * 0.72f), iround(eye_y + 2), blush);
    draw_blush(iround(j.fx + j.fa * 0.72f), iround(eye_y + 2), blush);

    draw_mouth(iround(j.fx), iround(eye_y + 3), mouth, mouth_open);

    /* ---- foreground ---- */
    draw_sparkles(p, j.cx, j.cy, true, spk_speed, spk_count);

    float top = j.cy - j.b;
    if (mode == MUSE_MODE_LISTENING || mode == MUSE_MODE_SPEAKING) {
        draw_waves(j.cx, j.fy + 2, j.a, level, t);
    }
    if (mode == MUSE_MODE_THINKING) {
        draw_thought_dots(j.cx + 14, top + 2, t);
    }
    if (happy > 0) {
        draw_hearts(j.cx, top + 1, t, happy);
    }
    if (mode == MUSE_MODE_ERROR) {
        draw_alert(iround(j.cx + 18), iround(top - 1));
    }

}
