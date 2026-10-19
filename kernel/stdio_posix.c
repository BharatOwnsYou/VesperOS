#include "../include/stdio.h"
#include "../include/unistd.h"
#include "../include/errno.h"
#include "../include/stdlib.h"
#include "athenalibc.h"

typedef __builtin_va_list ac_va_list;
#define ac_va_start(ap, last) __builtin_va_start(ap, last)
#define ac_va_end(ap)         __builtin_va_end(ap)

static FILE std_streams[3] = {
    { STDIN_FILENO,  false, false },
    { STDOUT_FILENO, false, false },
    { STDERR_FILENO, false, false },
};
FILE *stdin  = &std_streams[0];
FILE *stdout = &std_streams[1];
FILE *stderr = &std_streams[2];

static int parse_mode_flags(const char *mode) {
    if (mode[0] == 'r') return O_RDONLY;
    if (mode[0] == 'w') return O_WRONLY | O_CREAT | O_TRUNC;
    if (mode[0] == 'a') return O_WRONLY | O_CREAT | O_APPEND;
    return O_RDONLY;
}

FILE *fopen(const char *path, const char *mode) {
    int fd = open(path, parse_mode_flags(mode));
    if (fd < 0) return NULL;
    FILE *f = (FILE*)malloc(sizeof(FILE));
    if (!f) { close(fd); errno = ENOMEM; return NULL; }
    f->fd = fd; f->eof = false; f->error = false;
    return f;
}

int fclose(FILE *f) {
    if (!f || f == stdin || f == stdout || f == stderr) return 0;
    close(f->fd);
    free(f);
    return 0;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *f) {
    if (!f || size == 0 || nmemb == 0) return 0;
    ssize_t n = read(f->fd, ptr, size * nmemb);
    if (n <= 0) { f->eof = true; return 0; }
    return (size_t)n / size;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *f) {
    if (!f || size == 0 || nmemb == 0) return 0;
    ssize_t n = write(f->fd, ptr, size * nmemb);
    if (n < 0) { f->error = true; return 0; }
    return (size_t)n / size;
}

int fputs(const char *s, FILE *f) {
    size_t len = k_strlen(s);
    ssize_t n = write(f->fd, s, len);
    return (n < 0) ? EOF : (int)n;
}

int fputc(int c, FILE *f) {
    char ch = (char)c;
    return (write(f->fd, &ch, 1) == 1) ? c : EOF;
}

int fgetc(FILE *f) {
    char ch;
    ssize_t n = read(f->fd, &ch, 1);
    if (n <= 0) { f->eof = true; return EOF; }
    return (int)(u8)ch;
}

char *fgets(char *s, int size, FILE *f) {
    int i = 0;
    while (i < size - 1) {
        int c = fgetc(f);
        if (c == EOF) { if (i == 0) return NULL; break; }
        s[i++] = (char)c;
        if (c == '\n') break;
    }
    s[i] = 0;
    return s;
}

int feof(FILE *f)   { return f ? (int)f->eof : 0; }
int ferror(FILE *f) { return f ? (int)f->error : 0; }

int fprintf(FILE *f, const char *fmt, ...) {
    char buf[512];
    ac_va_list ap;
    ac_va_start(ap, fmt);
    int len = k_vsnprintf(buf, sizeof(buf), fmt, ap);
    ac_va_end(ap);
    int to_write = (len < (int)sizeof(buf)) ? len : (int)sizeof(buf) - 1;
    if (to_write > 0) write(f->fd, buf, (size_t)to_write);
    return len;
}

int printf(const char *fmt, ...) {
    char buf[512];
    ac_va_list ap;
    ac_va_start(ap, fmt);
    int len = k_vsnprintf(buf, sizeof(buf), fmt, ap);
    ac_va_end(ap);
    int to_write = (len < (int)sizeof(buf)) ? len : (int)sizeof(buf) - 1;
    if (to_write > 0) write(STDOUT_FILENO, buf, (size_t)to_write);
    return len;
}
