#include "fat_file.h"
#include "fat16.h"
#include <string.h>
#include <stddef.h>

/* Stub — writeMatrix only used under GENERATE_OUTPUT which is never defined */
void writeMatrix(void *input, char *inpath)  { (void)input; (void)inpath; }
void fWriteMatrix(void *input, char *inpath) { (void)input; (void)inpath; }

/* Provided by bare_malloc.c */
extern void *bare_malloc(size_t size);
extern void  bare_free(void *ptr);

static FAT16 *g_vol = (void *)0;

void fat_file_init(FAT16 *vol) {
    g_vol = vol;
}

/* ---- name helpers ---- */

static char fat_upper(char c) {
    return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
}

/*
 * Convert a filename component (e.g. "1.bmp" or "expected_C.txt") to
 * FAT 8.3 format stored as two arrays: name[8] and ext[3], space-padded,
 * upper-cased.  Returns 1 if the base name was truncated (> 8 chars).
 */
static int to_83(const char *comp, char name[8], char ext[3]) {
    /* find last dot */
    const char *dot = (void *)0;
    for (const char *p = comp; *p; p++)
        if (*p == '.') dot = p;

    const char *base = comp;
    int base_len = dot ? (int)(dot - comp) : (int)strlen(comp);
    const char *ext_str = dot ? dot + 1 : "";
    int ext_len = (int)strlen(ext_str);
    int truncated = (base_len > 8);

    /* fill name */
    for (int i = 0; i < 8; i++) name[i] = ' ';
    for (int i = 0; i < base_len && i < 8; i++) name[i] = fat_upper(base[i]);

    /* fill ext */
    for (int i = 0; i < 3; i++) ext[i] = ' ';
    for (int i = 0; i < ext_len && i < 3; i++) ext[i] = fat_upper(ext_str[i]);

    return truncated;
}

/*
 * Match a directory entry (11-byte 8.3 field) against a path component.
 * Handles:
 *   1. Exact 8.3 match (case-insensitive via our upper-casing).
 *   2. Short-name match for long filenames (e.g. "expected_C.txt" → "EXPECT~1.TXT").
 */
static int entry_matches(const uint8_t *e, const char *comp) {
    char name[8], ext[3];
    int truncated = to_83(comp, name, ext);

    /* Exact 8.3 match */
    if (memcmp(e, name, 8) == 0 && memcmp(e + 8, ext, 3) == 0)
        return 1;

    /* Short-name fuzzy: first 6 chars match, then '~', extension matches */
    if (truncated) {
        int stem = 6;
        int ok = 1;
        for (int i = 0; i < stem; i++)
            if (e[i] != name[i]) { ok = 0; break; }
        if (ok && e[6] == '~' && memcmp(e + 8, ext, 3) == 0)
            return 1;
    }
    return 0;
}

/* ---- directory search ---- */

/* Find a name in the root directory; returns dir entry pointer or NULL */
static uint8_t *find_in_root(const char *comp) {
    uint8_t *dir = fat16_sector(g_vol, g_vol->root_sec);
    for (uint16_t i = 0; i < g_vol->rdcnt; i++) {
        uint8_t *e = dir + i * 32;
        if (e[0] == 0x00) break;
        if (e[0] == 0xE5) continue;
        if (e[11] & 0x08) continue;   /* volume label */
        if (entry_matches(e, comp)) return e;
    }
    return (void *)0;
}

/* Find a name in a cluster-chain directory; returns dir entry pointer or NULL */
static uint8_t *find_in_dir(uint16_t start_cluster, const char *comp) {
    uint16_t cluster = start_cluster;
    uint32_t entries_per_cluster = ((uint32_t)g_vol->spc * g_vol->bps) / 32;

    while (cluster >= 2 && cluster < 0xFFF7) {
        uint8_t *data = fat16_cluster_ptr(g_vol, cluster);
        for (uint32_t i = 0; i < entries_per_cluster; i++) {
            uint8_t *e = data + i * 32;
            if (e[0] == 0x00) return (void *)0;
            if (e[0] == 0xE5) continue;
            if (e[11] & 0x08) continue;
            if (e[0] == '.') continue;
            if (entry_matches(e, comp)) return e;
        }
        cluster = fat16_next_cluster(g_vol, cluster);
    }
    return (void *)0;
}

/* ---- path resolution ---- */

/*
 * Resolve a path like "DISP/1.bmp" or "1.bmp" to a directory entry.
 * Only handles depth-1 sub-directories (which is all disparity needs).
 */
static uint8_t *resolve_path(const char *path) {
    /* Split on first '/' */
    char buf[64];
    int len = (int)strlen(path);
    if (len >= (int)sizeof(buf)) return (void *)0;
    memcpy(buf, path, len + 1);

    char *slash = (void *)0;
    for (char *p = buf; *p; p++)
        if (*p == '/') { slash = p; break; }

    if (!slash) {
        /* Single component — look in root */
        return find_in_root(buf);
    }

    /* Two components: dir/file */
    *slash = '\0';
    const char *dir_name  = buf;
    const char *file_name = slash + 1;

    uint8_t *dir_entry = find_in_root(dir_name);
    if (!dir_entry) return (void *)0;
    if (!(dir_entry[11] & 0x10)) return (void *)0;  /* not a directory */

    uint16_t dir_cluster = fat16_dir_cluster(dir_entry);
    return find_in_dir(dir_cluster, file_name);
}

