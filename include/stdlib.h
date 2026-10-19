#ifndef ATHENA_STDLIB_H
#define ATHENA_STDLIB_H
#include "types.h"

void *malloc(size_t size);
void  free(void *ptr);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);

int   atoi(const char *s);
long  atol(const char *s);
int   abs(int n);

void  srand(unsigned int seed);
int   rand(void);

void  exit(int code) __attribute__((noreturn));

#endif
