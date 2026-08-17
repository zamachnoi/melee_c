#ifndef PARSER_H
#define PARSER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SLP_MAX_PORTS 4
#define SLP_SLOT_COUNT 8      /* 4 ports * (leader + follower) */
#define SLP_FRAME_BASE 123    /* frame -123 maps to index 0 */

typedef enum {
    SLP_OK = 0,
    SLP_ERR_FILE,
    SLP_ERR_INVALID_UBJSON,
    SLP_ERR_MISSING_RAW,
    SLP_ERR_LIVE_GAME,
    SLP_ERR_MISSING_METADATA,
    SLP_ERR_MISSING_PAYLOAD_TABLE,
    SLP_ERR_UNKNOWN_PAYLOAD_SIZE,
    SLP_ERR_TRUNCATED_EVENT,
    SLP_ERR_OUT_OF_MEMORY,
} slp_error_t;

typedef struct {
    uint8_t version[4]; /* major.minor.build.unused */
    uint16_t stage_id;
    uint32_t timer;
    bool is_teams;
    bool pal;
    bool has_player[SLP_MAX_PORTS]; /* player_type != empty */
    uint8_t player_type[SLP_MAX_PORTS];
    uint8_t external_char_id[SLP_MAX_PORTS];
    uint8_t costume_index[SLP_MAX_PORTS];
    uint8_t stock_count[SLP_MAX_PORTS];
    uint8_t team_id[SLP_MAX_PORTS];
    char name[SLP_MAX_PORTS][32];
    char connect_code[SLP_MAX_PORTS][16];
} slp_game_start_t;

typedef struct {
    int32_t frame_number;
    uint8_t player_index;
    uint8_t is_follower;
    uint8_t character_id;   /* internal id, from post-frame */
    uint16_t action_state;  /* raw state id, from post-frame */
    uint16_t pre_action_state;
    float x, y;             /* post-frame position */
    float facing;           /* post-frame facing */
    float pre_x, pre_y;
    float pre_facing;
    float percent;
    float shield_size;
    float anim_frame;
    float hitstun_remaining;
    uint8_t stocks_remaining;
    uint8_t last_hit_by;
    uint8_t jumps_remaining;
    uint8_t l_cancel_status;
    uint8_t vuln_state;
    bool is_airborne;
    uint8_t state_flags[5];
    uint16_t last_ground_id;
    float self_air_x, self_air_y;
    float attack_x, attack_y;
    float ground_x_vel;
    float hitlag_frames;
    uint32_t animation_index;
    uint16_t instance_hit_by;
    uint16_t instance_id;
    /* pre-frame controller data */
    float joystick_x, joystick_y;
    float cstick_x, cstick_y;
    float trigger;
    float physical_l, physical_r;
    uint32_t processed_buttons;
    uint16_t physical_buttons;
    uint32_t random_seed;
    int8_t raw_joy_x, raw_joy_y, raw_cstick_x, raw_cstick_y;
} slp_frame_t;

typedef struct {
    slp_frame_t *frames;
    size_t count; /* number of committed frames, index 0 = frame -123 */
    size_t cap;
    bool active;
} slp_slot_t;

typedef struct {
    uint32_t spawn_id;
    int32_t frame_number;
    uint16_t type_id;
    uint8_t state;
    float facing;
    float x_vel, y_vel;
    float x, y;
    uint16_t damage_taken;
    float expiration_timer;
    uint8_t misc[4];
    int8_t owner;
    uint16_t instance_id;
} slp_item_t;

typedef struct {
    slp_item_t *items;
    size_t count;
    size_t cap;
} slp_item_list_t;

typedef struct {
    int32_t frame_number;
    uint8_t platform; /* 0 = right, 1 = left */
    float height;
} slp_fod_platform_t;

typedef struct {
    int32_t frame_number;
    uint8_t direction; /* 0 = none, 1 = left, 2 = right */
} slp_whispy_blow_t;

typedef struct {
    int32_t frame_number;
    uint16_t event; /* 2 init ... 6 finalize, 0 finished */
    uint16_t type;  /* 3 fire, 4 grass, 5 normal, 6 rock, 9 water */
} slp_stadium_transform_t;

typedef struct {
    slp_game_start_t game_start;
    bool have_game_start;
    slp_slot_t slots[SLP_SLOT_COUNT];
    slp_frame_t pending[SLP_SLOT_COUNT]; /* in-progress frame per slot */
    int32_t last_frame; /* from metadata; INT32_MIN if absent */
    char start_at[64];
    char played_on[16];
    size_t frame_count; /* max committed frame count across slots */

    /* one item list per frame index (rollback-safe: list replaced each
       bookend so the most recent version of a frame wins) */
    slp_item_list_t *frame_items;
    size_t frame_items_count;
    size_t frame_items_cap;

    /* stage events, one slot per frame index (overwrite = latest wins).
       Empty FoD/Whispy/Stadium slots use frame_number == INT32_MIN.
       slp_fod_at returns the last recorded height at or before the frame. */
    slp_fod_platform_t *fod;      /* index = frame_index * 2 + platform */
    size_t fod_count;
    size_t fod_cap;
    slp_whispy_blow_t *whispy;    /* index = frame_index */
    size_t whispy_count;
    size_t whispy_cap;
    slp_stadium_transform_t *stadium; /* index = frame_index */
    size_t stadium_count;
    size_t stadium_cap;
} slp_replay_t;

slp_error_t slp_parse(const uint8_t *data, size_t len, slp_replay_t *out);
void slp_replay_free(slp_replay_t *r);

const slp_frame_t *slp_frame_at(const slp_replay_t *r, unsigned port,
                                bool follower, int32_t frame_number);

const slp_item_list_t *slp_items_at(const slp_replay_t *r,
                                    int32_t frame_number);
/* Last 0x3F height for this platform at or before frame_number, or NULL
   if it has not moved yet. */
const slp_fod_platform_t *slp_fod_at(slp_replay_t *r, int32_t frame_number,
                                     unsigned platform);
const slp_whispy_blow_t *slp_whispy_at(slp_replay_t *r, int32_t frame_number);
const slp_stadium_transform_t *slp_stadium_at(slp_replay_t *r,
                                              int32_t frame_number);

const char *slp_character_name(uint8_t internal_id);
const char *slp_stage_name(uint16_t stage_id);
const char *slp_item_name(uint16_t type_id);
const char *slp_error_string(slp_error_t e);
uint8_t slp_external_to_internal(uint8_t external_id);

#ifdef __cplusplus
}
#endif

#endif /* PARSER_H */
