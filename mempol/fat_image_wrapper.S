/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 University of Waterloo
 */

/* Embed reduced FAT image at 0x82000000 for simulation */
.section .fat_image, "a", @progbits
.global _fat_image_start
.global _fat_image_end
.align 8

_fat_image_start:
.incbin "my_fat_image_reduced.img"
_fat_image_end:
