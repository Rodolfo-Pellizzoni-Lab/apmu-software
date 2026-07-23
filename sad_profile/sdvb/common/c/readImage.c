/********************************
Author: Sravanthi Kota Venkata
Modified for bare-metal FAT16.
********************************/

#include "sdvbs_common.h"
#include "fat_file.h"

I2D* readImage(const char* pathName)
{
    char signature[2];
    int file_size, loc_of_bitmap, size_of_infoheader;
    int width, height, compression_method, bytes_of_bitmap;
    int hori_reso, vert_reso, no_of_colors, no_of_imp_colors;
    short int reserved1, reserved2, number_of_planes, bits_per_pixel;
    int nI, nJ;
    unsigned char tempb, tempg, tempr;
    int ta;
    I2D* srcImage;

    FAT_FILE *input = fat_fopen(pathName, "rb");
    if (!input) {
        printf("readImage: cannot open %s\n", pathName);
        return (void*)0;
    }

    fat_fread(&signature,          2, 1, input);
    fat_fread(&file_size,          4, 1, input);
    fat_fread(&reserved1,          2, 1, input);
    fat_fread(&reserved2,          2, 1, input);
    fat_fread(&loc_of_bitmap,      4, 1, input);
    fat_fread(&size_of_infoheader, 4, 1, input);
    fat_fread(&width,              4, 1, input);
    fat_fread(&height,             4, 1, input);
    fat_fread(&number_of_planes,   2, 1, input);
    fat_fread(&bits_per_pixel,     2, 1, input);
    fat_fread(&compression_method, 4, 1, input);
    fat_fread(&bytes_of_bitmap,    4, 1, input);
    fat_fread(&hori_reso,          4, 1, input);
    fat_fread(&vert_reso,          4, 1, input);
    fat_fread(&no_of_colors,       4, 1, input);
    fat_fread(&no_of_imp_colors,   4, 1, input);

    if (signature[0] != 'B' || signature[1] != 'M' ||
        height <= 0 || width <= 0 ||
        (bits_per_pixel != 8 && bits_per_pixel != 24)) {
        printf("readImage: bad BMP format in %s\n", pathName);
        fat_fclose(input);
        return (void*)0;
    }

    srcImage = iMallocHandle(height, width);
    fat_fseek(input, loc_of_bitmap, SEEK_SET);

    if (bits_per_pixel == 8) {
        for (nI = height - 1; nI >= 0; nI--)
            for (nJ = 0; nJ < width; nJ++) {
                fat_fread(&tempg, 1, 1, input);
                subsref(srcImage, nI, nJ) = (int)tempg;
            }
    } else {
        for (nI = height - 1; nI >= 0; nI--)
            for (nJ = 0; nJ < width; nJ++) {
                fat_fread(&tempb, 1, 1, input);
                fat_fread(&tempg, 1, 1, input);
                fat_fread(&tempr, 1, 1, input);
                ta = tempg;
                subsref(srcImage, nI, nJ) = (int)ta;
            }
    }

    fat_fclose(input);
    return srcImage;
}
