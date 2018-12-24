#include "cetus-acl.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

cetus_acl_t* cetus_acl_new()
{
    cetus_acl_t* acl = g_new0(cetus_acl_t, 1);
    return acl;
}

static void acl_entry_free(gpointer entry)
{
    struct cetus_acl_entry_t* e = entry;
    g_free(e->username);
    g_free(e->host);
    g_free(e);
}

void cetus_acl_free(cetus_acl_t* acl)
{
    g_list_free_full(acl->whitelist, acl_entry_free);
    g_free(acl);
}

static GList* acl_add_to_list(GList* entries, const char* user,
                                    const char* host, gboolean* ok)
{
    GList* l;
    for (l = entries; l; l = l->next) {
        struct cetus_acl_entry_t* entry = l->data;
        if (strcmp(user, entry->username)==0
            && strcmp(host, entry->host)==0) {
            g_message(G_STRLOC "adding duplicate entry to acl, neglected");
            *ok = FALSE;
            return entries;
        }
    }
    struct cetus_acl_entry_t* ent = g_new0(struct cetus_acl_entry_t, 1);
    ent->username = g_strdup(user);
    ent->host = g_strdup(host);
    *ok = TRUE;
    return g_list_append(entries, ent);
}

#define IS_WILDCARD(X) (X[0] == '*' || X[0] == '%')

/**
 * user can be wildcard
 */
static GList* acl_delete_from_list(GList* entries, const char* user,
                                         const char* host, int* count)
{
    GList* l = entries;
    while (l) {
        struct cetus_acl_entry_t* entry = l->data;
        GList* next = g_list_next(l);
        if (strcmp(entry->host, host)==0
            && (IS_WILDCARD(user) || strcmp(user, entry->username)==0)) {
            acl_entry_free(entry);
            entries = g_list_delete_link(entries, l);
            *count += 1;
        }
        l = next;
    }
    return entries;
}

static gboolean is_ip_address(const gchar *ip)
{
    gchar *host_ip = (gchar *)ip;
    gchar *wildcard_pos = g_strstr_len(host_ip, strlen(host_ip), "*");
    gint cmp_size = strlen(host_ip);
    if(wildcard_pos) {
        cmp_size = (gint)(wildcard_pos - host_ip);
    }
    gint i = 0;
    gchar *start = host_ip;
    gchar *end = start;
    gint dot_num = 0;
    while(i<cmp_size) {
        if(!isdigit(*end) && *end != '.') {
            return FALSE;
        }
        if(*end == '.') {
            dot_num ++;
            if(dot_num > 3) {
                return FALSE;
            }
            gint len = (gint)(end - start);
            gchar num[4] = {""};
            if(len > 0) {
                memcpy(num, start, len);
            }
            if(atoi(num) > 255 || atoi(num) < 0) {
                return FALSE;
            }
            start = end + 1;
        }
        i++;
        end++;
    }
    return TRUE;
}

static gboolean acl_ip_contains(const gchar *cip, const gchar *hip)
{
    gchar *client_ip = (gchar *)cip;
    gchar *host_ip = (gchar *)hip;
    if(g_strcmp0(host_ip, "*") == 0) {
        return TRUE;
    }
    gchar *wildcard_pos = g_strstr_len(host_ip, strlen(host_ip), "*");
    gint cmp_size = 0;
    if(wildcard_pos) {
        cmp_size = (gint)(wildcard_pos - host_ip);
    } else {
        cmp_size = strlen(host_ip);
    }
    if(cmp_size > 0) {
        gboolean ret = (g_ascii_strncasecmp(client_ip, host_ip, cmp_size) == 0);
        return ret;
    }
    return FALSE;
}

static gboolean acl_list_contains(GList* entries, const char* user, const char* host)
{
    GList* l;
    for (l = entries; l; l = l->next) {
        struct cetus_acl_entry_t* entry = l->data;
        if (acl_ip_contains(host, entry->host)
            && (strcmp(entry->username, user)==0 || IS_WILDCARD(entry->username))) {
            return TRUE;
        }
    }
    return FALSE;
}

gboolean cetus_acl_add_rule(cetus_acl_t* acl, enum cetus_acl_category cate,
                            const char* user, const char* host)
{
    gboolean ok = FALSE;
    if (cate == ACL_WHITELIST) {
        acl->whitelist = acl_add_to_list(acl->whitelist, user, host, &ok);
    } else {
        acl->blacklist = acl_add_to_list(acl->blacklist, user, host, &ok);
    }
    return ok;
}

gboolean cetus_acl_add_rule_str(cetus_acl_t* acl, enum cetus_acl_category cate,
                                const char* the_rule)
{
    char* rule = g_strdup(the_rule);
    char* user = "*";
    char* host = strchr(rule, '@');
    if (host) { /* user@xx.xx.xx.xx style */
        *host = '\0';
        host += 1;
        user = rule;
    } else {  /* xx.xx.xx.xx host only style */
        host = rule;
    }
    if (!is_ip_address(host)) {
        g_message(G_STRLOC "host name not ip address");
        g_free(rule);
        return FALSE;
    }
    gboolean ok = cetus_acl_add_rule(acl, cate, user, host);
    g_free(rule);
    return ok;
}


int cetus_acl_delete_rule(cetus_acl_t* acl, enum cetus_acl_category cate,
                          const char* user, const char* host)
{
    int count = 0;
    if (cate == ACL_WHITELIST) {
        acl->whitelist = acl_delete_from_list(acl->whitelist, user, host, &count);
    } else {
        acl->blacklist = acl_delete_from_list(acl->blacklist, user, host, &count);
    }
    return count;
}

gboolean cetus_acl_delete_rule_str(cetus_acl_t* acl, enum cetus_acl_category cate,
                                const char* the_rule)
{
    char* rule = g_strdup(the_rule);
    char* user = "*";
    char* host = strchr(rule, '@');
    if (host) { /* user@xx.xx.xx.xx style */
        *host = '\0';
        host += 1;
        user = rule;
    } else {  /* xx.xx.xx.xx host only style */
        host = rule;
    }
    if (!is_ip_address(host)) {
        g_message(G_STRLOC "host name not ip address");
        g_free(rule);
        return 0;
    }
    int ok = cetus_acl_delete_rule(acl, cate, user, host);
    g_free(rule);
    return ok;
}

gboolean cetus_acl_verify(cetus_acl_t* acl, const char* user, const char* host)
{
    if (acl->whitelist == NULL && acl->blacklist == NULL) {
        return TRUE; /* acl is empty, every ip is ok to pass */
    }
    if (acl->whitelist && acl_list_contains(acl->whitelist, user, host)) {
        return TRUE;
    }
    if (acl->blacklist && acl_list_contains(acl->blacklist, user, host)) {
        return FALSE;
    }
    if (acl->whitelist && !acl->blacklist)
        return FALSE;
    else
        return TRUE;
}

int cetus_acl_add_rules(cetus_acl_t* acl, enum cetus_acl_category cate, const char* rules)
{
    int count = 0;
    if (rules) {
        char **ip_arr = g_strsplit(rules, ",", -1);
        int i;
        for (i = 0; ip_arr[i]; i++) {
            gboolean ok = cetus_acl_add_rule_str(acl, cate, ip_arr[i]);
            if (ok) {
                count += 1;
            }
        }
        g_strfreev(ip_arr);
    }
    return count;
}

