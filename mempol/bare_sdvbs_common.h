/*
 * Bare-metal SDVB common header.
 * Replaces FreeRTOS version when BARE_METAL is defined.
 * FILE I/O is handled by directly calling fat_fopen/fat_fread/etc. in
 * modified copies of readImage.c and selfCheck.c
 */
#ifndef _BARE_SDVBS_COMMON_
#define _BARE_SDVBS_COMMON_

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <math.h>

/* ---- malloc / free ---- */
void *bare_malloc(size_t size);
void  bare_free(void *ptr);
void *bare_calloc(size_t nmemb, size_t size);
void *bare_realloc(void *ptr, size_t size);

#undef  malloc
#undef  free
#undef  calloc
#undef  realloc
#define malloc(sz)       bare_malloc(sz)
#define free(p)          bare_free(p)
#define calloc(n,sz)     bare_calloc(n,sz)
#define realloc(p,sz)    bare_realloc(p,sz)

/* ---- sprintf ---- */
int bare_sprintf(char *buf, const char *fmt, ...);
#undef  sprintf
#define sprintf          bare_sprintf

/* ---- abs ---- */
#undef  abs
#define abs(x)           ((x) < 0 ? -(x) : (x))

/* ---- assert ---- */
#undef  assert
#define assert(x) do { if(!(x)){ printf("ASSERT FAIL\n"); while(1){} } } while(0)

/* ---- perror stub ---- */
#undef  perror
#define perror(s)        printf("ERROR: %s\n", (s))

/* ---- SDVB matrix types ---- */
typedef struct { int width; int height; int data[]; }          I2D;
typedef struct { int width; int height; unsigned int data[]; } UI2D;
typedef struct { int width; int height; float data[]; }        F2D;

#define subsref(a,i,j)   a->data[(i) * a->width + (j)]
#define asubsref(a,i)    a->data[i]
#define arrayref(a,i)    a[i]

/* Image I/O */
I2D*  readImage(const char* pathName);
F2D*  readFile(unsigned char* fileName);

/* Allocation */
I2D*  iMallocHandle(int rows, int cols);
F2D*  fMallocHandle(int rows, int cols);
UI2D* uiMallocHandle(int rows, int cols);
void  iFreeHandle(I2D* out);
void  fFreeHandle(F2D* out);
void  uiFreeHandle(UI2D* out);

/* Copy / set */
I2D* iSetArray(int rows, int cols, int val);
F2D* fSetArray(int rows, int cols, float val);
I2D* iDeepCopy(I2D* in);
F2D* fDeepCopy(F2D* in);
I2D* iDeepCopyRange(I2D* in, int startRow, int nr, int startCol, int nc);
F2D* fDeepCopyRange(F2D* in, int startRow, int nr, int startCol, int nc);
F2D* fiDeepCopy(I2D* in);
I2D* ifDeepCopy(F2D* in);

/* Matrix ops */
F2D* ffVertcat(F2D* a, F2D* b);  I2D* iVertcat(I2D* a, I2D* b);
F2D* fHorzcat(F2D* a, F2D* b);   I2D* iHorzcat(I2D* a, I2D* b);
F2D* horzcat(F2D* a, F2D* b, F2D* c);
F2D* fTranspose(F2D* a);          I2D* iTranspose(I2D* a);
F2D* fReshape(F2D* in, int r, int c); I2D* iReshape(I2D* in, int r, int c);
F2D* fDivide(F2D* a, float b);    F2D* fMdivide(F2D* a, F2D* b);
F2D* ffDivide(F2D* a, F2D* b);    F2D* ffTimes(F2D* a, float b);
F2D* fTimes(F2D* a, F2D* b);      I2D* iTimes(I2D* a, I2D* b);
F2D* fMtimes(F2D* a, F2D* b);     F2D* ifMtimes(I2D* a, F2D* b);
F2D* fMinus(F2D* a, F2D* b);      I2D* iMinus(I2D* a, I2D* b);
I2D* isMinus(I2D* a, int b);      F2D* fPlus(F2D* a, F2D* b);
I2D* isPlus(I2D* a, int b);

/* Filtering / transforms */
F2D* calcSobel_dX(F2D* in); F2D* calcSobel_dY(F2D* in);
F2D* ffConv2(F2D* a, F2D* b); F2D* fiConv2(I2D* a, F2D* b);
F2D* ffConv2_dY(F2D* a, F2D* b); F2D* ffiConv2(F2D* a, I2D* b);
I2D* iiConv2(I2D* a, I2D* b);
F2D* imageResize(F2D* in); F2D* imageBlur(I2D* in);

/* Support */
F2D* fFind3(F2D* in); F2D* fSum2(F2D* in, int dir); F2D* fSum(F2D* in);
I2D* iSort(I2D* in, int dim); F2D* fSort(F2D* in, int dim);
I2D* iSortIndices(I2D* in, int dim); I2D* fSortIndices(F2D* in, int dim);
F2D* randnWrapper(int m, int n); F2D* randWrapper(int m, int n);

/* Checking */
int  selfCheck(I2D* in1, char* path, int tol);
int  fSelfCheck(F2D* in1, char* path, float tol);
void writeMatrix(I2D* input, char* inpath);
void fWriteMatrix(F2D* input, char* inpath);

/* Timing */
unsigned int* photonStartTiming(void);
unsigned int* photonEndTiming(void);
unsigned int* photonReportTiming(unsigned int* start, unsigned int* end);
void          photonPrintTiming(unsigned int* elapsed);

#endif /* _BARE_SDVBS_COMMON_ */
