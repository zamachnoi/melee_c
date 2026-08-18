/*
 * asset.h - runtime asset types + on-disk cache format shared by
 * tools/extract (writer) and src/viewer.c / src/asset.c (reader).
 *
 * The cache is a set of binary files under $ASSET_DIR (default ./cache,
 * shared across worktrees as fixtures/cache), versioned + ISO-fingerprinted
 * via meta.json. See docs/DAT.md, docs/HSD.md,
 * docs/ANIM.md, docs/STAGES.md for the source formats this decodes.
 */

#ifndef ASSET_H
#define ASSET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ASSET_SCHEMA_VERSION 5
/* Bump when extract --all writes a new class of files (e.g. stage .anims).
   Persistent Docker volumes rebuild when meta.json cache_id does not match. */
#define ASSET_CACHE_ID 2
#define ASSET_MAGIC 0x4D444C00u /* "MDL\0" for models */

/* ---- model (shared by characters and stage sections) ---- */

typedef struct {
    uint16_t parent;      /* UINT16_MAX = root */
    uint16_t pgroup_start;/* first primitive group owned by this bone */
    uint16_t pgroup_len;
    uint16_t pad;
    uint32_t flags;
    float base[12];       /* bind local transform, column-major 3x4 */
    float inv_world[12];  /* inverse bind world transform */
} asset_bone_t;

typedef struct {
    float pos[3];
    float nrm[3];
    float uv[2];
    uint8_t color[4];
    float weight[4];
    uint16_t bone[4];
} asset_vertex_t;

/* HSD TObj TEV / texgen + material combine state, baked per primitive group.
   Mirrors the fields HSD_TObjLoadDesc + HSD_TObjTevLoadDesc carry so the
   runtime can reproduce the GX two-stage texture combine (see docs/WEBGL2.md). */
typedef struct {
    uint8_t tev_active;      /* bit0 COLOR_TEV, bit1 ALPHA_TEV (HSD_TObjTevActive) */
    uint8_t color_op;        /* 0 COMP, 1 ADD, 2 SUB */
    uint8_t alpha_op;
    uint8_t color_bias;      /* 0 ZERO, 1 +0.5, 2 -0.5, 3 +1 */
    uint8_t alpha_bias;
    uint8_t color_scale;     /* 0 x1, 1 x2, 2 x4, 3 x0.5 */
    uint8_t alpha_scale;
    uint8_t color_clamp;
    uint8_t alpha_clamp;
    uint8_t color_in[4];     /* HSD_TObjTevColorIn selectors */
    uint8_t alpha_in[4];     /* HSD_TObjTevAlphaIn selectors */
    uint8_t colormap;        /* HSD_TObjFlags (flags>>16)&0xF: NONE..SUB */
    uint8_t alphamap;        /* HSD_TObjFlags (flags>>20)&0xF */
    uint8_t wrap_s;          /* 0 CLAMP, 1 REPEAT, 2 MIRROR */
    uint8_t wrap_t;
    uint8_t repeat_s;
    uint8_t repeat_t;
    float blend;             /* TOBJ blending factor (for BLEND combine) */
    uint8_t constant[4];     /* TEV KONST color (RGBA) */
    uint8_t tev0[4];         /* TEV0 register (RGBA) */
    uint8_t tev1[4];         /* TEV1 register (RGBA) */
    float tex_scale[2];      /* TOBJ scale.x/y */
    float tex_rot;           /* TOBJ rotation.z (radians) */
    float tex_trans[2];      /* TOBJ translation.x/y */
} asset_tev_t;

typedef struct {
    int16_t texture_idx;  /* -1 = none */
    uint32_t indices_start;
    uint32_t indices_len;
    uint32_t mobj_flags;
    uint8_t model_group_idx;
    uint8_t pad[3];
    asset_tev_t tev;
} asset_pgroup_t;

typedef struct {
    uint8_t ambient[4], diffuse[4], specular[4];
    float alpha, shininess;
} asset_phong_t;

typedef struct {
    uint16_t width, height;
    uint32_t format;
    uint8_t *rgba;        /* width*height*4 */
} asset_texture_t;

typedef struct {
    uint32_t bone_count, vertex_count, index_count, pgroup_count;
    uint32_t phong_count, texture_count;
    asset_bone_t *bones;
    asset_vertex_t *vertices;
    uint16_t *indices;
    asset_pgroup_t *pgroups;
    asset_phong_t *phongs;
    asset_texture_t *textures;
} asset_model_t;

/* ---- animation clip (one action) ---- */

enum { ATK_NONE = 0, ATK_STEP, ATK_LINEAR, ATK_HERMITE };

typedef struct {
    float frame;
    float value;
    float in_tan, out_tan;
    uint8_t interp;
} asset_key_t;

/* track channel ids, matching FigaTrack obj_type for joints:
   1..3 rot x/y/z, 5..7 trans x/y/z, 8..10 scale x/y/z */
typedef struct {
    uint8_t channel;      /* channel id (1..10) */
    uint16_t start_frame;
    uint32_t key_count;
    asset_key_t *keys;
} asset_track_t;

typedef struct {
    uint16_t bone_index;
    uint32_t track_count;
    asset_track_t *tracks;
} asset_joint_anim_t;

typedef struct {
    char name[48];
    float end_frame;
    bool loop;
    uint32_t joint_count;
    asset_joint_anim_t *joints;
} asset_action_t;

typedef struct {
    uint32_t action_count;
    asset_action_t *actions;
} asset_anims_t;

/* ---- stage ---- */

typedef struct {
    uint8_t kind;         /* 0 ambient, 1 infinite, 2 point, 3 spot */
    uint8_t flags;
    uint8_t color[4];
    float pos[3];
    float dir[3];
    float a0, a1, a2, k0, k1, k2;
} asset_light_t;

typedef struct {
    float scale;
    float cam_pos[3];
    float cam_fov;        /* degrees */
    float cam_vert, cam_horiz;
    uint32_t section_count;
    asset_model_t *sections;
    uint32_t light_count;
    asset_light_t *lights;
} asset_stage_t;

/* ---- stage props (animated actors like Yoshi's Story shy guys) ----
   Extracted from GrSt.dat's `itemdata` root. Each prop carries a model
   (HSD Joint tree) + its animation clips. Live in a separate
   `stages/<slug>.props` file so it never perturbs the shared .stage/.anims
   layout or other stages. */
typedef struct {
    uint32_t kind;        /* item kind (e.g. It_Kind_Heiho) */
    float pos[3];         /* baked world position (root JOBJ translate) */
    asset_model_t model;
    asset_anims_t anims;
} asset_prop_t;

typedef struct {
    uint32_t prop_count;
    asset_prop_t *props;
} asset_props_t;

/* ---- load / free ---- */

asset_model_t *asset_model_load(const char *path);
asset_anims_t *asset_anims_load(const char *path);
asset_stage_t *asset_stage_load(const char *path);
asset_props_t *asset_props_load(const char *path);

void asset_model_free(asset_model_t *m);
void asset_anims_free(asset_anims_t *a);
void asset_stage_free(asset_stage_t *s);
void asset_props_free(asset_props_t *p);

/* ---- meta.json helpers (writer side) ---- */

typedef struct {
    char iso_fingerprint[64];
    uint32_t schema_version;
} asset_meta_t;

int asset_write_meta(const char *dir, const asset_meta_t *meta);

#endif /* ASSET_H */