/* ---- public API ---- */

FAT_FILE *fat_fopen(const char *path, const char *mode) {
    (void)mode;
    if (!g_vol) return (void *)0;

    uint8_t *entry = resolve_path(path);
    if (!entry) return (void *)0;

    uint32_t size    = fat16_dir_size(entry);
    uint16_t cluster = fat16_dir_cluster(entry);

    /* Allocate FILE struct and data buffer */
    FAT_FILE *f = (FAT_FILE *)bare_malloc(sizeof(FAT_FILE));
    if (!f) return (void *)0;

    uint8_t *buf = (uint8_t *)bare_malloc(size + 1);
    if (!buf) return (void *)0;

    /* Copy cluster chain into contiguous buffer */
    uint32_t cluster_bytes = (uint32_t)g_vol->spc * g_vol->bps;
    uint32_t copied = 0;

    while (cluster >= 2 && cluster < 0xFFF7 && copied < size) {
        uint8_t *cdata = fat16_cluster_ptr(g_vol, cluster);
        uint32_t to_copy = cluster_bytes;
        if (copied + to_copy > size) to_copy = size - copied;
        memcpy(buf + copied, cdata, to_copy);
        copied += to_copy;
        cluster = fat16_next_cluster(g_vol, cluster);
    }
    buf[size] = 0;  /* null-terminate (useful for text reads) */

    f->data = buf;
    f->size = size;
    f->pos  = 0;
    return f;
}

size_t fat_fread(void *buf, size_t sz, size_t count, FAT_FILE *f) {
    if (!f || !buf) return 0;
    uint32_t want  = (uint32_t)(sz * count);
    uint32_t avail = f->size - f->pos;
    if (want > avail) want = avail;
    memcpy(buf, f->data + f->pos, want);
    f->pos += want;
    return want / sz;
}

int fat_fseek(FAT_FILE *f, long offset, int whence) {
    if (!f) return -1;
    long base;
    if      (whence == SEEK_SET) base = 0;
    else if (whence == SEEK_CUR) base = (long)f->pos;
    else if (whence == SEEK_END) base = (long)f->size;
    else return -1;

    long newpos = base + offset;
    if (newpos < 0) newpos = 0;
    if (newpos > (long)f->size) newpos = (long)f->size;
    f->pos = (uint32_t)newpos;
    return 0;
}

long fat_ftell(FAT_FILE *f) {
    return f ? (long)f->pos : -1;
}

int fat_feof(FAT_FILE *f) {
    return (!f || f->pos >= f->size) ? 1 : 0;
}

int fat_fclose(FAT_FILE *f) {
    /* bump allocator: no real free, just mark as done */
    if (f) { f->data = (void *)0; f->size = 0; f->pos = 0; }
    return 0;
}

/* ---- minimal fscanf: only "%d" ---- */
int fat_fscanf(FAT_FILE *f, const char *fmt, void *result) {
    if (!f || f->pos >= f->size) return -1;

    /* Skip whitespace */
    while (f->pos < f->size &&
           (f->data[f->pos] == ' '  || f->data[f->pos] == '\t' ||
            f->data[f->pos] == '\n' || f->data[f->pos] == '\r'))
        f->pos++;

    if (f->pos >= f->size) return -1;

    if (fmt[0] == '%' && (fmt[1] == 'd' || fmt[1] == 'i')) {
        int sign = 1, val = 0;
        if (f->data[f->pos] == '-') { sign = -1; f->pos++; }
        if (f->pos >= f->size || f->data[f->pos] < '0' || f->data[f->pos] > '9')
            return 0;
        while (f->pos < f->size && f->data[f->pos] >= '0' && f->data[f->pos] <= '9')
            val = val * 10 + (int)(f->data[f->pos++] - '0');
        *(int *)result = sign * val;
        return 1;
    }
    if (fmt[0] == '%' && fmt[1] == 'f') {
        float sign = 1.0f, val = 0.0f, frac = 0.0f, fdiv = 1.0f;
        if (f->data[f->pos] == '-') { sign = -1.0f; f->pos++; }
        else if (f->data[f->pos] == '+') { f->pos++; }
        if (f->pos >= f->size || f->data[f->pos] < '0' || f->data[f->pos] > '9')
            return 0;
        while (f->pos < f->size && f->data[f->pos] >= '0' && f->data[f->pos] <= '9')
            val = val * 10.0f + (float)(f->data[f->pos++] - '0');
        if (f->pos < f->size && f->data[f->pos] == '.') {
            f->pos++;
            while (f->pos < f->size && f->data[f->pos] >= '0' && f->data[f->pos] <= '9') {
                frac = frac * 10.0f + (float)(f->data[f->pos++] - '0');
                fdiv *= 10.0f;
            }
            val += frac / fdiv;
        }
        *(float *)result = sign * val;
        return 1;
    }
    return 0;
}
