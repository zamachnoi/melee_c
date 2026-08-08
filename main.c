#include "parser.h"

#include <stdio.h>
#include <stdlib.h>

static unsigned char *read_file(const char *path, size_t *len_out) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    unsigned char *buf = malloc((size_t)sz);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *len_out = (size_t)sz;
    return buf;
}

static void print_frame(FILE *out, const slp_frame_t *f) {
    fprintf(out,
            "  frame %d P%d%s %-14s state=0x%04X x=%8.2f y=%8.2f face=%+.1f "
            "%%=%6.2f stocks=%u anim=%.2f air=%d inst=%u\n",
            f->frame_number, f->player_index, f->is_follower ? " (f)" : "",
            slp_character_name(f->character_id), f->action_state, f->x, f->y,
            f->facing, f->percent, f->stocks_remaining, f->anim_frame,
            (int)f->is_airborne, f->instance_id);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <file.slp>\n", argv[0]);
        return 2;
    }

    size_t len;
    unsigned char *data = read_file(argv[1], &len);
    if (!data) {
        fprintf(stderr, "error: cannot read %s\n", argv[1]);
        return 1;
    }

    slp_replay_t r;
    slp_error_t e = slp_parse(data, len, &r);
    if (e != SLP_OK) {
        fprintf(stderr, "parse error: %s\n", slp_error_string(e));
        free(data);
        return 1;
    }

    const slp_game_start_t *gs = &r.game_start;
    printf("stage:  %d (%s)\n", gs->stage_id, slp_stage_name(gs->stage_id));
    printf("timer:  %d\n", gs->timer);
    printf("version:%u.%u.%u\n", gs->version[0], gs->version[1],
           gs->version[2]);
    printf("teams:  %s\n", gs->is_teams ? "yes" : "no");
    printf("pal:    %s\n", gs->pal ? "yes" : "no");
    printf("start:  %s\n", r.start_at);
    printf("played: %s\n", r.played_on);
    printf("last:   %d\n", r.last_frame);
    printf("frames: %zu\n", r.frame_count);
    for (int i = 0; i < SLP_MAX_PORTS; i++) {
        if (!gs->has_player[i]) continue;
        printf("port %d: char %d (%s) costume %u stocks %u name '%s' code '%s'\n",
               i + 1, gs->external_char_id[i],
               slp_character_name(gs->external_char_id[i]),
               gs->costume_index[i], gs->stock_count[i], gs->name[i],
               gs->connect_code[i]);
    }
    printf("\n");

    for (int32_t fn = -123; fn <= r.last_frame; fn++) {
        bool any = false;
        for (unsigned p = 0; p < SLP_MAX_PORTS; p++) {
            slp_frame_t *f = slp_frame_at(&r, p, false, fn);
            if (f) {
                if (!any) printf("frame %d\n", fn);
                print_frame(stdout, f);
                any = true;
            }
        }
        if (any) printf("\n");
    }

    /* sanity stats */
    int total = 0;
    for (int32_t fn = -123; fn <= r.last_frame; fn++) {
        for (unsigned p = 0; p < SLP_MAX_PORTS; p++) {
            if (slp_frame_at(&r, p, false, fn)) total++;
        }
    }
    printf("resolved frames total: %d\n", total);

    slp_replay_free(&r);
    free(data);
    return 0;
}
