/*
 * datdump.c - Phase A exploration tool.
 *
 * Reads a GameCube ISO's FST, locates .dat files, extracts one by path,
 * and dumps the DAT container header + root/reference nodes + names.
 * With --jobj it walks the first *_joint root's JOBJ tree (bones + DOBJ
 * counts) so the docs can be grounded in real bytes.
 *
 * Original C, written from the format facts in docs/DAT.md and docs/HSD.md.
 */

#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int32_t i32;

static u32 rd_u32(const u8 *p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }
static float rd_f32(const u8 *p) { u32 x = rd_u32(p); float f; memcpy(&f, &x, 4); return f; }

#define MAX_PATH 1024

typedef struct {
    char *path;
    u32 offset;
    u32 size;
} DatEntry;

typedef struct {
    DatEntry *items;
    size_t count;
    size_t cap;
} DatList;

typedef struct {
    const u8 *bytes;
    size_t len;
    u32 file_size;
    u32 data_block_size;
    u32 reloc_count;
    u32 root_count;
    u32 ref_count;
    u32 unknown_0x14;
    u32 unknown_0x18;
    u32 unknown_0x1c;
    size_t reloc_start;
    size_t roots_start;
    size_t refs_start;
    size_t string_start;
} DatDoc;

static void dat_list_push(DatList *l, DatEntry e) {
    if (l->count == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 64;
        l->items = realloc(l->items, l->cap * sizeof(DatEntry));
        if (!l->items) { fprintf(stderr, "oom\n"); exit(1); }
    }
    l->items[l->count++] = e;
}

/* ---- ISO / FST ---- */

static u8 *iso_slice(FILE *f, u32 off, u32 size) {
    u8 *buf = malloc(size);
    if (!buf) return NULL;
    fseek(f, off, SEEK_SET);
    if (fread(buf, 1, size, f) != size) { free(buf); return NULL; }
    return buf;
}

static void index_dat_files(FILE *iso, u32 fst_off, u32 fst_size, DatList *out) {
    u8 *fst = iso_slice(iso, fst_off, fst_size);
    if (!fst) { fprintf(stderr, "cannot read FST\n"); exit(1); }

    u32 entry_count = rd_u32(fst + 8);
    if (entry_count == 0 || entry_count > fst_size / 12) {
        fprintf(stderr, "bad FST entry count %u\n", entry_count);
        exit(1);
    }
    size_t name_base = (size_t)entry_count * 12;

    /* directory stack: end_index + accumulated path */
    typedef struct { u32 end; char path[MAX_PATH]; } DirFrame;
    static DirFrame stack[128];
    size_t sp = 0;
    char cur[MAX_PATH] = "";

    for (u32 i = 1; i < entry_count; i++) {
        const u8 *e = fst + (size_t)i * 12;
        while (sp > 0 && i >= stack[sp - 1].end) {
            sp--;
            if (sp == 0) cur[0] = 0;
            else strcpy(cur, stack[sp - 1].path);
        }
        u32 type_name = rd_u32(e);
        u32 is_dir = (type_name >> 24) & 0xFF;
        u32 name_off = type_name & 0x00FFFFFF;
        char name[256];
        name[0] = 0;
        if (name_base + name_off < fst_size) {
            size_t avail = fst_size - (name_base + name_off);
            size_t want = avail < sizeof(name) - 1 ? avail : sizeof(name) - 1;
            strncpy(name, (const char *)fst + name_base + name_off, want);
            name[want] = 0;
        }

        if (is_dir) {
            u32 end = rd_u32(e + 8);
            if (sp >= 128) { fprintf(stderr, "dir stack overflow\n"); exit(1); }
            DirFrame *df = &stack[sp++];
            df->end = end;
            if (cur[0]) snprintf(df->path, sizeof(df->path), "%s/%s", cur, name);
            else snprintf(df->path, sizeof(df->path), "%s", name);
            strcpy(cur, df->path);
        } else {
            u32 off = rd_u32(e + 4);
            u32 size = rd_u32(e + 8);
            size_t n = strlen(name);
            if (n >= 4 && strcasecmp(name + n - 4, ".dat") == 0) {
                char full[MAX_PATH];
                if (cur[0]) snprintf(full, sizeof(full), "%s/%s", cur, name);
                else snprintf(full, sizeof(full), "%s", name);
                dat_list_push(out, (DatEntry){ .path = strdup(full), .offset = off, .size = size });
            }
        }
    }
    free(fst);
}

/* ---- DAT container ---- */

