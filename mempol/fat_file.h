#ifndef FAT_FILE_H
#define FAT_FILE_H

#include <stddef.h>
#include <stdint.h>
#include "fat16.h"

#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif

typedef struct {
    uint8_t *data;   /* contiguous copy of file in RAM */
    uint32_t size;
    uint32_t pos;
} FAT_FILE;

/* Call once after fat16_mount() */
void fat_file_init(FAT16 *vol);

FAT_FILE *fat_fopen(const char *path, const char *mode);
size_t    fat_fread(void *buf, size_t sz, size_t count, FAT_FILE *f);
int       fat_fseek(FAT_FILE *f, long offset, int whence);
long      fat_ftell(FAT_FILE *f);
int       fat_feof(FAT_FILE *f);
int       fat_fclose(FAT_FILE *f);

/* Minimal fscanf: only supports "%d" */
int       fat_fscanf(FAT_FILE *f, const char *fmt, void *result);

/* Stub — disparity never writes files */
static inline size_t fat_fwrite_stub(const void *b, size_t s, size_t n, FAT_FILE *f) {
    (void)b; (void)s; (void)f; return n;
}
static inline int fat_fprintf_stub(FAT_FILE *f, const char *fmt, ...) {
    (void)f; (void)fmt; return 0;
}

#endif /* FAT_FILE_H */
