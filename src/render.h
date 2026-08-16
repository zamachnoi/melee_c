#ifndef RENDER_H
#define RENDER_H

#include <stddef.h>
#include <stdint.h>

#include "asset.h"

uint32_t render_find_action(const asset_anims_t *a, const char *name);

/* bounds = {min_x, min_y, max_x, max_y} after pose evaluation. */
int render_pose_bounds(const asset_model_t *m, const asset_anims_t *a,
                       uint32_t action_idx, float frame, float bounds[4]);

size_t render_pose(const asset_model_t *m, const asset_anims_t *a,
                   uint32_t action_idx, float frame, int facing,
                   float scale, float tx, float ty,
                   uint8_t *fb, int W, int H);

/* As above, with model Z contributing to projected screen X/Y.  This is
   useful for stages whose floor lies in the X/Z plane while gameplay remains
   in X/Y. */
size_t render_pose_tilted(const asset_model_t *m, const asset_anims_t *a,
                          uint32_t action_idx, float frame, int facing,
                          float scale, float tx, float ty,
                          float z_to_x, float z_to_y,
                          uint8_t *fb, int W, int H);

#endif /* RENDER_H */