static int parse_dat(const u8 *bytes, size_t len, DatDoc *d) {
    memset(d, 0, sizeof(*d));
    d->bytes = bytes;
    d->len = len;
    if (len < 0x20) return -1;
    d->file_size = rd_u32(bytes + 0x00);
    d->data_block_size = rd_u32(bytes + 0x04);
    d->reloc_count = rd_u32(bytes + 0x08);
    d->root_count = rd_u32(bytes + 0x0C);
    d->ref_count = rd_u32(bytes + 0x10);
    d->unknown_0x14 = rd_u32(bytes + 0x14);
    d->unknown_0x18 = rd_u32(bytes + 0x18);
    d->unknown_0x1c = rd_u32(bytes + 0x1C);

    if (d->file_size != (u32)len) return -2;

    size_t reloc = 0x20 + (size_t)d->data_block_size;
    size_t roots = reloc + (size_t)d->reloc_count * 4;
    size_t refs = roots + (size_t)d->root_count * 8;
    size_t strs = refs + (size_t)d->ref_count * 8;
    if (roots > len || refs > len || strs > len) return -3;
    d->reloc_start = reloc;
    d->roots_start = roots;
    d->refs_start = refs;
    d->string_start = strs;
    return 0;
}

static const char *dat_name(const DatDoc *d, u32 name_rel) {
    size_t off = d->string_start + name_rel;
    if (off >= d->len) return "?";
    return (const char *)d->bytes + off;
}

static void dump_header(const DatDoc *d) {
    printf("== DAT header (HSD Archive) ==\n");
    printf("  fileSize(%02X)           0x%08X (%u)\n", 0x00, d->file_size, d->file_size);
    printf("  dataSize(%02X)           0x%08X (%u)\n", 0x04, d->data_block_size, d->data_block_size);
    printf("  nbReloc(%02X)            0x%08X (%u)\n", 0x08, d->reloc_count, d->reloc_count);
    printf("  nbPublic/roots(%02X)     0x%08X (%u)\n", 0x0C, d->root_count, d->root_count);
    printf("  nbExtern/refs(%02X)      0x%08X (%u)\n", 0x10, d->ref_count, d->ref_count);
    char ver[5] = { (char)(d->unknown_0x14 >> 24), (char)(d->unknown_0x14 >> 16),
                    (char)(d->unknown_0x14 >> 8), (char)d->unknown_0x14, 0 };
    int printable = 1;
    for (int i = 0; i < 4; i++)
        if (ver[i] < 0x20 || ver[i] > 0x7E) printable = 0;
    printf("  version(%02X)            0x%08X \"%s\"\n", 0x14, d->unknown_0x14,
           printable ? ver : "");
    printf("  pad(%02X)                0x%08X\n", 0x18, d->unknown_0x18);
    printf("  pad(%02X)                0x%08X\n", 0x1C, d->unknown_0x1c);
    printf("  reloc section @ 0x%zX (%u * 4 bytes)\n", d->reloc_start, d->reloc_count);
    printf("  roots section @ 0x%zX (%u * 8 bytes)\n", d->roots_start, d->root_count);
    printf("  refs section  @ 0x%zX (%u * 8 bytes)\n", d->refs_start, d->ref_count);
    printf("  string table  @ 0x%zX\n", d->string_start);
}

static void dump_nodes(const DatDoc *d) {
    printf("== root nodes ==\n");
    for (u32 i = 0; i < d->root_count; i++) {
        const u8 *e = d->bytes + d->roots_start + (size_t)i * 8;
        u32 data_off = rd_u32(e);
        u32 name_off = rd_u32(e + 4);
        printf("  root[%2u] dataOff=0x%08X (abs 0x%08X) name=\"%s\"\n",
               i, data_off, 0x20 + data_off, dat_name(d, name_off));
    }
    printf("== reference nodes ==\n");
    for (u32 i = 0; i < d->ref_count; i++) {
        const u8 *e = d->bytes + d->refs_start + (size_t)i * 8;
        u32 data_off = rd_u32(e);
        u32 name_off = rd_u32(e + 4);
        printf("  ref[%2u]  dataOff=0x%08X (abs 0x%08X) name=\"%s\"\n",
               i, data_off, 0x20 + data_off, dat_name(d, name_off));
    }
}

static void dump_relocs(const DatDoc *d, int max) {
    printf("== relocation table (first %d of %u) ==\n", max, d->reloc_count);
    u32 shown = d->reloc_count < (u32)max ? d->reloc_count : (u32)max;
    for (u32 i = 0; i < shown; i++) {
        u32 rel = rd_u32(d->bytes + d->reloc_start + (size_t)i * 4);
        printf("  reloc[%4u] dataRel=0x%08X abs=0x%08X\n", i, rel, 0x20 + rel);
    }
}

/* ---- JOBJ walk (grounding for HSD.md) ---- */

