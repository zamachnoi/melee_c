#include <math.h>
#include <stdio.h>
#include <string.h>

#include "asset.h"
#include "render.h"

static int check_action(const char *name, float expected_z, float expected_y) {
    asset_bone_t bones[2] = {0};
    asset_vertex_t vertex = {0};
    asset_key_t zkey = {.frame = 0, .value = 35, .interp = ATK_STEP};
    asset_key_t ykey = {.frame = 0, .value = -15, .interp = ATK_STEP};
    asset_track_t tracks[2] = {
        {.channel = 7, .key_count = 1, .keys = &zkey},
        {.channel = 6, .key_count = 1, .keys = &ykey},
    };
    asset_joint_anim_t joint = {.bone_index = 1, .track_count = 2,
                                .tracks = tracks};
    asset_action_t action = {.end_frame = 40, .joint_count = 1,
                             .joints = &joint};
    asset_anims_t anims = {.action_count = 1, .actions = &action};
    asset_model_t model = {.bone_count = 2, .vertex_count = 1,
                           .bones = bones, .vertices = &vertex};
    float bounds[4];

    bones[0].parent = UINT16_MAX;
    bones[1].parent = 0;
    bones[0].base[0] = bones[0].base[4] = bones[0].base[8] = 1;
    bones[1].base[0] = bones[1].base[4] = bones[1].base[8] = 1;
    vertex.weight[0] = 1;
    vertex.bone[0] = 1;
    snprintf(action.name, sizeof action.name, "%s", name);

    if (render_pose_profile_bounds(&model, &anims, 0, 0, bounds) != 0)
        return -1;
    if (fabsf(bounds[0] - expected_z) > 0.001f ||
        fabsf(bounds[1] - expected_y) > 0.001f) {
        fprintf(stderr, "%s: got z/y %.2f/%.2f, expected %.2f/%.2f\n",
                name, bounds[0], bounds[1], expected_z, expected_y);
        return -1;
    }
    return 0;
}

int main(void) {
    if (check_action("PlyFox_ACTION_AttackDash_figatree", 0, 0) != 0)
        return 1;
    if (check_action("PlyFox_ACTION_CliffWait_figatree", 0, 0) != 0)
        return 1;
    puts("pose root-motion regression: passed");
    return 0;
}
