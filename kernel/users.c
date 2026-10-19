#include "users.h"
#include "string.h"

static user_t users[USERS_MAX];
static int user_count = 0;
static int current_idx = 0;

void users_init(void) {
    user_count = 0;
    k_strcpy(users[user_count].name, "root");
    users[user_count].is_root = true;
    user_count++;
    current_idx = 0;
}

static int find_user(const char *name) {
    for (int i = 0; i < user_count; i++)
        if (k_streq(users[i].name, name)) return i;
    return -1;
}

int user_add(const char *name) {
    if (find_user(name) >= 0) return 0;
    if (user_count >= USERS_MAX) return -1;
    k_strcpy(users[user_count].name, name);
    users[user_count].is_root = false;
    user_count++;
    return 1;
}

int user_switch(const char *name) {
    int idx = find_user(name);
    if (idx < 0) return -1;
    current_idx = idx;
    return 1;
}

const char *user_current(void) { return users[current_idx].name; }
bool user_current_is_root(void) { return users[current_idx].is_root; }

void users_list(void (*printer)(const char *name, bool is_root, bool is_current)) {
    for (int i = 0; i < user_count; i++)
        printer(users[i].name, users[i].is_root, i == current_idx);
}
