/********************************
Author: Sravanthi Kota Venkata
********************************/

#include "sdvbs_common.h"
#include "fat_file.h"

int fSelfCheck(F2D* in1, char* path, float tol)
{
    int r1, c1, ret=1;
    float *buffer;
    int count=0, i;
    char file[256];

    r1 = in1->height;
    c1 = in1->width;

    buffer = (float*)malloc(sizeof(float)*r1*c1);

    bare_sprintf(file, "%s/expected_C.txt", path);
    FAT_FILE *fd = fat_fopen(file, "r");

    if (!fd)
    {
        printf("Error: Expected file not opened %s\n", file);
        return -1;
    }

    while (!fat_feof(fd) && count < r1*c1) {
        if (fat_fscanf(fd, "%f", &buffer[count]) == 1)
            count++;
        else
            break;
    }
    fat_fclose(fd);

    if (count != (r1*c1))
    {
        printf("Checking error: dimensions mismatch. Expected = %d, Observed = %d \n", count, (r1*c1));
        return -1;
    }

    for (i=0; i<r1*c1; i++)
    {
        float inVal = asubsref(in1,i);
        if ((inVal-buffer[i])>tol || (buffer[i]-inVal)>tol)
        {
            printf("Mismatch %d: (%f, %f)\n", i, buffer[i], inVal);
            return -1;
        }
    }

    printf("Verification\t\t- Successful\n");
    free(buffer);
    return ret;
}


