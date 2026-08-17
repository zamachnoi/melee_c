#include "timeline.h"

#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "asset.h"
#include "protocol.h"

enum {
    HEADER_SIZE = 64,
    PLAYER_SIZE = 64,
    SLOT_SIZE = 56,
    ITEM_SIZE = 40,
    STAGE_EVENT_SIZE = 16,
    CAMERA_HEADER_SIZE = 16,
};

typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
} writer_t;

static int reserve(writer_t *w, size_t add) {
    if (add > SIZE_MAX - w->len) return -1;
    size_t need = w->len + add;
    if (need <= w->cap) return 0;
    size_t cap = w->cap ? w->cap : 4096;
    while (cap < need) {
        if (cap > SIZE_MAX / 2) { cap = need; break; }
        cap *= 2;
    }
    uint8_t *p = realloc(w->data, cap);
    if (!p) return -1;
    w->data = p;
    w->cap = cap;
    return 0;
}

static int append_zero(writer_t *w, size_t n, size_t *offset) {
    if (reserve(w, n) != 0) return -1;
    if (offset) *offset = w->len;
    memset(w->data + w->len, 0, n);
    w->len += n;
    return 0;
}

static int align4(writer_t *w) {
    size_t pad = (4 - (w->len & 3)) & 3;
    return append_zero(w, pad, NULL);
}

static void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void putf32(uint8_t *p, float v) {
    uint32_t bits;
    memcpy(&bits, &v, sizeof bits);
    put32(p, bits);
}

static int offset32(size_t value, uint32_t *out) {
    if (value > UINT32_MAX) return -1;
    *out = (uint32_t)value;
    return 0;
}

static const slp_frame_t *frame_at_exact(const slp_replay_t *r,
                                         unsigned slot, int32_t frame) {
    const slp_frame_t *f = slp_frame_at(r, slot / 2, slot & 1, frame);
    return f && f->frame_number == frame ? f : NULL;
}

static int append_u8_field(writer_t *w, const slp_replay_t *r,
                           unsigned slot, int32_t start, uint32_t count,
                           unsigned which, uint32_t *offset) {
    if (align4(w) != 0 || offset32(w->len, offset) != 0 ||
        reserve(w, count) != 0)
        return -1;
    for (uint32_t i = 0; i < count; i++) {
        int32_t fn = start + (int32_t)i;
        const slp_frame_t *f = frame_at_exact(r, slot, fn);
        uint8_t value = 0;
        if (which == 0) value = f != NULL;
        else if (which == 1) value = f ? f->character_id : 0;
        else if (which == 2) value = f ? f->stocks_remaining : 0;
        else if (which == 3) value = f && f->is_airborne
                                     ? TIMELINE_SLOT_AIRBORNE : 0;
        w->data[w->len++] = value;
    }
    return 0;
}

static int append_u16_field(writer_t *w, const slp_replay_t *r,
                            unsigned slot, int32_t start, uint32_t count,
                            uint32_t *offset) {
    if (align4(w) != 0 || offset32(w->len, offset) != 0 ||
        reserve(w, (size_t)count * 2) != 0)
        return -1;
    for (uint32_t i = 0; i < count; i++) {
        const slp_frame_t *f = frame_at_exact(r, slot, start + (int32_t)i);
        put16(w->data + w->len, f ? f->action_state : 0);
        w->len += 2;
    }
    return 0;
}

static int append_u32_field(writer_t *w, const slp_replay_t *r,
                            unsigned slot, int32_t start, uint32_t count,
                            uint32_t *offset) {
    if (align4(w) != 0 || offset32(w->len, offset) != 0 ||
        reserve(w, (size_t)count * 4) != 0)
        return -1;
    for (uint32_t i = 0; i < count; i++) {
        const slp_frame_t *f = frame_at_exact(r, slot, start + (int32_t)i);
        put32(w->data + w->len, f ? f->animation_index : UINT32_MAX);
        w->len += 4;
    }
    return 0;
}