static void dump_jobj_tree(const DatDoc *d, u32 data_rel, int depth, int max_depth) {
    if (depth > max_depth) return;
    u32 abs = 0x20 + data_rel;
    if (abs + 0x40 > d->len) return;

    const u8 *j = d->bytes + abs;
    u32 class_off = rd_u32(j + 0x00);
    u32 flags = rd_u32(j + 0x04);
    u32 child_rel = rd_u32(j + 0x08);
    u32 next_rel = rd_u32(j + 0x0C);
    u32 dobj_rel = rd_u32(j + 0x10);
    float rx = rd_f32(j + 0x14), ry = rd_f32(j + 0x18), rz = rd_f32(j + 0x1C);
    float sx = rd_f32(j + 0x20), sy = rd_f32(j + 0x24), sz = rd_f32(j + 0x28);
    float tx = rd_f32(j + 0x2C), ty = rd_f32(j + 0x30), tz = rd_f32(j + 0x34);
    u32 inv_rel = rd_u32(j + 0x38);
    u32 robj_rel = rd_u32(j + 0x3C);

    (void)class_off; /* className string ref, usually 0 in these files */

    printf("%*sJOBJ dataRel=0x%08X flags=0x%08X classOff=0x%08X\n", depth * 2, "",
           data_rel, flags, class_off);
    printf("%*s  child=0x%08X next=0x%08X dobj=0x%08X\n", depth * 2, "",
           child_rel, next_rel, dobj_rel);
    printf("%*s  rot=(%.4f, %.4f, %.4f) scale=(%.4f, %.4f, %.4f) trans=(%.4f, %.4f, %.4f)\n",
           depth * 2, "", rx, ry, rz, sx, sy, sz, tx, ty, tz);
    printf("%*s  invWorld=0x%08X robj=0x%08X\n", depth * 2, "", inv_rel, robj_rel);

    if (dobj_rel) {
        u32 dabs = 0x20 + dobj_rel;
        if (dabs + 0x10 <= d->len) {
            const u8 *do_ = d->bytes + dabs;
            u32 mobj_rel = rd_u32(do_ + 0x08);
            u32 pobj_rel = rd_u32(do_ + 0x0C);
            printf("%*s  -> DOBJ mobj=0x%08X pobj=0x%08X\n", depth * 2, "", mobj_rel, pobj_rel);
        }
    }

    if (child_rel) dump_jobj_tree(d, child_rel, depth + 1, max_depth);
    if (next_rel) dump_jobj_tree(d, next_rel, depth, max_depth);
}

int main(int argc, char **argv) {
    const char *iso_path = "fixtures/game.iso";
    const char *dat_path = NULL;
    int do_jobj = 0;
    int max_depth = 6;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--jobj") == 0) do_jobj = 1;
        else if (strncmp(argv[i], "--depth=", 8) == 0) max_depth = atoi(argv[i] + 8);
        else if (strncmp(argv[i], "--iso=", 6) == 0) iso_path = argv[i] + 6;
        else dat_path = argv[i];
    }
    if (!dat_path) {
        fprintf(stderr, "usage: %s [--iso=PATH] [--jobj] [--depth=N] PlFc.dat\n", argv[0]);
        return 2;
    }

    FILE *iso = fopen(iso_path, "rb");
    if (!iso) { fprintf(stderr, "cannot open %s\n", iso_path); return 1; }

    u8 hdr[0x430];
    if (fread(hdr, 1, sizeof(hdr), iso) != sizeof(hdr)) { fprintf(stderr, "short disc header\n"); return 1; }
    u32 fst_off = rd_u32(hdr + 0x424);
    u32 fst_size = rd_u32(hdr + 0x428);

    DatList dats = {0};
    index_dat_files(iso, fst_off, fst_size, &dats);
    printf("FST: offset=0x%08X size=0x%X  .dat files=%zu\n", fst_off, fst_size, dats.count);

    DatEntry *hit = NULL;
    for (size_t i = 0; i < dats.count; i++)
        if (strcasecmp(dats.items[i].path, dat_path) == 0) { hit = &dats.items[i]; break; }
    if (!hit) {
        fprintf(stderr, "file '%s' not found in ISO\n", dat_path);
        for (size_t i = 0; i < dats.count && i < 40; i++)
            printf("  %s\n", dats.items[i].path);
        return 1;
    }
    printf("found %s @ 0x%08X size=%u\n", hit->path, hit->offset, hit->size);

    u8 *bytes = iso_slice(iso, hit->offset, hit->size);
    if (!bytes) { fprintf(stderr, "read failed\n"); return 1; }

    DatDoc doc;
    int rc = parse_dat(bytes, hit->size, &doc);
    if (rc < 0) { fprintf(stderr, "DAT parse failed (%d)\n", rc); return 1; }

    dump_header(&doc);
    dump_nodes(&doc);
    dump_relocs(&doc, 8);

    if (do_jobj) {
        /* find first root whose name looks like a *_joint mesh root */
        for (u32 i = 0; i < doc.root_count; i++) {
            const u8 *e = doc.bytes + doc.roots_start + (size_t)i * 8;
            u32 data_off = rd_u32(e);
            u32 name_off = rd_u32(e + 4);
            const char *nm = dat_name(&doc, name_off);
            if (strstr(nm, "_joint") && !strstr(nm, "shapeanim") && !strstr(nm, "matanim")) {
                printf("== JOBJ tree from root \"%s\" (dataRel=0x%08X) ==\n", nm, data_off);
                dump_jobj_tree(&doc, data_off, 0, max_depth);
                break;
            }
        }
    }

    free(bytes);
    fclose(iso);
    return 0;
}