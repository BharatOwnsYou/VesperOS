#ifndef VESPEROS_STRING_H
#define VESPEROS_STRING_H
#include "../include/types.h"

size_t k_strlen(const char *s);
int    k_strcmp(const char *a, const char *b);
int    k_strncmp(const char *a, const char *b, size_t n);
char  *k_strcpy(char *dst, const char *src);
char  *k_strcat(char *dst, const char *src);
void  *k_memset(void *dst, int val, size_t n);
void  *k_memcpy(void *dst, const void *src, size_t n);
int    k_streq(const char *a, const char *b);
void   k_itoa(u32 val, char *buf, int base);
int    k_split_ws(char *line, char *argv[], int max_args);

#endif
