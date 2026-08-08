#include "parser.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* big-endian writers */
static void put_u32be(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}
static void put_i32be(uint8_t *p, int32_t v) {
    put_u32be(p, (uint32_t)v);
}
static void put_u16be(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}
static void put_f32be(uint8_t *p, float v) {
    uint32_t b;
    memcpy(&b, &v, 4);
    put_u32be(p, b);
}

static uint8_t *buf;

static void emit_pre(uint8_t **w, int32_t fn, uint8_t port, float x) {
    uint8_t payload[66];
    memset(payload, 0, sizeof payload);
    **w = 0x37; (*w)++;
    put_i32be(payload + 0, fn);
    payload[4] = port;
    payload[5] = 0;            /* is_follower */
    put_u32be(payload + 6, 1); /* random seed */
    put_u16be(payload + 10, 0x0010); /* action state */
    put_f32be(payload + 12, x);      /* pre x */
    put_f32be(payload + 16, 0);      /* pre y */
    put_f32be(payload + 20, 1);      /* facing */
    memcpy(*w, payload, sizeof payload);
    *w += sizeof payload;
}

static void emit_post(uint8_t **w, int32_t fn, uint8_t port, float x,
                      float pct) {
    uint8_t payload[84];
    memset(payload, 0, sizeof payload);
    **w = 0x38; (*w)++;
    put_i32be(payload + 0, fn);
    payload[4] = port;
    payload[5] = 0;            /* is_follower */
    payload[6] = 22;           /* internal char id (Falco) */
    put_u16be(payload + 7, 0x002B); /* action state (Wait1) */
    put_f32be(payload + 9, x);
    put_f32be(payload + 13, 0);
    put_f32be(payload + 17, 1);     /* facing */
    put_f32be(payload + 21, pct);
    payload[32] = 4;           /* stocks_remaining at payload offset 0x21 */
    memcpy(*w, payload, sizeof payload);
    *w += sizeof payload;
}

static void emit_bookend(uint8_t **w, int32_t fn) {
    **w = 0x3C; (*w)++;
    put_i32be(*w, fn); *w += 4;
    put_i32be(*w, fn); *w += 4;
}

static void emit_item(uint8_t **w, int32_t fn, uint32_t spawn, uint16_t type,
                      float x, float y, int8_t owner) {
    uint8_t payload[44];
    memset(payload, 0, sizeof payload);
    **w = 0x3B; (*w)++;
    put_i32be(payload + 0, fn);
    put_u16be(payload + 4, type);
    payload[6] = 1;                 /* state */
    put_f32be(payload + 0x13, x);
    put_f32be(payload + 0x17, y);
    put_u32be(payload + 0x21, spawn);
    payload[0x29] = (uint8_t)owner;
    memcpy(*w, payload, sizeof payload);
    *w += sizeof payload;
}

static void emit_fod(uint8_t **w, int32_t fn, uint8_t platform, float h) {
    uint8_t payload[9];
    memset(payload, 0, sizeof payload);
    **w = 0x3F; (*w)++;
    put_i32be(payload + 0, fn);
    payload[4] = platform;
    put_f32be(payload + 5, h);
    memcpy(*w, payload, sizeof payload);
    *w += sizeof payload;
}

static void emit_whispy(uint8_t **w, int32_t fn, uint8_t dir) {
    uint8_t payload[5];
    memset(payload, 0, sizeof payload);
    **w = 0x40; (*w)++;
    put_i32be(payload + 0, fn);
    payload[4] = dir;
    memcpy(*w, payload, sizeof payload);
    *w += sizeof payload;
}

static void emit_stadium(uint8_t **w, int32_t fn, uint16_t ev, uint16_t type) {
    uint8_t payload[8];
    memset(payload, 0, sizeof payload);
    **w = 0x41; (*w)++;
    put_i32be(payload + 0, fn);
    put_u16be(payload + 4, ev);
    put_u16be(payload + 6, type);
    memcpy(*w, payload, sizeof payload);
    *w += sizeof payload;
}

