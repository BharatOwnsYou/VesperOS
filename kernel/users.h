#ifndef VESPEROS_USERS_H
#define VESPEROS_USERS_H
#include "../include/types.h"

/* scope: user accounts are BOOKKEEPING only. no privilege rings, no paging protection, no permission checks anywhere,
 * everything already runs with full hw access whichever "user" is active. "root" is just an account name,
 * switching users changes the prompt and nothing else. no passwords, hashing or auth, `su` switches on request.
 * don't treat this as a security boundary */

#define USERS_MAX 8
#define USER_NAME_MAX 24

typedef struct {
    char name[USER_NAME_MAX];
    bool is_root;
} user_t;

void users_init(void); /* seeds the "root" account */
int  user_add(const char *name);           /* 1 = added, 0 = already exists, -1 = table full */
int  user_switch(const char *name);        /* 1 = switched, -1 = no such user */
const char *user_current(void);
bool user_current_is_root(void);
void users_list(void (*printer)(const char *name, bool is_root, bool is_current));

#endif