static int append_float_field(writer_t *w, const slp_replay_t *r,
                              unsigned slot, int32_t start, uint32_t count,
                              unsigned which, uint32_t *offset) {
    if (align4(w) != 0 || offset32(w->len, offset) != 0 ||
        reserve(w, (size_t)count * 4) != 0)
        return -1;
    for (uint32_t i = 0; i < count; i++) {
        const slp_frame_t *f = frame_at_exact(r, slot, start + (int32_t)i);
        float value = 0.0f;
        if (f) {
            if (which == 0) value = f->anim_frame;
            else if (which == 1) value = f->x;
            else if (which == 2) value = f->y;
            else if (which == 3) value = f->facing;
            else if (which == 4) value = f->percent;
            else if (which == 5) value = f->shield_size;
        }
        putf32(w->data + w->len, value);
        w->len += 4;
    }
    return 0;
}

static size_t count_items(const slp_replay_t *r, int32_t start, int32_t end) {
    size_t count = 0;
    for (int32_t fn = start; fn <= end; fn++) {
        const slp_item_list_t *items = slp_items_at(r, fn);
        if (items) count += items->count;
        if (fn == INT32_MAX) break;
    }
    return count;
}

static int fod_event_on_frame(const slp_replay_t *r, int32_t fn,
                              unsigned platform) {
    const slp_fod_platform_t *f = slp_fod_at((slp_replay_t *)r, fn, platform);
    return f && f->frame_number == fn && f->platform == platform;
}

static int fod_needs_spawn(const slp_replay_t *r, int32_t fn,
                           unsigned platform) {
    return r->have_game_start && r->game_start.stage_id == SLP_FOD_STAGE_ID &&
           !fod_event_on_frame(r, fn, platform);
}

static float fod_spawn_height(unsigned platform) {
    return platform == 0 ? SLP_FOD_RIGHT_START : SLP_FOD_LEFT_START;
}

static size_t count_stage_events(const slp_replay_t *r, int32_t start,
                                 int32_t end) {
    size_t count = 0;
    for (int32_t fn = start; fn <= end; fn++) {
        const slp_fod_platform_t *f0 = slp_fod_at((slp_replay_t *)r, fn, 0);
        const slp_fod_platform_t *f1 = slp_fod_at((slp_replay_t *)r, fn, 1);
        const slp_whispy_blow_t *wh = slp_whispy_at((slp_replay_t *)r, fn);
        const slp_stadium_transform_t *st =
            slp_stadium_at((slp_replay_t *)r, fn);
        if (f0 && f0->frame_number == fn && f0->platform == 0) count++;
        else if (fn == start && fod_needs_spawn(r, start, 0)) count++;
        if (f1 && f1->frame_number == fn && f1->platform == 1) count++;
        else if (fn == start && fod_needs_spawn(r, start, 1)) count++;
        if (wh && wh->frame_number == fn) count++;
        if (st && st->frame_number == fn) count++;
        if (fn == INT32_MAX) break;
    }
    return count;
}

static void write_stage_event(uint8_t *p, int32_t frame, uint8_t kind,
                              uint8_t index, uint32_t data0, uint32_t data1) {
    put32(p, (uint32_t)frame);
    p[4] = kind;
    p[5] = index;
    put16(p + 6, 0);
    put32(p + 8, data0);
    put32(p + 12, data1);
}

