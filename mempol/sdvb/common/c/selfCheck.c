/********************************
Author: Sravanthi Kota Venkata
Modified for bare-metal FAT16.
********************************/

#include "sdvbs_common.h"
#include "fat_file.h"

int selfCheck(I2D* in1, char* path, int tol)
{
    int r1, c1, ret = 1;
    int count = 0, i;
    char file[100];
    int *data = in1->data;

    r1 = in1->height;
    c1 = in1->width;

    int *buffer = (int*)malloc(sizeof(int) * r1 * c1);

    /* expected_C.txt is stored as EXPECT~1.TXT on the FAT image */
    bare_sprintf(file, "%s/expected_C.txt", path);
    FAT_FILE *fd = fat_fopen(file, "r");
    if (!fd) {
        printf("Error: Expected file not opened (%s)\n", file);
        return -1;
    }

    while (!fat_feof(fd) && count < r1 * c1) {
        if (fat_fscanf(fd, "%d", &buffer[count]) == 1)
            count++;
        else
            break;
    }
    fat_fclose(fd);

    if (count < r1 * c1) {
        printf("Checking error: dimensions mismatch. Got=%d expected=%d\n",
               count, r1 * c1);
        return -1;
    }

    for (i = 0; i < r1 * c1; i++) {
        int diff = abs(data[i]) - abs(buffer[i]);
        if (diff > tol || diff < -tol) {
            printf("Checking error: mismatch at element %d\n", i);
            printf("Expected=%d observed=%d\n", buffer[i], data[i]);
            return -1;
        }
    }

    printf("Verification\t\t- Successful\n");
    return ret;
}
