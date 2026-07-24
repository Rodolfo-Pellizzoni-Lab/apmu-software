/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 University of Waterloo
 */

#ifndef FAT16_H
#define FAT16_H

#include <stdint.h>
#include <string.h>

/*
 * Minimal FAT16 for a RAM-resident disk image.
 *
 * The FAT image loaded by GDB sits at:
 *   0x82000000  = MBR / start of raw disk image
 *   0x82100000  = FAT16 partition start (LBA 2048, 1 MB into image)
 *
 * All "disk I/O" is just pointer arithmetic — no real I/O needed.
 */

#define FAT_PART_BASE  ((uint8_t *)0x82100000UL)

typedef struct {
    uint16_t bps;       /* bytes per sector          */
    uint8_t  spc;       /* sectors per cluster       */
    uint16_t rsvd;      /* reserved sectors          */
    uint8_t  nfats;     /* number of FATs            */
    uint16_t rdcnt;     /* root directory entry count*/
    uint16_t spf;       /* sectors per FAT           */
    uint32_t fat_sec;   /* FAT1 start sector         */
    uint32_t root_sec;  /* root directory start sector*/
    uint32_t data_sec;  /* data area start sector    */
} FAT16;

/* Parse BPB at 0x82100000. Returns 0 on success, -1 on bad signature. */
int      fat16_mount(FAT16 *f);

/* Return a pointer to the raw bytes of a sector (within the volume). */
uint8_t *fat16_sector(FAT16 *f, uint32_t sector);

/* Return a pointer to the data bytes of a cluster (cluster >= 2). */
uint8_t *fat16_cluster_ptr(FAT16 *f, uint16_t cluster);

/* Allocate a free cluster, zero it, mark EOC in FAT. Returns 0 on failure. */
uint16_t fat16_alloc(FAT16 *f);

/* Search root directory for an 8.3 name (11 chars, space-padded).
   Returns pointer to the 32-byte directory entry, or NULL if not found. */
uint8_t *fat16_find(FAT16 *f, const char name11[11]);

/* Find a free (empty/deleted) root directory slot. Returns NULL if full. */
uint8_t *fat16_free_entry(FAT16 *f);

/* Create a file: allocate dir entry + cluster. Returns dir entry ptr.
   Caller must later call fat16_set_size() after writing data. */
uint8_t *fat16_create(FAT16 *f, const char name11[11], uint16_t *cluster_out);

/* Helpers to read fields from a directory entry */
static inline uint16_t fat16_dir_cluster(const uint8_t *e) {
    return (uint16_t)(e[26] | (e[27] << 8));
}
static inline uint32_t fat16_dir_size(const uint8_t *e) {
    return (uint32_t)(e[28] | (e[29]<<8) | (e[30]<<16) | (e[31]<<24));
}
static inline void fat16_set_size(uint8_t *e, uint32_t sz) {
    e[28] = sz & 0xFF; e[29] = (sz>>8)&0xFF;
    e[30] = (sz>>16)&0xFF; e[31] = (sz>>24)&0xFF;
}
/* Follow FAT chain: returns next cluster, 0xFFFF = EOC, 0xFFF7 = bad */
static inline uint16_t fat16_next_cluster(FAT16 *f, uint16_t cluster) {
    uint8_t *fat = fat16_sector(f, f->fat_sec);
    return (uint16_t)(fat[cluster * 2] | (fat[cluster * 2 + 1] << 8));
}

#endif /* FAT16_H */
