#ifndef ATHENA_STDIO_H
#define ATHENA_STDIO_H
#include "types.h"

typedef struct { int fd; bool eof; bool error; } FILE;

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

#define EOF (-1)

FILE  *fopen(const char *path, const char *mode);
int    fclose(FILE *f);
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *f);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *f);
int    fputs(const char *s, FILE *f);
char  *fgets(char *s, int size, FILE *f);
int    fputc(int c, FILE *f);
int    fgetc(FILE *f);
int    feof(FILE *f);
int    ferror(FILE *f);
int    fprintf(FILE *f, const char *fmt, ...);
int    printf(const char *fmt, ...);

#endif
