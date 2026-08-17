/*
 * asset.c - loads the binary cache produced by tools/extract into runtime
 * structs. See src/asset.h for the on-disk format.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asset.h"

/* ------------------------------------------------------------------ */
/* Reader helpers (cache is big-endian, matching the extractor writer) */
/* ------------------------------------------------------------------ */

typedef struct {
    const uint8_t *p;
    size_t len;
    size_t pos;
} reader_t;

static void r_fail(reader_t *r) {
    (void)r;
    fprintf(stderr, "asset: truncated cache file\n");
    exit(1);
}
static uint8_t rd8(reader_t *r) {
    if (r->pos + 1 > r->len) r_fail(r);
    return r->p[r->pos++];
}
static uint16_t rd16(reader_t *r) {
    if (r->pos + 2 > r->len) r_fail(r);
    uint16_t v = (uint16_t)((r->p[r->pos] << 8) | r->p[r->pos + 1]);
    r->pos += 2;
    return v;
}
static uint32_t rd32(reader_t *r) {
    if (r->pos + 4 > r->len) r_fail(r);
    const uint8_t *p = r->p + r->pos;
    uint32_t v = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                 ((uint32_t)p[2] << 8) | p[3];
    r->pos += 4;
    return v;
}
static float rdf32(reader_t *r) {
    uint32_t x = rd32(r);
    float f;
    memcpy(&f, &x, 4);
    return f;
}
static void rd_bytes(reader_t *r, void *dst, size_t n) {
    if (r->pos + n > r->len) r_fail(r);
    memcpy(dst, r->p + r->pos, n);
    r->pos += n;
}

/* ------------------------------------------------------------------ */
/* File loading                                                        */
/* ------------------------------------------------------------------ */

static reader_t load_file(const char *path) {
    reader_t r = {0};
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "asset: cannot open %s\n", path);
        r.p = NULL;
        return r;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    uint8_t *buf = malloc((size_t)sz);
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        fprintf(stderr, "asset: read error %s\n", path);
        free(buf);
        fclose(f);
        r.p = NULL;
        return r;
    }
    fclose(f);
    r.p = buf;
    r.len = (size_t)sz;
    r.pos = 0;
    return r;
}

static void check_magic(reader_t *r, uint32_t expect, const char *what) {
    uint32_t magic = rd32(r);
    if (magic != expect) {
        fprintf(stderr, "asset: bad magic in %s (0x%08X)\n", what, magic);
        exit(1);
    }
    uint32_t ver = rd32(r);
    if (ver != ASSET_SCHEMA_VERSION) {
        fprintf(stderr, "asset: %s schema mismatch (%u)\n", what, ver);
        exit(1);
    }
}

/* ------------------------------------------------------------------ */
/* Model                                                               */
/* ------------------------------------------------------------------ */

