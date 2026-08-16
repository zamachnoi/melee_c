#include "protocol.h"
#include "sha256.h"
#include "timeline.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static slp_frame_t *make_slot(slp_replay_t *r, unsigned slot) {
    slp_slot_t *s = &r->slots[slot];
    s->active = true;
    s->count = SLP_FRAME_BASE + 2;
    s->cap = s->count;
    s->frames = calloc(s->count, sizeof(*s->frames));
    assert(s->frames);
    for (size_t i = 0; i < s->count; i++) s->frames[i].frame_number = INT32_MIN;
    return s->frames;
}

static slp_frame_t frame(int32_t number, unsigned port, int follower,
                         uint8_t character, float x) {
    slp_frame_t f = {0};
    f.frame_number = number; f.player_index = (uint8_t)port;
    f.is_follower = (uint8_t)follower; f.character_id = character;
    f.action_state = (uint16_t)(0x20 + number); f.animation_index = 100 + (uint32_t)number;
    f.anim_frame = 3.25f + number; f.x = x; f.y = 7.5f + number;
    f.facing = number ? -1.0f : 1.0f; f.percent = 12.5f + number;
    f.shield_size = 55.0f; f.stocks_remaining = 3; f.is_airborne = number > 0;
    return f;
}

int main(int argc, char **argv) {
    uint8_t digest[32]; char hex[65];
    sha256_bytes("abc", 3, digest); sha256_hex(digest, hex);
    assert(strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0);

    slp_replay_t r = {0};
    r.have_game_start = true; r.game_start.stage_id = 32;
    r.game_start.has_player[0] = true; r.game_start.external_char_id[0] = 20;
    r.game_start.costume_index[0] = 2; r.game_start.stock_count[0] = 4;
    strcpy(r.game_start.name[0], "Falco Fixture");
    r.game_start.has_player[1] = true; r.game_start.external_char_id[1] = 2;
    r.game_start.costume_index[1] = 0; r.game_start.stock_count[1] = 4;
    strcpy(r.game_start.name[1], "Fox Fixture");

    slp_frame_t *leader = make_slot(&r, 0);
    leader[SLP_FRAME_BASE - 1] = frame(-1, 0, 0, 22, 1.5f);
    leader[SLP_FRAME_BASE] = frame(0, 0, 0, 22, 2.5f);
    leader[SLP_FRAME_BASE + 1] = frame(1, 0, 0, 22, 3.5f);
    slp_frame_t *nana = make_slot(&r, 1);
    nana[SLP_FRAME_BASE] = frame(0, 0, 1, 11, -4.0f);
    slp_frame_t *fox = make_slot(&r, 2);
    fox[SLP_FRAME_BASE] = frame(0, 1, 0, 1, 9.0f);

    r.frame_items_count = SLP_FRAME_BASE + 1;
    r.frame_items_cap = r.frame_items_count;
    r.frame_items = calloc(r.frame_items_count, sizeof(*r.frame_items));
    r.frame_items[SLP_FRAME_BASE].count = 1;
    r.frame_items[SLP_FRAME_BASE].cap = 1;
    r.frame_items[SLP_FRAME_BASE].items = calloc(1, sizeof(slp_item_t));
    r.frame_items[SLP_FRAME_BASE].items[0] = (slp_item_t){
        .frame_number=0,.spawn_id=77,.type_id=0x4b,.state=2,.owner=-1,
        .x=10.0f,.y=20.0f,.x_vel=1.0f,.y_vel=-2.0f,.instance_id=9,
    };
    r.fod_count = (SLP_FRAME_BASE + 1) * 2;
    r.fod_cap = r.fod_count;
    r.fod = calloc(r.fod_count, sizeof(*r.fod));
    r.fod[SLP_FRAME_BASE * 2] = (slp_fod_platform_t){.frame_number=0,.platform=0,.height=31.5f};
    r.whispy_count = SLP_FRAME_BASE + 1;
    r.whispy_cap = r.whispy_count;
    r.whispy = calloc(r.whispy_count, sizeof(*r.whispy));
    r.whispy[SLP_FRAME_BASE] = (slp_whispy_blow_t){.frame_number=0,.direction=2};

    timeline_camera_t cameras[3] = {{1,2,3},{4,5,6},{7,8,9}};
    timeline_blob_t blob = {0};
    assert(timeline_serialize(&r, -1, 1, cameras, 3, &blob) == 0);
    assert(blob.len > 64 && blob.len < 4096);
    assert(memcmp(blob.data, "RPL2", 4) == 0);
    assert(blob.data[5] == TIMELINE_SCHEMA_VERSION);
    if (argc > 1) {
        FILE *f = fopen(argv[1], "wb");
        assert(f && fwrite(blob.data, 1, blob.len, f) == blob.len);
        assert(fclose(f) == 0);
    }
    printf("timeline test passed (%zu bytes)\n", blob.len);
    timeline_blob_free(&blob);
    slp_replay_free(&r);
    return 0;
}
