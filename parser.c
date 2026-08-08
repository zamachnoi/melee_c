#include "parser.h"

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
 * Walks the top-level metadata object, extracting the fields the basic
 * parser cares about. Unknown values are skipped.
 */
static slp_error_t parse_metadata(const uint8_t *p, size_t n, size_t *pos,
                                  slp_replay_t *out) {
    if (*pos >= n || p[*pos] != '{') return SLP_ERR_MISSING_METADATA;
    (*pos)++;

    for (;;) {
        if (*pos >= n) return SLP_ERR_MISSING_METADATA;
        if (p[*pos] == '}') {
            (*pos)++;
            return SLP_OK;
        }
        char key[64];
        slp_error_t e = read_string(p, n, pos, key, sizeof key);
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

    int64_t idx = (int64_t)frame_number + SLP_FRAME_BASE;
    if (idx < 0) idx = 0;
    slp_error_t e = slot_grow(s, (size_t)idx + 1);
    if (e != SLP_OK) return e;

    memcpy(&s->frames[idx], &out->pending[slot], sizeof(slp_frame_t));
    if ((size_t)idx + 1 > s->count) s->count = (size_t)idx + 1;
    if (s->count > out->frame_count) out->frame_count = s->count;
    return SLP_OK;
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

    while (pos < n) {
        uint8_t code = raw[pos];
        pos++;
        int32_t sz = sizes[code];
        if (sz < 0) {
            slp_replay_free(out);
            return SLP_ERR_UNKNOWN_PAYLOAD_SIZE;
        }
        if ((size_t)sz > n - pos) {
            slp_replay_free(out);
            return SLP_ERR_TRUNCATED_EVENT;
        }
        const uint8_t *p = raw + pos;
        pos += (size_t)sz;

        switch (code) {
            case 0x36:
                if (!out->have_game_start) {
                    decode_game_start(p, (size_t)sz, &out->game_start);
                    out->have_game_start = true;
                }
                break;
            case 0x37: {
                slp_frame_t tmp;
                memset(&tmp, 0, sizeof tmp);
                decode_pre(p, (size_t)sz, &tmp);
                out->pending[slot_index(&tmp)] = tmp;
                out->slots[slot_index(&tmp)].active = true;
                break;
            }
            case 0x38: {
                slp_frame_t tmp;
                memset(&tmp, 0, sizeof tmp);
                decode_post(p, (size_t)sz, &tmp);
                out->pending[slot_index(&tmp)] = tmp;
                out->slots[slot_index(&tmp)].active = true;
                break;
            }
            case 0x3C: { /* frame bookend */
                int32_t fn = rd_i32be(p + 1);
                for (unsigned i = 0; i < SLP_SLOT_COUNT; i++) {
                    if (!out->slots[i].active) continue;
                    if (out->pending[i].frame_number != fn) continue;
                    e = commit_frame(out, i, fn);
                    if (e != SLP_OK) {
                        slp_replay_free(out);
                        return e;
                    }
                }
                break;
            }
            case 0x39: /* game end */
                goto done;
            default:
                break;
        }
    }

done:
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
    return SLP_OK;
}

void slp_replay_free(slp_replay_t *r) {
    if (!r) return;
    for (unsigned i = 0; i < SLP_SLOT_COUNT; i++) {
        free(r->slots[i].frames);
        r->slots[i].frames = NULL;
        r->slots[i].cap = 0;
    }
}

slp_frame_t *slp_frame_at(slp_replay_t *r, unsigned port, bool follower,
                          int32_t frame_number) {
    if (port >= SLP_MAX_PORTS) return NULL;
    unsigned slot = port * 2 + (follower ? 1u : 0u);
    slp_slot_t *s = &r->slots[slot];
    if (!s->active || !s->frames) return NULL;
    int64_t idx = (int64_t)frame_number + SLP_FRAME_BASE;
    if (idx < 0 || (size_t)idx >= s->count) return NULL;
    return &s->frames[idx];
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