int timeline_serialize(const slp_replay_t *r, int32_t start, int32_t end,
                       const timeline_camera_t *cameras, size_t camera_count,
                       timeline_blob_t *out) {
    if (!r || !out || end < start) return -1;
    uint64_t count64 = (uint64_t)((int64_t)end - start + 1);
    if (count64 > UINT32_MAX) return -1;
    uint32_t frame_count = (uint32_t)count64;
    if ((cameras == NULL) != (camera_count == 0) ||
        (cameras && camera_count != frame_count))
        return -1;

    writer_t w = {0};
    size_t header_off, players_off, slots_off;
    if (append_zero(&w, HEADER_SIZE, &header_off) != 0 ||
        append_zero(&w, SLP_MAX_PORTS * PLAYER_SIZE, &players_off) != 0 ||
        append_zero(&w, SLP_SLOT_COUNT * SLOT_SIZE, &slots_off) != 0)
        goto fail;

    unsigned player_count = 0;
    for (unsigned port = 0; port < SLP_MAX_PORTS; port++) {
        if (!r->game_start.has_player[port]) continue;
        uint8_t *p = w.data + players_off + player_count * PLAYER_SIZE;
        p[0] = (uint8_t)port;
        p[1] = r->game_start.external_char_id[port];
        p[2] = r->game_start.costume_index[port];
        p[3] = r->game_start.stock_count[port];
        p[4] = r->game_start.team_id[port];
        p[5] = r->game_start.player_type[port];
        memcpy(p + 8, r->game_start.name[port], 32);
        memcpy(p + 40, r->game_start.connect_code[port], 16);
        player_count++;
    }

    for (unsigned slot = 0; slot < SLP_SLOT_COUNT; slot++) {
        uint8_t *d = w.data + slots_off + slot * SLOT_SIZE;
        d[0] = (uint8_t)(slot / 2);
        d[1] = (uint8_t)(slot & 1);
        d[2] = r->slots[slot].active ? 1 : 0;
        uint32_t offsets[13];
        if (!r->slots[slot].active) {
            if (align4(&w) != 0 || offset32(w.len, &offsets[0]) != 0 ||
                append_zero(&w, (size_t)frame_count * 4, NULL) != 0)
                goto fail;
            for (unsigned i = 1; i < 12; i++) offsets[i] = offsets[0];
            if (offset32(w.len, &offsets[12]) != 0) goto fail;
            d = w.data + slots_off + slot * SLOT_SIZE;
            for (unsigned i = 0; i < 13; i++) put32(d + 4 + i * 4, offsets[i]);
            continue;
        }
        if (append_u8_field(&w, r, slot, start, frame_count, 0, &offsets[0]) ||
            append_u8_field(&w, r, slot, start, frame_count, 1, &offsets[1]) ||
            append_u32_field(&w, r, slot, start, frame_count, &offsets[2]) ||
            append_u16_field(&w, r, slot, start, frame_count, &offsets[3]) ||
            append_float_field(&w, r, slot, start, frame_count, 0, &offsets[4]) ||
            append_float_field(&w, r, slot, start, frame_count, 1, &offsets[5]) ||
            append_float_field(&w, r, slot, start, frame_count, 2, &offsets[6]) ||
            append_float_field(&w, r, slot, start, frame_count, 3, &offsets[7]) ||
            append_float_field(&w, r, slot, start, frame_count, 4, &offsets[8]) ||
            append_float_field(&w, r, slot, start, frame_count, 5, &offsets[9]) ||
            append_u8_field(&w, r, slot, start, frame_count, 2, &offsets[10]) ||
            append_u8_field(&w, r, slot, start, frame_count, 3, &offsets[11]))
            goto fail;
        if (offset32(w.len, &offsets[12]) != 0) goto fail;
        /* The final value is the end offset; the preceding twelve are fields. */
        d = w.data + slots_off + slot * SLOT_SIZE;
        for (unsigned i = 0; i < 13; i++) put32(d + 4 + i * 4, offsets[i]);
    }

    if (align4(&w) != 0) goto fail;
    uint32_t items_off;
    size_t item_count = count_items(r, start, end);
    if (item_count > UINT32_MAX || offset32(w.len, &items_off) != 0 ||
        reserve(&w, item_count * ITEM_SIZE) != 0)
        goto fail;
    for (int32_t fn = start; fn <= end; fn++) {
        const slp_item_list_t *items = slp_items_at(r, fn);
        if (items) for (size_t i = 0; i < items->count; i++) {
            const slp_item_t *it = &items->items[i];
            uint8_t *p = w.data + w.len;
            put32(p, (uint32_t)fn); put32(p + 4, it->spawn_id);
            put16(p + 8, it->type_id); p[10] = it->state; p[11] = (uint8_t)it->owner;
            putf32(p + 12, it->facing); putf32(p + 16, it->x_vel);
            putf32(p + 20, it->y_vel); putf32(p + 24, it->x);
            putf32(p + 28, it->y); put16(p + 32, it->damage_taken);
            put16(p + 34, it->instance_id); put32(p + 36, 0);
            w.len += ITEM_SIZE;
        }
        if (fn == INT32_MAX) break;
    }

    uint32_t events_off;
    size_t event_count = count_stage_events(r, start, end);
    if (event_count > UINT32_MAX || offset32(w.len, &events_off) != 0 ||
        reserve(&w, event_count * STAGE_EVENT_SIZE) != 0)
        goto fail;
    for (int32_t fn = start; fn <= end; fn++) {
        for (unsigned platform = 0; platform < 2; platform++) {
            const slp_fod_platform_t *f = slp_fod_at((slp_replay_t *)r, fn,
                                                      platform);
            float height = 0.f;
            int write = 0;
            if (f && f->frame_number == fn && f->platform == platform) {
                height = f->height;
                write = 1;
            } else if (fn == start && fod_needs_spawn(r, start, platform)) {
                height = fod_spawn_height(platform);
                write = 1;
            }
            if (write) {
                uint32_t bits; memcpy(&bits, &height, 4);
                write_stage_event(w.data + w.len, fn, TIMELINE_STAGE_FOD,
                                  (uint8_t)platform, bits, 0);
                w.len += STAGE_EVENT_SIZE;
            }
        }
        const slp_whispy_blow_t *wh = slp_whispy_at((slp_replay_t *)r, fn);
        if (wh && wh->frame_number == fn) {
            write_stage_event(w.data + w.len, fn, TIMELINE_STAGE_WHISPY, 0,
                              wh->direction, 0);
            w.len += STAGE_EVENT_SIZE;
        }
        const slp_stadium_transform_t *st =
            slp_stadium_at((slp_replay_t *)r, fn);
        if (st && st->frame_number == fn) {
            write_stage_event(w.data + w.len, fn, TIMELINE_STAGE_STADIUM, 0,
                              st->event, st->type);
            w.len += STAGE_EVENT_SIZE;
        }
        if (fn == INT32_MAX) break;
    }

    uint32_t camera_off = 0;
    if (cameras) {
        size_t cam_header;
        if (align4(&w) != 0 || offset32(w.len, &camera_off) != 0 ||
            append_zero(&w, CAMERA_HEADER_SIZE, &cam_header) != 0)
            goto fail;
        for (unsigned field = 0; field < 3; field++) {
            uint32_t field_off;
            if (offset32(w.len, &field_off) != 0 ||
                reserve(&w, (size_t)frame_count * 4) != 0)
                goto fail;
            put32(w.data + cam_header + field * 4, field_off);
            for (uint32_t i = 0; i < frame_count; i++) {
                float value = field == 0 ? cameras[i].x
                            : field == 1 ? cameras[i].y : cameras[i].zoom;
                putf32(w.data + w.len, value);
                w.len += 4;
            }
        }
    }

    uint32_t end_off, players32, slots32;
    if (offset32(w.len, &end_off) || offset32(players_off, &players32) ||
        offset32(slots_off, &slots32))
        goto fail;
    uint8_t *h = w.data + header_off;
    put32(h, TIMELINE_MAGIC); put16(h + 4, TIMELINE_SCHEMA_VERSION);
    put16(h + 6, TIMELINE_FLAG_COMPLETED |
                 (cameras ? TIMELINE_FLAG_CAMERA : 0));
    put32(h + 8, (uint32_t)start); put32(h + 12, (uint32_t)end);
    put16(h + 16, r->game_start.stage_id); h[18] = SLP_SLOT_COUNT;
    h[19] = (uint8_t)player_count; put32(h + 20, frame_count);
    put32(h + 24, players32); put32(h + 28, slots32);
    put32(h + 32, items_off); put32(h + 36, (uint32_t)item_count);
    put32(h + 40, events_off); put32(h + 44, (uint32_t)event_count);
    put32(h + 48, camera_off); put32(h + 52, end_off);
    put16(h + 56, ASSET_SCHEMA_VERSION); put16(h + 58, LIVE_PROTOCOL_VERSION);

    out->data = w.data;
    out->len = w.len;
    return 0;

fail:
    free(w.data);
    out->data = NULL;
    out->len = 0;
    return -1;
}

void timeline_blob_free(timeline_blob_t *blob) {
    if (!blob) return;
    free(blob->data);
    blob->data = NULL;
    blob->len = 0;
}
