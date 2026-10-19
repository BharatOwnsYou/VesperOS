#include "../include/stdlib.h"

int atoi(const char *s) {
    int sign = 1;
    long r = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') { r = r * 10 + (*s - '0'); s++; }
    return (int)(r * sign);
}

long atol(const char *s) {
    int sign = 1;
    long r = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') { r = r * 10 + (*s - '0'); s++; }
    return r * sign;
}

int abs(int n) { return (n < 0) ? -n : n; }

static unsigned int rand_state = 1;

void srand(unsigned int seed) { rand_state = seed ? seed : 1; }

int rand(void) {
    rand_state ^= rand_state << 13;
    rand_state ^= rand_state >> 17;
    rand_state ^= rand_state << 5;
    return (int)(rand_state & 0x7FFFFFFF);
}

void exit(int code) {
    (void)code;
    __asm__ volatile ("cli; hlt");
    while (1) { }
}
