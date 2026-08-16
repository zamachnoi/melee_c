#include <math.h>
#include <stdio.h>
#include <string.h>
#include "asset.h"
#include "render.h"

static int check_falco_wait1(const asset_model_t *m, const asset_anims_t *a) {
    if (m->bone_count != 67) return 0;
    const asset_action_t *wait = NULL;
    for (uint32_t i = 0; i < a->action_count; i++)
        if (strstr(a->actions[i].name, "ACTION_Wait1_figatree")) { wait = &a->actions[i]; break; }
    if (!wait) return -1;
    int found_y = 0, found_z = 0;
    for (uint32_t j = 0; j < wait->joint_count; j++) {
        const asset_joint_anim_t *ja = &wait->joints[j];
        if (ja->bone_index != 3) continue;
        for (uint32_t t = 0; t < ja->track_count; t++) {
            const asset_track_t *tk = &ja->tracks[t];
            if (!tk->key_count) continue;
            if (tk->channel == 6 && fabsf(tk->keys[0].value - -0.9263916f) < 0.0001f) found_y = 1;
            if (tk->channel == 7 && fabsf(tk->keys[0].value - -0.0028076172f) < 0.0001f) found_z = 1;
        }
    }
    if (!found_y || !found_z) return -1;
    puts("falco Wait1 packed-track regression: passed");
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: test_asset <model> <anims> <stage>\n"); return 1; }
    asset_model_t *m = asset_model_load(argv[1]);
    if (!m) { fprintf(stderr, "model load failed\n"); return 1; }
    printf("model: bones=%u verts=%u idx=%u pg=%u tex=%u\n",
           m->bone_count, m->vertex_count, m->index_count, m->pgroup_count, m->texture_count);

    asset_anims_t *a = asset_anims_load(argv[2]);
    if (!a) { fprintf(stderr, "anims load failed\n"); return 1; }
    printf("anims: actions=%u\n", a->action_count);
    if (a->action_count) printf("  [0] %s end=%.1f joints=%u\n",
        a->actions[0].name, a->actions[0].end_frame, a->actions[0].joint_count);
    if (check_falco_wait1(m, a) != 0) {
        fprintf(stderr, "Falco Wait1 track decode regression\n");
        return 1;
    }

    asset_stage_t *s = asset_stage_load(argv[3]);
    if (!s) { fprintf(stderr, "stage load failed\n"); return 1; }
    printf("stage: scale=%.3f sections=%u lights=%u cam=(%.1f,%.1f,%.1f)\n",
           s->scale, s->section_count, s->light_count,
           s->cam_pos[0], s->cam_pos[1], s->cam_pos[2]);
    for (uint32_t i = 0; i < s->section_count; i++) {
        float b[4];
        if (render_pose_bounds(&s->sections[i], NULL, UINT32_MAX, 0, b) == 0)
            printf("  section[%u]: verts=%u x[%.1f,%.1f] y[%.1f,%.1f]\n",
                   i, s->sections[i].vertex_count, b[0], b[2], b[1], b[3]);
    }

    asset_model_free(m);
    asset_anims_free(a);
    asset_stage_free(s);
    return 0;
}