static asset_model_t *model_from_reader(reader_t *r) {
    check_magic(r, ASSET_MAGIC, "model");
    asset_model_t *m = calloc(1, sizeof(asset_model_t));
    m->bone_count = rd32(r);
    m->vertex_count = rd32(r);
    m->index_count = rd32(r);
    m->pgroup_count = rd32(r);
    m->phong_count = rd32(r);
    m->texture_count = rd32(r);
    m->bones = calloc(m->bone_count ? m->bone_count : 1, sizeof(asset_bone_t));
    for (uint32_t i = 0; i < m->bone_count; i++) {
        asset_bone_t *b = &m->bones[i];
        b->parent = rd16(r);
        b->pgroup_start = rd16(r);
        b->pgroup_len = rd16(r);
        b->pad = rd16(r);
        b->flags = rd32(r);
        for (int k = 0; k < 12; k++) b->base[k] = rdf32(r);
        for (int k = 0; k < 12; k++) b->inv_world[k] = rdf32(r);
    }
    m->vertices = calloc(m->vertex_count ? m->vertex_count : 1, sizeof(asset_vertex_t));
    for (uint32_t i = 0; i < m->vertex_count; i++) {
        asset_vertex_t *v = &m->vertices[i];
        for (int k = 0; k < 3; k++) v->pos[k] = rdf32(r);
        for (int k = 0; k < 3; k++) v->nrm[k] = rdf32(r);
        for (int k = 0; k < 2; k++) v->uv[k] = rdf32(r);
        rd_bytes(r, v->color, 4);
        for (int k = 0; k < 4; k++) v->weight[k] = rdf32(r);
        v->bone[0] = rd16(r);
        v->bone[1] = rd16(r);
        v->bone[2] = rd16(r);
        v->bone[3] = rd16(r);
    }
    m->indices = calloc(m->index_count ? m->index_count : 1, sizeof(uint16_t));
    for (uint32_t i = 0; i < m->index_count; i++) m->indices[i] = rd16(r);
    m->pgroups = calloc(m->pgroup_count ? m->pgroup_count : 1, sizeof(asset_pgroup_t));
    for (uint32_t i = 0; i < m->pgroup_count; i++) {
        asset_pgroup_t *pg = &m->pgroups[i];
        pg->texture_idx = (int16_t)rd16(r);
        pg->indices_start = rd32(r);
        pg->indices_len = rd32(r);
        pg->mobj_flags = rd32(r);
        pg->model_group_idx = rd8(r);
        rd8(r);
        rd16(r);
    }
    m->phongs = calloc(m->phong_count ? m->phong_count : 1, sizeof(asset_phong_t));
    for (uint32_t i = 0; i < m->phong_count; i++) {
        rd_bytes(r, m->phongs[i].ambient, 4);
        rd_bytes(r, m->phongs[i].diffuse, 4);
        rd_bytes(r, m->phongs[i].specular, 4);
        m->phongs[i].alpha = rdf32(r);
        m->phongs[i].shininess = rdf32(r);
    }
    m->textures = calloc(m->texture_count ? m->texture_count : 1, sizeof(asset_texture_t));
    for (uint32_t i = 0; i < m->texture_count; i++) {
        asset_texture_t *t = &m->textures[i];
        t->width = rd16(r);
        t->height = rd16(r);
        t->format = rd32(r);
        size_t n = (size_t)t->width * t->height * 4;
        t->rgba = malloc(n ? n : 1);
        rd_bytes(r, t->rgba, n);
    }
    return m;
}

asset_model_t *asset_model_load(const char *path) {
    reader_t r = load_file(path);
    if (!r.p) return NULL;
    asset_model_t *m = model_from_reader(&r);
    free((void *)r.p);
    return m;
}

/* ------------------------------------------------------------------ */
/* Animations                                                          */
/* ------------------------------------------------------------------ */

asset_anims_t *asset_anims_load(const char *path) {
    reader_t r = load_file(path);
    if (!r.p) return NULL;
    check_magic(&r, ASSET_MAGIC, path);
    asset_anims_t *a = calloc(1, sizeof(asset_anims_t));
    a->action_count = rd32(&r);
    a->actions = calloc(a->action_count ? a->action_count : 1, sizeof(asset_action_t));
    for (uint32_t i = 0; i < a->action_count; i++) {
        asset_action_t *act = &a->actions[i];
        rd_bytes(&r, act->name, 48);
        act->end_frame = rdf32(&r);
        act->loop = rd8(&r) != 0;
        rd8(&r);
        rd16(&r);
        act->joint_count = rd32(&r);
        act->joints = calloc(act->joint_count ? act->joint_count : 1,
                             sizeof(asset_joint_anim_t));
        for (uint32_t j = 0; j < act->joint_count; j++) {
            asset_joint_anim_t *ja = &act->joints[j];
            ja->bone_index = rd16(&r);
            ja->track_count = rd32(&r);
            ja->tracks = calloc(ja->track_count ? ja->track_count : 1,
                                sizeof(asset_track_t));
            for (uint32_t k = 0; k < ja->track_count; k++) {
                asset_track_t *tk = &ja->tracks[k];
                tk->channel = rd8(&r);
                tk->start_frame = rd16(&r);
                tk->key_count = rd32(&r);
                tk->keys = calloc(tk->key_count ? tk->key_count : 1,
                                  sizeof(asset_key_t));
                for (uint32_t q = 0; q < tk->key_count; q++) {
                    asset_key_t *k = &tk->keys[q];
                    k->frame = rdf32(&r);
                    k->value = rdf32(&r);
                    k->in_tan = rdf32(&r);
                    k->out_tan = rdf32(&r);
                    k->interp = rd8(&r);
                }
            }
        }
    }
    free((void *)r.p);
    return a;
}

