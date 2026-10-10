/*
 * Pet World & 3-Room House Engine for Muse Gadget
 * Native 320x240 High-Resolution Interactive Pet Experience
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WORLD_SCREEN_W 320
#define WORLD_SCREEN_H 240
#define WORLD_STATUS_BAR_H 26

#define ROOM_KITCHEN 0
#define ROOM_LIVING  1
#define ROOM_BEDROOM 2
#define NUM_ROOMS    3

#define ROOM_W 320
#define WORLD_W (ROOM_W * NUM_ROOMS) // 960

#define FLOOR_Y_MIN 160
#define FLOOR_Y_MAX 205

typedef enum {
    PET_ST_IDLE,
    PET_ST_WALK,
    PET_ST_DRAGGED,
    PET_ST_EAT,
    PET_ST_PLAY,
    PET_ST_SLEEP,
    PET_ST_PETTED,
    PET_ST_HAPPY,
    PET_ST_CELEBRATE,
    PET_ST_YAWN,
    PET_ST_TIRED,
    PET_ST_DIZZY,
    PET_ST_LISTENING,
    PET_ST_THINKING,
    PET_ST_SPEAKING
} pet_state_t;

typedef enum {
    PET_GOAL_NONE,
    PET_GOAL_HUNGER,
    PET_GOAL_PLAY_BALL,
    PET_GOAL_WINDOW_GAZE,
    PET_GOAL_COUCH_RELAX,
    PET_GOAL_BEDTIME,
    PET_GOAL_ASK_ATTENTION
} pet_goal_t;

typedef struct {
    float x, y;
    float vx, vy;
    int type; // 0 = heart, 1 = Zzz, 2 = crumb, 3 = star
    int life;
    int max_life;
} pet_particle_t;

typedef struct {
    float x, y;
    float vx, vy;
    float radius;
} toy_ball_t;

typedef struct {
    // World coordinates
    float cam_x;
    float cam_vx;
    bool is_swiping;
    int last_touch_x;
    int touch_start_x;
    int touch_start_y;
    uint32_t touch_start_ms;
    bool is_touching_pet;

    // Pet position and state
    float pet_x;
    float pet_y;
    float pet_vx;
    bool facing_right;
    pet_state_t state;
    pet_goal_t goal;
    uint32_t goal_timer;
    uint32_t next_goal_eval;
    float walk_cycle;

    // Pet drives (0..100)
    int hunger;
    int energy;
    int happiness;

    // Food bowl & Ball
    int food_amount;
    toy_ball_t ball;

    // Particles
    #define MAX_PARTICLES 12
    pet_particle_t particles[MAX_PARTICLES];

    // Voice integration
    float audio_level;
    char speech_text[128];
    uint32_t speech_timer;
    bool is_talk_pressed;

    // Clock & Status
    int cur_hour;
    int cur_min;
    int battery_pct;
    bool wifi_connected;
    bool hatch_connected;
} pet_world_t;

// Initialize pet world in PSRAM
void pet_world_init(void);

// Update simulation (run each frame, e.g. ~40ms)
void pet_world_update(uint32_t now_ms, int muse_mode, float audio_level, const char *caption);

// Voice / talk button state
void pet_world_set_talk_pressed(bool pressed);
bool pet_world_is_talk_pressed(void);

// Handle touch events
void pet_world_touch_down(int tx, int ty);
void pet_world_touch_move(int tx, int ty);
void pet_world_touch_up(void);

// Render entire 320x240 frame into RGB565 frame buffer
void pet_world_render(uint16_t *fb, uint32_t now_ms);

// Direct pointer to internal PSRAM frame buffer (320x240)
uint16_t *pet_world_get_fb(void);

#ifdef __cplusplus
}
#endif
