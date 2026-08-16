/* Standalone check: render a model+action+frame to a PPM for inspection.
   usage: test_render model anims action frame out.ppm [facing] */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asset.h"
#include "render.h"

#define W 480
#define H 540

int main(int argc, char **argv) {
    if (argc < 6) { fprintf(stderr, "usage: %s model anims action frame out.ppm [facing]\n", argv[0]); return 1; }
    asset_model_t *m = asset_model_load(argv[1]);
    asset_anims_t *a = asset_anims_load(argv[2]);
    if (!m || !a) { fprintf(stderr, "load failed\n"); return 1; }
    uint32_t aidx = (strcmp(argv[3], "bind") == 0) ? UINT32_MAX : render_find_action(a, argv[3]);
    float frame = (float)atof(argv[4]);
    int facing = argc > 6 ? atoi(argv[6]) : 1;
    if (aidx == UINT32_MAX && strcmp(argv[3], "bind") != 0) { fprintf(stderr, "action '%s' not found\n", argv[3]); return 1; }

    uint8_t *fb = calloc(1, (size_t)W * H * 4);
    for (int i = 0; i < W * H; i++) { fb[i * 4] = 30; fb[i * 4 + 1] = 30; fb[i * 4 + 2] = 40; fb[i * 4 + 3] = 255; }

    /* Auto-fit to the evaluated pose; extracted vertices may be JOBJ-local. */
    float bounds[4];
    if (render_pose_bounds(m, a, aidx, frame, bounds) != 0) {
        fprintf(stderr, "pose bounds failed\n");
        return 1;
    }
    float minx = bounds[0], miny = bounds[1];
    float maxx = bounds[2], maxy = bounds[3];
    printf("pose bounds x[%.2f,%.2f] y[%.2f,%.2f]\n",
           minx, maxx, miny, maxy);
    float w = maxx - minx, h = maxy - miny;
    if (w < 1e-3f) w = 1e-3f;
    if (h < 1e-3f) h = 1e-3f;
    float scale = fminf((W * 0.85f) / w, (H * 0.85f) / h);
    float cx = (minx + maxx) / 2.0f, cy = (miny + maxy) / 2.0f;
    float tx = W / 2.0f - cx * scale;
    float ty = H / 2.0f + cy * scale;

    size_t tris = render_pose(m, a, aidx, frame, facing, scale, tx, ty, fb, W, H);
    printf("scale=%.2f tx=%.1f ty=%.1f\n", scale, tx, ty);
    printf("action=%s frame=%.1f facing=%d tris=%zu bones=%u\n", argv[3], frame, facing, tris, m->bone_count);

    FILE *f = fopen(argv[5], "wb");
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int i = 0; i < W * H; i++) fwrite(&fb[i * 4], 1, 3, f);
    fclose(f);
    free(fb);
    asset_model_free(m);
    asset_anims_free(a);
    return 0;
}
