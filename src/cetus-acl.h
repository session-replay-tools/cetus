#ifndef CETUS_ACL_H
#define CETUS_ACL_H

#include "glib-ext.h"

struct cetus_acl_entry_t {
    char* username;
    char* host;
};

typedef struct cetus_acl_t {
    GList* whitelist;
    GList* blacklist;
} cetus_acl_t;

enum cetus_acl_category {
    ACL_WHITELIST,
    ACL_BLACKLIST,
};

cetus_acl_t* cetus_acl_new();

void cetus_acl_free(cetus_acl_t* acl);

gboolean cetus_acl_add_rule(cetus_acl_t* acl, enum cetus_acl_category cate,
                            const char* user, const char* host);

gboolean cetus_acl_add_rule_str(cetus_acl_t* acl, enum cetus_acl_category cate,
                            const char* rule);

int cetus_acl_delete_rule(cetus_acl_t* acl, enum cetus_acl_category cate,
                          const char* user, const char* host);

gboolean cetus_acl_delete_rule_str(cetus_acl_t* acl, enum cetus_acl_category cate,
                            const char* rule);

gboolean cetus_acl_verify(cetus_acl_t* acl, const char* user, const char* host);

int cetus_acl_add_rules(cetus_acl_t* acl, enum cetus_acl_category cate, const char* str);

#endif
