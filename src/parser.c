#include "parser.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Big-endian readers                                                  */
/* ------------------------------------------------------------------ */

static inline uint16_t rd_u16be(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static inline uint32_t rd_u32be(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static inline int32_t rd_i32be(const uint8_t *p) {
    return (int32_t)rd_u32be(p);
}

static inline float rd_f32(const uint8_t *p) {
    uint32_t bits = rd_u32be(p);
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

static inline bool in_range(size_t pos, size_t n, size_t need) {
    return need <= n && pos + need <= n;
}

/* frame_number -> array index (frame -123 is index 0) */
static inline size_t frame_index(int32_t fn) {
    int64_t idx = (int64_t)fn + SLP_FRAME_BASE;
    return idx < 0 ? 0 : (size_t)idx;
}

/* Grows *ptr (element size elem) to hold `need` entries. */
static slp_error_t grow_array(void **ptr, size_t *cap, size_t elem,
                              size_t need) {
    size_t nc = *cap ? *cap : 16;
    if (*ptr && nc >= need) return SLP_OK;
    while (nc < need) nc *= 2;
    void *p = realloc(*ptr, nc * elem);
    if (!p) return SLP_ERR_OUT_OF_MEMORY;
    *ptr = p;
    *cap = nc;
    return SLP_OK;
}

/* ------------------------------------------------------------------ */
/* UBJSON metadata helpers                                             */
/* ------------------------------------------------------------------ */

/* Reads a UBJSON integer (various int types) and advances past it. */
static slp_error_t read_int(const uint8_t *p, size_t n, size_t *pos,
                            uint8_t m, uint64_t *out) {
    if (!in_range(*pos, n, 8)) return SLP_ERR_TRUNCATED_EVENT;
    switch (m) {
        case 'i': /* int8 */
        case 'U': /* uint8 */
        case 'C':
            *out = p[*pos];
            *pos += 1;
            return SLP_OK;
        case 'I': /* int16 */
            *out = rd_u16be(p + *pos);
            *pos += 2;
            return SLP_OK;
        case 'l': /* int32 */
        case 'd': /* float32 */
            *out = rd_u32be(p + *pos);
            *pos += 4;
            return SLP_OK;
        case 'L': /* int64 */
        case 'D': /* float64 */
            *out = 0;
            for (int k = 0; k < 8; k++) *out = (*out << 8) | p[*pos + k];
            *pos += 8;
            return SLP_OK;
        default:
            return SLP_ERR_INVALID_UBJSON;
    }
}

/*
 * Reads a UBJSON string. Strings may be written as
 *   'S' <length-type> <length> <bytes>   (spec)
 *   'U' <length> <bytes>                 (Slippi keys)
 * and writes a NUL-terminated copy into out.
 */
static slp_error_t read_string(const uint8_t *p, size_t n, size_t *pos,
                               char *out, size_t outcap) {
    if (*pos >= n) return SLP_ERR_TRUNCATED_EVENT;
    uint8_t m = p[*pos];
    (*pos)++;

    uint64_t slen;
    if (m == 'S') {
        if (*pos >= n) return SLP_ERR_TRUNCATED_EVENT;
        uint8_t t = p[*pos];
        (*pos)++;
        slp_error_t e = read_int(p, n, pos, t, &slen);
        if (e != SLP_OK) return e;
    } else if (m == 'U' || m == 'C' || m == 'i') {
        if (!in_range(*pos, n, 1)) return SLP_ERR_TRUNCATED_EVENT;
        slen = p[*pos];
        (*pos) += 1;
    } else {
        return SLP_ERR_INVALID_UBJSON;
    }

    if (slen > n - *pos) return SLP_ERR_TRUNCATED_EVENT;
    size_t copy = slen;
    if (copy >= outcap) copy = outcap - 1;
    memcpy(out, p + *pos, copy);
    out[copy] = '\0';
    *pos += (size_t)slen;
    return SLP_OK;
}

/* Skips a full UBJSON value beginning at *pos. */
static slp_error_t skip_value(const uint8_t *p, size_t n, size_t *pos,
                              int depth) {
    if (*pos >= n) return SLP_ERR_TRUNCATED_EVENT;
    if (depth > 128) return SLP_ERR_INVALID_UBJSON;
    uint8_t m = p[*pos];
    (*pos)++;

    switch (m) {
        case 'Z':
        case 'N':
        case 'T':
        case 'F':
            return SLP_OK;
        case 'i':
        case 'U':
        case 'C':
            if (!in_range(*pos, n, 1)) return SLP_ERR_TRUNCATED_EVENT;
            *pos += 1;
            return SLP_OK;
        case 'I':
            if (!in_range(*pos, n, 2)) return SLP_ERR_TRUNCATED_EVENT;
            *pos += 2;
            return SLP_OK;
        case 'l':
        case 'd':
            if (!in_range(*pos, n, 4)) return SLP_ERR_TRUNCATED_EVENT;
            *pos += 4;
            return SLP_OK;
        case 'L':
        case 'D':
            if (!in_range(*pos, n, 8)) return SLP_ERR_TRUNCATED_EVENT;
            *pos += 8;
            return SLP_OK;
        case 'S': {
            if (*pos >= n) return SLP_ERR_TRUNCATED_EVENT;
            uint8_t t = p[*pos];
            if (t == 'i' || t == 'U' || t == 'I' || t == 'l' || t == 'L') {
                (*pos)++;
                uint64_t slen;
                slp_error_t e = read_int(p, n, pos, t, &slen);
                if (e != SLP_OK) return e;
                if (slen > n - *pos) return SLP_ERR_TRUNCATED_EVENT;
                *pos += (size_t)slen;
                return SLP_OK;
            }
            if (!in_range(*pos, n, 1)) return SLP_ERR_TRUNCATED_EVENT;
            uint64_t slen = p[*pos];
            *pos += 1;
            if (slen > n - *pos) return SLP_ERR_TRUNCATED_EVENT;
            *pos += (size_t)slen;
            return SLP_OK;
        }
        case 'H': { /* high precision number: length-prefixed digits */
            if (!in_range(*pos, n, 1)) return SLP_ERR_TRUNCATED_EVENT;
            uint64_t slen = p[*pos];
            *pos += 1;
            if (slen > n - *pos) return SLP_ERR_TRUNCATED_EVENT;
            *pos += (size_t)slen;
            return SLP_OK;
        }
        case '[':
        case '{': {
            uint8_t close = (m == '[') ? ']' : '}';
            bool have_type = false, have_count = false;
            uint8_t elem_type = 0;
            uint64_t count = 0;

            if (*pos < n && p[*pos] == '$') {
                (*pos)++;
                if (*pos >= n) return SLP_ERR_TRUNCATED_EVENT;
                elem_type = p[*pos];
                (*pos)++;
                have_type = true;
            }
            if (*pos < n && p[*pos] == '#') {
                (*pos)++;
                if (*pos >= n) return SLP_ERR_TRUNCATED_EVENT;
                uint8_t t = p[*pos];
                (*pos)++;
                slp_error_t e = read_int(p, n, pos, t, &count);
                if (e != SLP_OK) return e;
                have_count = true;
            }

            if (have_count) {
                if (have_type) {
                    size_t esz;
                    switch (elem_type) {
                        case 'i': case 'U': case 'C': esz = 1; break;
                        case 'I': esz = 2; break;
                        case 'l': case 'd': esz = 4; break;
                        case 'L': case 'D': esz = 8; break;
                        default:
                            return SLP_ERR_INVALID_UBJSON;
                    }
                    if (count > (n - *pos) / esz) return SLP_ERR_TRUNCATED_EVENT;
                    *pos += (size_t)count * esz;
                    return SLP_OK;
                }
                for (uint64_t i = 0; i < count; i++) {
                    slp_error_t e = skip_value(p, n, pos, depth + 1);
                    if (e != SLP_OK) return e;
                }
                return SLP_OK;
            }

            if (have_type)
                return SLP_ERR_INVALID_UBJSON; /* optimized w/o count */

            for (;;) {
                if (*pos >= n) return SLP_ERR_TRUNCATED_EVENT;
                if (p[*pos] == close) {
                    (*pos)++;
                    return SLP_OK;
                }
                if (m == '{') {
                    char key[64];
                    slp_error_t e = read_string(p, n, pos, key, sizeof key);
                    if (e != SLP_OK) return e;
                }
                slp_error_t e = skip_value(p, n, pos, depth + 1);
                if (e != SLP_OK) return e;
            }
        }
        default:
            return SLP_ERR_INVALID_UBJSON;
    }
}

/*
 * Reads the `metadata` key/value pair from the outer document object.
 * `pos` points at the key's string marker. Extracts the fields the basic
 * parser cares about; unknown values are skipped.
 */
static slp_error_t parse_metadata(const uint8_t *p, size_t n, size_t *pos,
                                  slp_replay_t *out) {
    char key[64];
    slp_error_t e = read_string(p, n, pos, key, sizeof key);
    if (e != SLP_OK) return e;
    if (strcmp(key, "metadata") != 0) return SLP_ERR_MISSING_METADATA;

    if (*pos >= n || p[*pos] != '{') return SLP_ERR_MISSING_METADATA;
    (*pos)++;

    for (;;) {
        if (*pos >= n) return SLP_ERR_MISSING_METADATA;
        if (p[*pos] == '}') {
            (*pos)++;
            return SLP_OK;
        }
        e = read_string(p, n, pos, key, sizeof key);
        if (e != SLP_OK) return e;

        if (*pos >= n) return SLP_ERR_MISSING_METADATA;
        uint8_t vm = p[*pos];

        if (strcmp(key, "lastFrame") == 0 && vm == 'l') {
            (*pos)++;
            if (!in_range(*pos, n, 4)) return SLP_ERR_TRUNCATED_EVENT;
            out->last_frame = rd_i32be(p + *pos);
            *pos += 4;
        } else if (strcmp(key, "startAt") == 0) {
            e = read_string(p, n, pos, out->start_at, sizeof out->start_at);
            if (e != SLP_OK) return e;
        } else if (strcmp(key, "playedOn") == 0) {
            e = read_string(p, n, pos, out->played_on, sizeof out->played_on);
            if (e != SLP_OK) return e;
        } else {
            e = skip_value(p, n, pos, 0);
            if (e != SLP_OK) return e;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Header / payload table                                              */
/* ------------------------------------------------------------------ */

/* Locates the raw byte stream and its length. */
static slp_error_t parse_header(const uint8_t *d, size_t len, size_t *raw_off,
                                uint32_t *raw_len_out) {
    static const uint8_t prefix[11] = { '{', 'U', 3, 'r', 'a', 'w',
                                        '[', '$', 'U', '#', 'l' };
    if (len < 15 || memcmp(d, prefix, sizeof prefix) != 0)
        return SLP_ERR_MISSING_RAW;
    uint32_t raw_len = rd_u32be(d + 11);
    if (raw_len == 0) return SLP_ERR_LIVE_GAME;
    if ((size_t)raw_len > len - 15) return SLP_ERR_MISSING_METADATA;
    *raw_off = 15;
    *raw_len_out = raw_len;
    return SLP_OK;
}

/* Parses the event-payload-size table (event 0x35). */
static slp_error_t parse_payload_table(const uint8_t *raw, size_t raw_len,
                                       int32_t sizes[256], size_t *next_pos) {
    for (int i = 0; i < 256; i++) sizes[i] = -1;
    if (raw_len < 2 || raw[0] != 0x35) return SLP_ERR_MISSING_PAYLOAD_TABLE;

    uint8_t plen = raw[1];
    size_t event_size = 1 + plen;
    if (event_size > raw_len) return SLP_ERR_TRUNCATED_EVENT;
    if ((plen - 1) % 3 != 0) return SLP_ERR_INVALID_UBJSON;

    sizes[0x35] = plen;
    size_t pos = 2;
    while (pos + 2 < event_size) {
        uint8_t code = raw[pos];
        sizes[code] = rd_u16be(raw + pos + 1);
        pos += 3;
    }
    *next_pos = event_size;
    return SLP_OK;
}

/* ------------------------------------------------------------------ */
/* Event decoders                                                      */
/* ------------------------------------------------------------------ */

static void decode_game_start(const uint8_t *p, size_t plen,
                              slp_game_start_t *gs) {
    memset(gs, 0, sizeof *gs);
    memcpy(gs->version, p + 1, 4);

    if (plen >= 5 + 312) {
        const uint8_t *gi = p + 5;
        gs->is_teams = gi[0x08] != 0;
        gs->stage_id = rd_u16be(gi + 0x0E);
        gs->timer = rd_u32be(gi + 0x10);
        for (int i = 0; i < SLP_MAX_PORTS; i++) {
            const uint8_t *pl = gi + 0x60 + 0x24 * i;
            gs->player_type[i] = pl[1];
            gs->has_player[i] = pl[1] != 3;
            gs->external_char_id[i] = pl[0];
            gs->costume_index[i] = pl[3];
            gs->stock_count[i] = pl[2];
            gs->team_id[i] = pl[9];
        }
    }
    if (plen >= 0x1A2) gs->pal = p[0x1A1] != 0;

    if (plen >= 0x24B) {
        for (int i = 0; i < SLP_MAX_PORTS; i++) {
            memcpy(gs->name[i], p + 0x1A5 + 0x1F * i, 31);
            gs->name[i][31] = '\0';
            memcpy(gs->connect_code[i], p + 0x221 + 0xA * i, 10);
            gs->connect_code[i][10] = '\0';
        }
    }
}

static void decode_pre(const uint8_t *p, size_t n, slp_frame_t *f) {
    f->frame_number = rd_i32be(p + 1);
    f->player_index = p[5];
    f->is_follower = p[6] != 0;
    f->random_seed = rd_u32be(p + 7);

    if (n >= 0x30) {
        f->pre_action_state = rd_u16be(p + 0xB);
        f->pre_x = rd_f32(p + 0xD);
        f->pre_y = rd_f32(p + 0x11);
        f->pre_facing = rd_f32(p + 0x15);
        f->joystick_x = rd_f32(p + 0x19);
        f->joystick_y = rd_f32(p + 0x1D);
        f->cstick_x = rd_f32(p + 0x21);
        f->cstick_y = rd_f32(p + 0x25);
        f->trigger = rd_f32(p + 0x29);
    }
    if (n >= 0x33) {
        f->processed_buttons = rd_u32be(p + 0x2D);
        f->physical_buttons = rd_u16be(p + 0x31);
    }
    if (n >= 0x39) {
        f->physical_l = rd_f32(p + 0x33);
        f->physical_r = rd_f32(p + 0x37);
    }
    if (n >= 0x40) {
        f->raw_joy_x = (int8_t)p[0x3B];
        f->percent = rd_f32(p + 0x3C);
    }
    if (n >= 0x43) {
        f->raw_joy_y = (int8_t)p[0x40];
        f->raw_cstick_x = (int8_t)p[0x41];
        f->raw_cstick_y = (int8_t)p[0x42];
    }
}

static void decode_post(const uint8_t *p, size_t n, slp_frame_t *f) {
    f->frame_number = rd_i32be(p + 1);
    f->player_index = p[5];
    f->is_follower = p[6] != 0;
    f->character_id = p[7];

    if (n >= 0x10) {
        f->action_state = rd_u16be(p + 8);
        f->x = rd_f32(p + 0xA);
        f->y = rd_f32(p + 0xE);
    }
    if (n >= 0x1A) {
        f->facing = rd_f32(p + 0x12);
        f->percent = rd_f32(p + 0x16);
        f->shield_size = rd_f32(p + 0x1A);
    }
    if (n >= 0x22) {
        f->last_hit_by = p[0x20];
        f->stocks_remaining = p[0x21];
    }
    if (n >= 0x2F) {
        f->anim_frame = rd_f32(p + 0x22);
        for (int i = 0; i < 5; i++) f->state_flags[i] = p[0x26 + i];
        f->hitstun_remaining = rd_f32(p + 0x2B);
    }
    if (n >= 0x31) {
        f->is_airborne = p[0x2F] != 0;
        f->last_ground_id = rd_u16be(p + 0x30);
    }
    if (n >= 0x35) {
        f->jumps_remaining = p[0x32];
        f->l_cancel_status = p[0x33];
        f->vuln_state = p[0x34];
    }
    if (n >= 0x49) {
        f->self_air_x = rd_f32(p + 0x35);
        f->self_air_y = rd_f32(p + 0x39);
        f->attack_x = rd_f32(p + 0x3D);
        f->attack_y = rd_f32(p + 0x41);
        f->ground_x_vel = rd_f32(p + 0x45);
        f->hitlag_frames = rd_f32(p + 0x49);
    }
    if (n >= 0x51) f->animation_index = rd_u32be(p + 0x4D);
    if (n >= 0x55) {
        f->instance_hit_by = rd_u16be(p + 0x51);
        f->instance_id = rd_u16be(p + 0x53);
    }
}

static void decode_item(const uint8_t *p, size_t n, slp_item_t *it) {
    memset(it, 0, sizeof *it);
    it->frame_number = rd_i32be(p + 1);
    if (n >= 0x7) {
        it->type_id = rd_u16be(p + 0x5);
        it->state = p[0x7];
    }
    if (n >= 0x20) {
        it->facing = rd_f32(p + 0x8);
        it->x_vel = rd_f32(p + 0xC);
        it->y_vel = rd_f32(p + 0x10);
        it->x = rd_f32(p + 0x14);
        it->y = rd_f32(p + 0x18);
        it->damage_taken = rd_u16be(p + 0x1C);
        it->expiration_timer = rd_f32(p + 0x1E);
    }
    if (n >= 0x26) it->spawn_id = rd_u32be(p + 0x22);
    if (n >= 0x2B) {
        it->misc[0] = p[0x26];
        it->misc[1] = p[0x27];
        it->misc[2] = p[0x28];
        it->misc[3] = p[0x29];
    }
    if (n >= 0x2C) it->owner = (int8_t)p[0x2A];
    if (n >= 0x2D) it->instance_id = rd_u16be(p + 0x2B);
}

static void decode_fod(const uint8_t *p, size_t n, slp_fod_platform_t *e) {
    memset(e, 0, sizeof *e);
    e->frame_number = rd_i32be(p + 1);
    e->platform = p[0x5];
    if (n >= 0xA) e->height = rd_f32(p + 0x6);
}

static void decode_whispy(const uint8_t *p, size_t n, slp_whispy_blow_t *e) {
    memset(e, 0, sizeof *e);
    e->frame_number = rd_i32be(p + 1);
    if (n >= 0x6) e->direction = p[0x5];
}

static void decode_stadium(const uint8_t *p, size_t n,
                           slp_stadium_transform_t *e) {
    memset(e, 0, sizeof *e);
    e->frame_number = rd_i32be(p + 1);
    if (n >= 0x9) {
        e->event = rd_u16be(p + 0x5);
        e->type = rd_u16be(p + 0x7);
    }
}

static inline unsigned slot_index(const slp_frame_t *f) {
    return (unsigned)f->player_index * 2 + (unsigned)f->is_follower;
}

static slp_error_t slot_grow(slp_slot_t *s, size_t need) {
    size_t cap = s->cap ? s->cap : 1024;
    while (cap < need) cap *= 2;
    if (cap == s->cap) return SLP_OK;
    slp_frame_t *nf = realloc(s->frames, cap * sizeof(slp_frame_t));
    if (!nf) return SLP_ERR_OUT_OF_MEMORY;
    memset(nf + s->cap, 0, (cap - s->cap) * sizeof(slp_frame_t));
    s->frames = nf;
    s->cap = cap;
    return SLP_OK;
}

/*
 * Commits the pending frame for a slot. Frames may be committed multiple
 * times (rollback): a later bookend for the same frame overwrites the
 * earlier one, so the most recently seen version of a frame wins.
 */
static slp_error_t commit_frame(slp_replay_t *out, unsigned slot,
                                int32_t frame_number) {
    slp_slot_t *s = &out->slots[slot];
    if (!s->active) return SLP_OK;

    size_t idx = frame_index(frame_number);
    slp_error_t e = slot_grow(s, idx + 1);
    if (e != SLP_OK) return e;

    memcpy(&s->frames[idx], &out->pending[slot], sizeof(slp_frame_t));
    if (idx + 1 > s->count) s->count = idx + 1;
    if (s->count > out->frame_count) out->frame_count = s->count;
    return SLP_OK;
}

/* ------------------------------------------------------------------ */
/* Item events                                                         */
/* ------------------------------------------------------------------ */

/*
 * Pending items track the most recent state per spawn_id as the file is
 * read. On a frame bookend, every pending item whose frame matches is
 * snapshotted into that frame's item list (replacing any earlier version,
 * so the most recent frame wins).
 */

static slp_item_t *pending_find(const slp_item_t *pending, size_t count,
                                uint32_t spawn_id) {
    for (size_t i = 0; i < count; i++)
        if (pending[i].spawn_id == spawn_id) return (slp_item_t *)&pending[i];
    return NULL;
}

static slp_error_t pending_upsert(slp_item_t **pending, size_t *count,
                                  size_t *cap, const slp_item_t *item) {
    slp_item_t *cur = pending_find(*pending, *count, item->spawn_id);
    if (cur) {
        *cur = *item;
        return SLP_OK;
    }
    slp_error_t e = grow_array((void **)pending, cap, sizeof(slp_item_t),
                               *count + 1);
    if (e != SLP_OK) return e;
    (*pending)[(*count)++] = *item;
    return SLP_OK;
}

static slp_error_t commit_items(slp_replay_t *out, const slp_item_t *pending,
                                size_t pending_count, int32_t fn) {
    size_t idx = frame_index(fn);

    if (idx >= out->frame_items_count) {
        slp_error_t e = grow_array((void **)&out->frame_items,
                                   &out->frame_items_cap,
                                   sizeof(slp_item_list_t), idx + 1);
        if (e != SLP_OK) return e;
        size_t old = out->frame_items_count;
        out->frame_items_count = idx + 1;
        memset(out->frame_items + old, 0,
               (out->frame_items_count - old) * sizeof(slp_item_list_t));
    }

    size_t count = 0;
    for (size_t i = 0; i < pending_count; i++)
        if (pending[i].frame_number == fn) count++;

    slp_item_list_t *list = &out->frame_items[idx];
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->cap = 0;

    if (count == 0) return SLP_OK;
    slp_item_t *items = malloc(count * sizeof(slp_item_t));
    if (!items) return SLP_ERR_OUT_OF_MEMORY;
    size_t j = 0;
    for (size_t i = 0; i < pending_count; i++)
        if (pending[i].frame_number == fn) items[j++] = pending[i];
    list->items = items;
    list->count = j;
    list->cap = j;
    return SLP_OK;
}

/*
 * Stage events are sparse, one slot per frame index. Overwriting the slot
 * on each event gives the most recent value for a frame (rollback-safe).
 */
static slp_error_t stage_event_set(void **arr, size_t *count, size_t *cap,
                                   size_t elem, size_t idx) {
    slp_error_t e = grow_array(arr, cap, elem, idx + 1);
    if (e != SLP_OK) return e;
    if (*count <= idx) {
        memset((uint8_t *)*arr + *count * elem, 0, (idx + 1 - *count) * elem);
        *count = idx + 1;
    }
    return SLP_OK;
}

static void fod_mark_empty(slp_fod_platform_t *arr, size_t from, size_t to) {
    for (size_t i = from; i < to; i++) {
        arr[i].frame_number = INT32_MIN;
        arr[i].platform = (uint8_t)(i & 1);
        arr[i].height = 0.f;
    }
}

static void whispy_mark_empty(slp_whispy_blow_t *arr, size_t from, size_t to) {
    for (size_t i = from; i < to; i++) {
        arr[i].frame_number = INT32_MIN;
        arr[i].direction = 0;
    }
}

static void stadium_mark_empty(slp_stadium_transform_t *arr, size_t from,
                               size_t to) {
    for (size_t i = from; i < to; i++) {
        arr[i].frame_number = INT32_MIN;
        arr[i].event = 0;
        arr[i].type = 0;
    }
}

/* Hold the last recorded height/direction so lookups work on frames where
   the stage did not emit a new 0x3F/0x40/0x41 event. */
static void persist_stage_events(slp_replay_t *r) {
    slp_fod_platform_t fod_last[2] = {0};
    bool fod_have[2] = {false, false};
    for (size_t i = 0; i < r->fod_count; i++) {
        unsigned platform = (unsigned)(i & 1);
        slp_fod_platform_t *e = &r->fod[i];
        if (e->frame_number != INT32_MIN && e->platform == platform) {
            fod_last[platform] = *e;
            fod_have[platform] = true;
        } else if (fod_have[platform]) {
            *e = fod_last[platform];
        }
    }

    slp_whispy_blow_t whispy_last = {0};
    bool whispy_have = false;
    for (size_t i = 0; i < r->whispy_count; i++) {
        slp_whispy_blow_t *e = &r->whispy[i];
        if (e->frame_number != INT32_MIN) {
            whispy_last = *e;
            whispy_have = true;
        } else if (whispy_have) {
            *e = whispy_last;
        }
    }

    slp_stadium_transform_t stadium_last = {0};
    bool stadium_have = false;
    for (size_t i = 0; i < r->stadium_count; i++) {
        slp_stadium_transform_t *e = &r->stadium[i];
        if (e->frame_number != INT32_MIN) {
            stadium_last = *e;
            stadium_have = true;
        } else if (stadium_have) {
            *e = stadium_last;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

slp_error_t slp_parse(const uint8_t *data, size_t len, slp_replay_t *out) {
    memset(out, 0, sizeof *out);
    out->last_frame = INT32_MIN;
    for (unsigned i = 0; i < SLP_SLOT_COUNT; i++)
        out->pending[i].frame_number = INT32_MIN;

    size_t raw_off;
    uint32_t raw_len;
    slp_error_t e = parse_header(data, len, &raw_off, &raw_len);
    if (e != SLP_OK) return e;

    const uint8_t *raw = data + raw_off;
    size_t n = raw_len;

    int32_t sizes[256];
    size_t pos;
    e = parse_payload_table(raw, n, sizes, &pos);
    if (e != SLP_OK) return e;

    size_t meta_pos = raw_off + n;
    e = parse_metadata(data, len, &meta_pos, out);
    if (e != SLP_OK) return e;

    /* pending items, keyed by spawn_id, tracking current frame state */
    slp_item_t *pending = NULL;
    size_t pending_count = 0, pending_cap = 0;
    bool failed = false;
    slp_error_t err = SLP_OK;

    while (pos < n) {
        uint8_t code = raw[pos];
        pos++;
        int32_t sz = sizes[code];
        if (sz < 0) {
            err = SLP_ERR_UNKNOWN_PAYLOAD_SIZE;
            failed = true;
            break;
        }
        if ((size_t)sz > n - pos) {
            err = SLP_ERR_TRUNCATED_EVENT;
            failed = true;
            break;
        }
        const uint8_t *event = raw + pos - 1; /* includes command byte */
        size_t evlen = (size_t)sz + 1;
        pos += (size_t)sz;

        switch (code) {
            case 0x36:
                if (!out->have_game_start) {
                    decode_game_start(event, evlen, &out->game_start);
                    out->have_game_start = true;
                }
                break;
            case 0x37: {
                slp_frame_t tmp;
                memset(&tmp, 0, sizeof tmp);
                decode_pre(event, evlen, &tmp);
                out->pending[slot_index(&tmp)] = tmp;
                out->slots[slot_index(&tmp)].active = true;
                break;
            }
            case 0x38: {
                slp_frame_t tmp;
                memset(&tmp, 0, sizeof tmp);
                decode_post(event, evlen, &tmp);
                out->pending[slot_index(&tmp)] = tmp;
                out->slots[slot_index(&tmp)].active = true;
                break;
            }
            case 0x3B: {
                slp_item_t it;
                decode_item(event, evlen, &it);
                err = pending_upsert(&pending, &pending_count, &pending_cap,
                                     &it);
                if (err != SLP_OK) {
                    failed = true;
                    break;
                }
                break;
            }
            case 0x3F: {
                slp_fod_platform_t e_;
                decode_fod(event, evlen, &e_);
                size_t idx = frame_index(e_.frame_number) * 2 + e_.platform;
                size_t old = out->fod_count;
                err = stage_event_set((void **)&out->fod, &out->fod_count,
                                      &out->fod_cap, sizeof(slp_fod_platform_t),
                                      idx);
                if (err != SLP_OK) {
                    failed = true;
                    break;
                }
                fod_mark_empty(out->fod, old, out->fod_count);
                out->fod[idx] = e_;
                break;
            }
            case 0x40: {
                slp_whispy_blow_t e_;
                decode_whispy(event, evlen, &e_);
                size_t idx = frame_index(e_.frame_number);
                size_t old = out->whispy_count;
                err = stage_event_set((void **)&out->whispy,
                                      &out->whispy_count, &out->whispy_cap,
                                      sizeof(slp_whispy_blow_t), idx);
                if (err != SLP_OK) {
                    failed = true;
                    break;
                }
                whispy_mark_empty(out->whispy, old, out->whispy_count);
                out->whispy[idx] = e_;
                break;
            }
            case 0x41: {
                slp_stadium_transform_t e_;
                decode_stadium(event, evlen, &e_);
                size_t idx = frame_index(e_.frame_number);
                size_t old = out->stadium_count;
                err = stage_event_set((void **)&out->stadium,
                                      &out->stadium_count, &out->stadium_cap,
                                      sizeof(slp_stadium_transform_t), idx);
                if (err != SLP_OK) {
                    failed = true;
                    break;
                }
                stadium_mark_empty(out->stadium, old, out->stadium_count);
                out->stadium[idx] = e_;
                break;
            }
            case 0x3C: { /* frame bookend */
                int32_t fn = rd_i32be(event + 1);
                for (unsigned i = 0; i < SLP_SLOT_COUNT; i++) {
                    if (!out->slots[i].active) continue;
                    if (out->pending[i].frame_number != fn) continue;
                    err = commit_frame(out, i, fn);
                    if (err != SLP_OK) {
                        failed = true;
                        break;
                    }
                }
                if (!failed) {
                    err = commit_items(out, pending, pending_count, fn);
                    if (err != SLP_OK) failed = true;
                }
                break;
            }
            case 0x39: /* game end */
                goto done;
            default:
                break;
        }
        if (failed) break;
    }

done:
    free(pending);
    if (failed) {
        slp_replay_free(out);
        return err;
    }

    /* Flush pending frames that never saw a bookend (older files). */
    for (unsigned i = 0; i < SLP_SLOT_COUNT; i++) {
        if (!out->slots[i].active) continue;
        if (out->pending[i].frame_number == INT32_MIN) continue;
        e = commit_frame(out, i, out->pending[i].frame_number);
        if (e != SLP_OK) {
            slp_replay_free(out);
            return e;
        }
    }
    persist_stage_events(out);
    return SLP_OK;
}

void slp_replay_free(slp_replay_t *r) {
    if (!r) return;
    for (unsigned i = 0; i < SLP_SLOT_COUNT; i++) {
        free(r->slots[i].frames);
        r->slots[i].frames = NULL;
        r->slots[i].cap = 0;
    }
    for (size_t i = 0; i < r->frame_items_count; i++)
        free(r->frame_items[i].items);
    free(r->frame_items);
    free(r->fod);
    free(r->whispy);
    free(r->stadium);
    r->frame_items = NULL;
    r->fod = NULL;
    r->whispy = NULL;
    r->stadium = NULL;
}

const slp_frame_t *slp_frame_at(const slp_replay_t *r, unsigned port,
                                bool follower, int32_t frame_number) {
    if (port >= SLP_MAX_PORTS) return NULL;
    unsigned slot = port * 2 + (follower ? 1u : 0u);
    const slp_slot_t *s = &r->slots[slot];
    if (!s->active || !s->frames) return NULL;
    size_t idx = frame_index(frame_number);
    if (idx >= s->count) return NULL;
    return &s->frames[idx];
}

const slp_item_list_t *slp_items_at(const slp_replay_t *r,
                                    int32_t frame_number) {
    size_t idx = frame_index(frame_number);
    if (idx >= r->frame_items_count) return NULL;
    return &r->frame_items[idx];
}

const slp_fod_platform_t *slp_fod_at(slp_replay_t *r, int32_t frame_number,
                                     unsigned platform) {
    if (!r || !r->fod || platform > 1) return NULL;
    size_t idx = frame_index(frame_number) * 2 + platform;
    if (idx >= r->fod_count) {
        if (r->fod_count <= platform) return NULL;
        idx = r->fod_count - 1;
        if ((idx & 1) != platform) {
            if (idx == 0) return NULL;
            idx--;
        }
    }
    const slp_fod_platform_t *e = &r->fod[idx];
    if (e->frame_number == INT32_MIN || e->platform != platform) return NULL;
    return e;
}

const slp_whispy_blow_t *slp_whispy_at(slp_replay_t *r, int32_t frame_number) {
    if (!r || !r->whispy) return NULL;
    size_t idx = frame_index(frame_number);
    if (idx >= r->whispy_count) {
        if (!r->whispy_count) return NULL;
        idx = r->whispy_count - 1;
    }
    const slp_whispy_blow_t *e = &r->whispy[idx];
    if (e->frame_number == INT32_MIN) return NULL;
    return e;
}

const slp_stadium_transform_t *slp_stadium_at(slp_replay_t *r,
                                              int32_t frame_number) {
    if (!r || !r->stadium) return NULL;
    size_t idx = frame_index(frame_number);
    if (idx >= r->stadium_count) {
        if (!r->stadium_count) return NULL;
        idx = r->stadium_count - 1;
    }
    const slp_stadium_transform_t *e = &r->stadium[idx];
    if (e->frame_number == INT32_MIN) return NULL;
    return e;
}

/* ------------------------------------------------------------------ */
/* Names                                                               */
/* ------------------------------------------------------------------ */

static const char *const character_names[27] = {
    "Mario",          "Fox",            "CaptainFalcon", "DonkeyKong",
    "Kirby",          "Bowser",         "Link",          "Sheik",
    "Ness",           "Peach",          "Popo",          "Nana",
    "Pikachu",        "Samus",          "Yoshi",         "Jigglypuff",
    "Mewtwo",         "Luigi",          "Marth",         "Zelda",
    "YoungLink",      "DrMario",        "Falco",         "Pichu",
    "MrGameAndWatch", "Ganondorf",      "Roy"};

static const struct {
    uint16_t id;
    const char *name;
} stage_names[] = {
    {2, "Fountain of Dreams"},      {3, "Pokemon Stadium"},
    {4, "Princess Peach's Castle"}, {5, "Kongo Jungle"},
    {6, "Brinstar"},                {7, "Corneria"},
    {8, "Yoshi's Story"},           {9, "Onett"},
    {10, "Mute City"},              {11, "Rainbow Cruise"},
    {12, "Jungle Japes"},           {13, "Great Bay"},
    {14, "Hyrule Temple"},          {15, "Brinstar Depths"},
    {16, "Yoshi's Island"},         {17, "Green Greens"},
    {18, "Fourside"},               {19, "Mushroom Kingdom I"},
    {20, "Mushroom Kingdom II"},    {22, "Venom"},
    {23, "Poke Floats"},            {24, "Big Blue"},
    {25, "Icicle Mountain"},        {27, "Flat Zone"},
    {28, "Dream Land 64"},          {29, "Yoshi's Island N64"},
    {30, "Kongo Jungle N64"},       {31, "Battlefield"},
    {32, "Final Destination"},
};

const char *slp_character_name(uint8_t id) {
    return id < 27 ? character_names[id] : "?";
}

const char *slp_stage_name(uint16_t id) {
    for (size_t i = 0; i < sizeof stage_names / sizeof stage_names[0]; i++)
        if (stage_names[i].id == id) return stage_names[i].name;
    return "?";
}

uint8_t slp_external_to_internal(uint8_t ext) {
    static const uint8_t map[26] = {
        /*  0 */ 2,  /* CaptainFalcon */
        /*  1 */ 3,  /* DonkeyKong    */
        /*  2 */ 1,  /* Fox           */
        /*  3 */ 24, /* MrGameAndWatch*/
        /*  4 */ 4,  /* Kirby         */
        /*  5 */ 5,  /* Bowser        */
        /*  6 */ 6,  /* Link          */
        /*  7 */ 17, /* Luigi         */
        /*  8 */ 0,  /* Mario         */
        /*  9 */ 18, /* Marth         */
        /* 10 */ 16, /* Mewtwo        */
        /* 11 */ 8,  /* Ness          */
        /* 12 */ 9,  /* Peach         */
        /* 13 */ 12, /* Pikachu       */
        /* 14 */ 10, /* Popo          */
        /* 15 */ 15, /* Jigglypuff    */
        /* 16 */ 13, /* Samus         */
        /* 17 */ 14, /* Yoshi         */
        /* 18 */ 19, /* Zelda         */
        /* 19 */ 7,  /* Sheik         */
        /* 20 */ 22, /* Falco         */
        /* 21 */ 20, /* YoungLink     */
        /* 22 */ 21, /* DrMario       */
        /* 23 */ 26, /* Roy           */
        /* 24 */ 23, /* Pichu         */
        /* 25 */ 25, /* Ganondorf     */
    };
    return ext < 26 ? map[ext] : 0xFF;
}

static const struct {
    uint16_t id;
    const char *name;
} item_names[] = {
    {0x00, "Capsule"},           {0x01, "Box"},
    {0x02, "Barrel"},            {0x03, "Egg"},
    {0x04, "Party Ball"},        {0x05, "Barrel Cannon"},
    {0x06, "Bob-omb"},           {0x07, "Mr. Saturn"},
    {0x08, "Heart Container"},   {0x09, "Maxim Tomato"},
    {0x0A, "Starman"},           {0x0B, "Home Run Bat"},
    {0x0C, "Beam Sword"},        {0x0D, "Parasol"},
    {0x0E, "Green Shell"},       {0x0F, "Red Shell"},
    {0x10, "Ray Gun"},           {0x11, "Freezie"},
    {0x12, "Food"},              {0x13, "Motion Sensor Bomb"},
    {0x14, "Flipper"},           {0x15, "Super Scope"},
    {0x16, "Star Rod"},          {0x17, "Lip's Stick"},
    {0x18, "Fan"},               {0x19, "Fire Flower"},
    {0x1A, "Super Mushroom"},    {0x1B, "Poison Mushroom"},
    {0x1C, "Hammer"},            {0x1D, "Warp Star"},
    {0x1E, "Screw Attack"},      {0x1F, "Bunny Hood"},
    {0x20, "Metal Box"},         {0x21, "Cloaking Device"},
    {0x22, "Poke Ball"},
    {0x36, "Fox Laser"},         {0x37, "Falco Laser"},
    {0x38, "Fox Illusion"},      {0x39, "Falco Phantasm"},
};

const char *slp_item_name(uint16_t id) {
    for (size_t i = 0; i < sizeof item_names / sizeof item_names[0]; i++)
        if (item_names[i].id == id) return item_names[i].name;
    return "?";
}

const char *slp_error_string(slp_error_t e) {
    switch (e) {
        case SLP_OK: return "ok";
        case SLP_ERR_FILE: return "file error";
        case SLP_ERR_INVALID_UBJSON: return "invalid ubjson";
        case SLP_ERR_MISSING_RAW: return "missing raw block";
        case SLP_ERR_LIVE_GAME: return "live game (raw len 0)";
        case SLP_ERR_MISSING_METADATA: return "missing metadata";
        case SLP_ERR_MISSING_PAYLOAD_TABLE: return "missing payload table";
        case SLP_ERR_UNKNOWN_PAYLOAD_SIZE: return "unknown payload size";
        case SLP_ERR_TRUNCATED_EVENT: return "truncated event";
        case SLP_ERR_OUT_OF_MEMORY: return "out of memory";
    }
    return "unknown error";
}