int main(void) {
    enum { N = 4096 };
    buf = malloc(N);
    uint8_t *w = buf;

    *w++ = '{';
    *w++ = 'U'; *w++ = 3; memcpy(w, "raw", 3); w += 3;
    *w++ = '['; *w++ = '$'; *w++ = 'U'; *w++ = '#';
    *w++ = 'l';

    uint8_t *raw_len_pos = w;
    w += 4; /* patched below */

    /* payload size table */
    *w++ = 0x35; *w++ = 3 * 9 + 1;
    *w++ = 0x36; put_u16be(w, 5); w += 2;
    *w++ = 0x37; put_u16be(w, 66); w += 2;
    *w++ = 0x38; put_u16be(w, 84); w += 2;
    *w++ = 0x3B; put_u16be(w, 44); w += 2;
    *w++ = 0x3C; put_u16be(w, 8); w += 2;
    *w++ = 0x3F; put_u16be(w, 9); w += 2;
    *w++ = 0x40; put_u16be(w, 5); w += 2;
    *w++ = 0x41; put_u16be(w, 8); w += 2;
    *w++ = 0x39; put_u16be(w, 6); w += 2;

    /* game start (payload 5: version only) */
    *w++ = 0x36;
    *w++ = 3; *w++ = 19; *w++ = 0; *w++ = 0; *w++ = 0; /* version 3.19.0 */

    /* frame 0: first version */
    emit_pre(&w, 0, 0, -10.0f);
    emit_post(&w, 0, 0, -10.0f, 0.0f);
    emit_item(&w, 0, 1, 0x4B, -57.45f, 5.19f, 0);
    emit_fod(&w, 0, 0, 10.0f);
    emit_fod(&w, 0, 1, 20.0f);
    emit_whispy(&w, 0, 1);
    emit_stadium(&w, 0, 2, 3);
    emit_bookend(&w, 0);

    /* frame 1 (no items) */
    emit_pre(&w, 1, 0, 5.0f);
    emit_post(&w, 1, 0, 5.0f, 12.5f);
    emit_bookend(&w, 1);

    /* rollback: frame 0 re-simulated with different state (latest wins) */
    emit_pre(&w, 0, 0, -40.0f);
    emit_post(&w, 0, 0, -40.0f, 30.0f);
    emit_item(&w, 0, 1, 0x4B, -40.0f, 8.0f, 0);
    emit_fod(&w, 0, 0, 30.0f);
    emit_fod(&w, 0, 1, 40.0f);
    emit_whispy(&w, 0, 2);
    emit_stadium(&w, 0, 5, 6);
    emit_bookend(&w, 0);

    /* game end */
    *w++ = 0x39; *w++ = 2; *w++ = 0xFF; *w++ = 0; *w++ = 1; *w++ = 2; *w++ = 3;

    size_t raw_len = (size_t)(w - (raw_len_pos + 4));
    put_u32be(raw_len_pos, (uint32_t)raw_len);

    /* metadata */
    *w++ = 'U'; *w++ = 8; memcpy(w, "metadata", 8); w += 8;
    *w++ = '{';
    *w++ = 'U'; *w++ = 9; memcpy(w, "lastFrame", 9); w += 9;
    *w++ = 'l'; put_i32be(w, 1); w += 4;
    *w++ = '}';
    *w++ = '}';

    size_t len = (size_t)(w - buf);

    slp_replay_t r;
    slp_error_t e = slp_parse(buf, len, &r);
    if (e != SLP_OK) {
        printf("parse error: %s (len=%zu)\n", slp_error_string(e), len);
        return 1;
    }

    assert(r.frame_count == SLP_FRAME_BASE + 2);

    slp_frame_t *f0 = slp_frame_at(&r, 0, false, 0);
    slp_frame_t *f1 = slp_frame_at(&r, 0, false, 1);
    assert(f0 != NULL && f1 != NULL);

    /* latest version of frame 0 must win */
    assert(f0->x == -40.0f);
    assert(f0->percent == 30.0f);
    assert(f0->frame_number == 0);
    assert(f0->stocks_remaining == 4);

    /* frame 1 unaffected */
    assert(f1->x == 5.0f);
    assert(f1->percent == 12.5f);

    /* items: latest version of frame 0 wins */
    slp_item_list_t *items = slp_items_at(&r, 0);
    assert(items != NULL);
    assert(items->count == 1);
    assert(items->items[0].spawn_id == 1);
    assert(items->items[0].type_id == 0x4B);
    assert(items->items[0].x == -40.0f);
    assert(items->items[0].y == 8.0f);
    assert(slp_items_at(&r, 1) == NULL || slp_items_at(&r, 1)->count == 0);

    /* stage events: latest wins, per platform slot */
    const slp_fod_platform_t *fod0 = slp_fod_at(&r, 0, 0);
    const slp_fod_platform_t *fod1 = slp_fod_at(&r, 0, 1);
    assert(fod0 && fod0->height == 30.0f);
    assert(fod1 && fod1->height == 40.0f);
    const slp_whispy_blow_t *whispy = slp_whispy_at(&r, 0);
    assert(whispy && whispy->direction == 2);
    const slp_stadium_transform_t *st = slp_stadium_at(&r, 0);
    assert(st && st->event == 5 && st->type == 6);

    printf("rollback test passed (frame 0 x=%.1f, latest version won)\n",
           f0->x);

    slp_replay_free(&r);
    free(buf);
    return 0;
}
