#include "fat16.h"
#include <string.h>
#include <stdio.h>

/* memcmp is not in the he-soc string_lib — provide our own */
int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *p = a, *q = b;
    for (size_t i = 0; i < n; i++) {
        if (p[i] != q[i]) return (int)p[i] - (int)q[i];
    }
    return 0;
}

/* ---- little-endian helpers ---- */
static inline uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
static inline uint32_t rd32(const uint8_t *p) {
    return (uint32_t)(p[0] | (p[1]<<8) | (p[2]<<16) | (p[3]<<24));
}
static inline void wr16(uint8_t *p, uint16_t v) {
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF;
}

/* ---- sector / cluster addressing ---- */
uint8_t *fat16_sector(FAT16 *f, uint32_t sector) {
    return FAT_PART_BASE + (uint32_t)(sector * f->bps);
}

uint8_t *fat16_cluster_ptr(FAT16 *f, uint16_t cluster) {
    uint32_t sec = f->data_sec + (uint32_t)(cluster - 2) * f->spc;
    return fat16_sector(f, sec);
}

/* ---- mount ---- */
int fat16_mount(FAT16 *f) {
    uint8_t *bs = FAT_PART_BASE;

    /* Check boot sector signature */
    if (bs[510] != 0x55 || bs[511] != 0xAA)
        printf("WARN: fat16_mount - bad signature 0x%02x%02x, continuing anyway\r\n",
               bs[510], bs[511]);

    f->bps   = rd16(bs + 0x0B);
    f->spc   = bs[0x0D];
    f->rsvd  = rd16(bs + 0x0E);
    f->nfats = bs[0x10];
    f->rdcnt = rd16(bs + 0x11);
    f->spf   = rd16(bs + 0x16);

    f->fat_sec  = f->rsvd;
    f->root_sec = f->fat_sec + (uint32_t)f->nfats * f->spf;

    uint32_t root_bytes = (uint32_t)f->rdcnt * 32;
    uint32_t root_secs  = (root_bytes + f->bps - 1) / f->bps;
    f->data_sec = f->root_sec + root_secs;

    return 0;
}

/* ---- FAT allocation ---- */
uint16_t fat16_alloc(FAT16 *f) {
    uint8_t *fat1 = fat16_sector(f, f->fat_sec);
    uint8_t *fat2 = (f->nfats > 1) ? fat16_sector(f, f->fat_sec + f->spf) : 0;

    for (uint16_t c = 2; c < 0xFFF7; c++) {
        if (rd16(fat1 + c * 2) == 0x0000) {
            wr16(fat1 + c * 2, 0xFFFF);          /* mark EOC in FAT1 */
            if (fat2) wr16(fat2 + c * 2, 0xFFFF); /* mirror to FAT2  */
            memset(fat16_cluster_ptr(f, c), 0, (uint32_t)f->spc * f->bps);
            return c;
        }
    }
    return 0; /* disk full */
}

/* ---- directory search ---- */
uint8_t *fat16_find(FAT16 *f, const char name11[11]) {
    uint8_t *dir = fat16_sector(f, f->root_sec);
    for (uint16_t i = 0; i < f->rdcnt; i++) {
        uint8_t *e = dir + i * 32;
        if (e[0] == 0x00) break;           /* end of used entries */
        if (e[0] == 0xE5) continue;        /* deleted             */
        if (e[11] & 0x08) continue;        /* volume label        */
        if (memcmp(e, name11, 11) == 0) return e;
    }
    return 0;
}

uint8_t *fat16_free_entry(FAT16 *f) {
    uint8_t *dir = fat16_sector(f, f->root_sec);
    for (uint16_t i = 0; i < f->rdcnt; i++) {
        uint8_t *e = dir + i * 32;
        if (e[0] == 0x00 || e[0] == 0xE5) return e;
    }
    return 0; /* root dir full */
}

/* ---- create ---- */
uint8_t *fat16_create(FAT16 *f, const char name11[11], uint16_t *cluster_out) {
    uint8_t *e = fat16_free_entry(f);
    if (!e) return 0;

    uint16_t c = fat16_alloc(f);
    if (!c) return 0;

    memset(e, 0, 32);
    memcpy(e, name11, 11);
    e[11] = 0x20;           /* attribute: archive */
    wr16(e + 26, c);        /* first cluster low  */

    *cluster_out = c;
    return e;
}