/* ------------------------------------------------------------------ */
/* Stage                                                               */
/* ------------------------------------------------------------------ */

asset_stage_t *asset_stage_load(const char *path) {
    reader_t r = load_file(path);
    if (!r.p) return NULL;
    check_magic(&r, ASSET_MAGIC, path);
    asset_stage_t *s = calloc(1, sizeof(asset_stage_t));
    s->scale = rdf32(&r);
    for (int i = 0; i < 3; i++) s->cam_pos[i] = rdf32(&r);
    s->cam_fov = rdf32(&r);
    s->cam_vert = rdf32(&r);
    s->cam_horiz = rdf32(&r);
    s->section_count = rd32(&r);
    s->light_count = rd32(&r);
    s->sections = calloc(s->section_count ? s->section_count : 1,
                         sizeof(asset_model_t));
    for (uint32_t i = 0; i < s->section_count; i++)
        s->sections[i] = *model_from_reader(&r);
    s->lights = calloc(s->light_count ? s->light_count : 1, sizeof(asset_light_t));
    for (uint32_t i = 0; i < s->light_count; i++) {
        asset_light_t *l = &s->lights[i];
        l->kind = rd8(&r);
        l->flags = rd8(&r);
        rd_bytes(&r, l->color, 4);
        for (int k = 0; k < 3; k++) l->pos[k] = rdf32(&r);
        for (int k = 0; k < 3; k++) l->dir[k] = rdf32(&r);
        l->a0 = rdf32(&r); l->a1 = rdf32(&r); l->a2 = rdf32(&r);
        l->k0 = rdf32(&r); l->k1 = rdf32(&r); l->k2 = rdf32(&r);
    }
    free((void *)r.p);
    return s;
}

/* ------------------------------------------------------------------ */
/* Free                                                                */
/* ------------------------------------------------------------------ */

void asset_model_free(asset_model_t *m) {
    if (!m) return;
    free(m->bones);
    free(m->vertices);
    free(m->indices);
    free(m->pgroups);
    free(m->phongs);
    for (uint32_t i = 0; i < m->texture_count; i++) free(m->textures[i].rgba);
    free(m->textures);
    free(m);
}

void asset_anims_free(asset_anims_t *a) {
    if (!a) return;
    for (uint32_t i = 0; i < a->action_count; i++) {
        asset_action_t *act = &a->actions[i];
        for (uint32_t j = 0; j < act->joint_count; j++) {
            asset_joint_anim_t *ja = &act->joints[j];
            for (uint32_t k = 0; k < ja->track_count; k++)
                free(ja->tracks[k].keys);
            free(ja->tracks);
            free(ja->spline_cv);
        }
        free(act->joints);
    }
    free(a->actions);
    free(a);
}

void asset_stage_free(asset_stage_t *s) {
    if (!s) return;
    for (uint32_t i = 0; i < s->section_count; i++) {
        asset_model_t *m = &s->sections[i];
        free(m->bones);
        free(m->vertices);
        free(m->indices);
        free(m->pgroups);
        free(m->phongs);
        for (uint32_t t = 0; t < m->texture_count; t++) free(m->textures[t].rgba);
        free(m->textures);
    }
    free(s->sections);
    free(s->lights);
    free(s);
}
