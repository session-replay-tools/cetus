/* $%BEGINLICENSE%$
 Copyright (c) 2007, 2012, Oracle and/or its affiliates. All rights reserved.

 This program is free software; you can redistribute it and/or
 modify it under the terms of the GNU General Public License as
 published by the Free Software Foundation; version 2 of the
 License.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 02110-1301  USA

 $%ENDLICENSE%$ */

#include "sharding-config.h"

#include <stdlib.h>
#include <string.h>
#include "glib-ext.h"
#include "sys-pedantic.h"
#include "cJSON.h"
#include "chassis-timings.h"

static GList *shard_conf_vdbs = NULL;

static GHashTable *shard_conf_tables = NULL; /* mapping< schema_table_t*, sharding_table_t* > */

static GList *shard_conf_single_tables = NULL;

static GList *shard_conf_all_groups = NULL;

static GString *parition_super_group = NULL;

struct schema_table_t {
    const char *schema;
    const char *table;
};

struct schema_table_t*
schema_table_new(const char* s, const char* t)
{
    struct schema_table_t *st = g_new0(struct schema_table_t, 1);
    st->schema = g_strdup(s);
    st->table = g_strdup(t);
    return st;
}

void schema_table_free(struct schema_table_t *st)
{
    g_free((char*)st->schema);
    g_free((char*)st->table);
    g_free(st);
}

/* djb hash, same as g_str_hash */
static guint
schema_table_hash(gconstpointer v)
{
    const struct schema_table_t *st = v;
    const signed char *p;
    guint32 h = 5381;

    for (p = st->schema; *p != '\0'; p++)
        h = (h << 5) + h + *p;
    h = (h << 5) + h + '.';
    for (p = st->table; *p != '\0'; p++)
        h = (h << 5) + h + *p;
    return h;
}

gboolean
schema_table_equal(gconstpointer v1,
             gconstpointer v2)
{
  const struct schema_table_t *st1 = v1;
  const struct schema_table_t *st2 = v2;
  return strcmp(st1->schema, st2->schema) == 0
      && strcmp(st1->table, st2->table) == 0;
}

gboolean
shard_conf_table_cmp(gpointer key, gpointer value, gpointer user_data)
{
    const struct schema_table_t *st1 = key;
    const struct schema_table_t *st2 = user_data;
    return (strcasecmp(st1->schema, st2->schema) == 0) &&
               (strcasecmp(st1->table, st2->table) == 0);
}

static sharding_table_t *
sharding_tables_get(const char *schema, const char *table)
{
    struct schema_table_t st = {schema, table};
    gpointer tinfo = g_hash_table_find(shard_conf_tables, shard_conf_table_cmp, &st);
    return tinfo;
}

gboolean sharding_tables_add(sharding_table_t* table)
{
    if (sharding_tables_get(table->schema->str, table->name->str)) {
        return FALSE; /* !! DON'T REPLACE ONLINE */
    }
    struct schema_table_t *st = schema_table_new(table->schema->str, table->name->str);
    g_hash_table_insert(shard_conf_tables, st, table);
    return TRUE;
}

static sharding_vdb_t *
shard_vdbs_get_by_id(GList *vdbs, int id)
{
    GList *l = vdbs;
    for (; l != NULL; l = l->next) {
        sharding_vdb_t *vdb = l->data;
        if (vdb->id == id) {
            return vdb;
        }
    }
    return NULL;
}

static sharding_vdb_t *
sharding_vdbs_get_by_table(const char *schema, const char *table)
{
    sharding_table_t *tinfo = sharding_tables_get(schema, table);
    if (tinfo)
        return tinfo->vdb_ref;
    else
        return NULL;
}

void
sharding_table_free(gpointer q)
{
    sharding_table_t *info = q;
    if (NULL != info->schema)
        g_string_free(info->schema, TRUE);
    if (NULL != info->name)
        g_string_free(info->name, TRUE);
    if (NULL != info->pkey)
        g_string_free(info->pkey, TRUE);
    g_free(info);
}

sharding_partition_t *sharding_partition_new(const char *group, const sharding_vdb_t *vdb)
{
    sharding_partition_t *p = g_new0(sharding_partition_t, 1);
    p->method = vdb->method;
    p->key_type = vdb->key_type;
    p->hash_count = vdb->logic_shard_num;
    p->group_name = g_string_new(group);
    return p;
}

gboolean
sharding_partition_contain_hash(sharding_partition_t *partition, int val)
{
    g_assert(partition->method == SHARD_METHOD_HASH);
    if (val >= partition->hash_count)
        return FALSE;
    return TestBit(partition->hash_set, val);
}

void sharding_partition_free(sharding_partition_t *p)
{
    if (p->method == SHARD_METHOD_RANGE) {
        if (p->key_type == SHARD_DATA_TYPE_STR) {
            g_free(p->value);
            g_free(p->low_value);
        }
    }
    if (p->group_name) {
        g_string_free(p->group_name, TRUE);
    }
    g_free(p);
}

void sharding_partition_to_string(sharding_partition_t* p, GString* repr)
{
    g_string_truncate(repr, 0);
    if (p->method == SHARD_METHOD_RANGE) {
        if (p->key_type == SHARD_DATA_TYPE_STR) {
            g_string_printf(repr, "(%s, %s]->%s", (char*)p->low_value, (char*)p->value,
                            p->group_name->str);
        } else {
            g_string_printf(repr, "(%ld, %ld]->%s",(int64_t)p->low_value, (int64_t)p->value,
                            p->group_name->str);
        }
    } else {
        int i = 0;
        g_string_append_c(repr, '[');
        for (i = 0; i < p->hash_count; ++i) {
            if (TestBit(p->hash_set, i)) {
                g_string_append_printf(repr, "%d,", i);
            }
        }
        g_string_truncate(repr, repr->len-1);
        g_string_append_printf(repr, "]->%s", p->group_name->str);
    }
}

void sharding_vdb_partitions_to_string(sharding_vdb_t* vdb, GString* repr)
{
    g_string_truncate(repr, 0);
    GString* str = g_string_new(0);
    int i = 0;
    for (i=0; i < vdb->partitions->len; ++i) {
        sharding_partition_t* part = g_ptr_array_index(vdb->partitions, i);
        sharding_partition_to_string(part, str);
        g_string_append(repr, str->str);
        if (i != vdb->partitions->len - 1)
            g_string_append(repr, "; ");
    }
    g_string_free(str, TRUE);
}

sharding_vdb_t *sharding_vdb_new()
{
    sharding_vdb_t *vdb = g_new0(struct sharding_vdb_t, 1);
    vdb->partitions = g_ptr_array_new();
    return vdb;
}

void sharding_vdb_free(sharding_vdb_t *vdb)
{
    if (!vdb) {
        return;
    }

    int i;
    for (i = 0; i < vdb->partitions->len; i++) {
        sharding_partition_t *item = g_ptr_array_index(vdb->partitions, i);
        sharding_partition_free(item);
    }
    g_ptr_array_free(vdb->partitions, TRUE);
    g_free(vdb);
}

gboolean sharding_vdb_is_valid(int is_partition_mode, sharding_vdb_t *vdb, int num_groups)
{
    if (!is_partition_mode) {
        if (vdb->partitions->len != num_groups) {
            g_critical("vdb-%d partition count not equal to number of groups, vdb partition len:%d, groups:%d",
                    vdb->id, vdb->partitions->len, num_groups);
            return FALSE;
        }
    }
    if (vdb->method == SHARD_METHOD_HASH) {
        if (vdb->logic_shard_num <= 0 || vdb->logic_shard_num > MAX_HASH_VALUE_COUNT) {
            return FALSE;
        }
        /* make sure all hash values fall into a partition */
        char *value_set = g_malloc0(vdb->logic_shard_num);
        int i, j;
        /* collect hash values of all partitions */
        for (i = 0; i < vdb->partitions->len; i++) {
            sharding_partition_t *part = g_ptr_array_index(vdb->partitions, i);
            for (j = 0; j < vdb->logic_shard_num; ++j) {
                if (TestBit(part->hash_set, j))
                    value_set[j] = 1;
            }
        }
        /* we expect that value_set all filled with 1 */
        for (i = 0; i < vdb->logic_shard_num; ++i) {
            if (value_set[i] == 0) {
                g_free(value_set);
                return FALSE;
            }
        }
        g_free(value_set);
    }
    return TRUE;
}

GPtrArray *
shard_conf_get_all_groups(GPtrArray *all_groups)
{
    GList *l = shard_conf_all_groups;
    for (; l; l = l->next) {
        GString* gp = l->data;
        g_ptr_array_add(all_groups, gp);
    }
    return all_groups;
}

void
shard_conf_find_groups(GPtrArray *groups, const char *pattern)
{
    if (strcasecmp(pattern, "all") == 0 || strcasecmp(pattern, "*") == 0) {
        shard_conf_get_all_groups(groups);
        return;
    }
    GList *l = shard_conf_all_groups;
    for (; l; l = l->next) {
        GString *gp = l->data;
        if (strcmp(gp->str, pattern) == 0) {
            g_ptr_array_add(groups, gp);
            return;
        }
    }
}

GPtrArray *
shard_conf_get_any_group(GPtrArray *any_group, const char *db, const char *table)
{
    if (!db || !table) {
        g_warning(G_STRLOC " db or table name is NULL");
        return NULL;
    }
    sharding_vdb_t *vdb = sharding_vdbs_get_by_table(db, table);
    if (vdb == NULL) {
        return NULL;
    }

    GPtrArray *partitions = vdb->partitions;
    int i = rand() % partitions->len;

    sharding_partition_t *part = g_ptr_array_index(partitions, i);
    g_ptr_array_add(any_group, part->group_name);

    return any_group;
}

GPtrArray *
shard_conf_get_table_groups(GPtrArray *visited_groups, const char *db, const char *table)
{
    if (!db || !table) {
        g_warning(G_STRLOC " schema or table name is NULL");
        return NULL;
    }

    sharding_vdb_t *vdb = sharding_vdbs_get_by_table(db, table);
    if (vdb == NULL) {
        return NULL;
    }

    GPtrArray *partitions = vdb->partitions;
    int i, j;
    for (i = 0; i < partitions->len; i++) {
        sharding_partition_t *partition = g_ptr_array_index(partitions, i);
        int already_added = 0;
        for (j = 0; j < visited_groups->len; j++) {
            GString *added_group_name = visited_groups->pdata[j];
            if (g_string_equal(partition->group_name, added_group_name)) {
                already_added = 1;
                break;
            }
        }
        if (already_added == 0) {
            g_ptr_array_add(visited_groups, partition->group_name);
        }
    }
    return visited_groups;
}

/**
 * get array of groups pointers of a table
 * no more duplication check cause one group correspond multiple range value
 */
GPtrArray *
shard_conf_table_partitions(GPtrArray *partitions, const char *db, const char *table)
{
    if (!db || !table) {
        g_warning(G_STRLOC " db or table name is NULL");
        return NULL;
    }

    sharding_vdb_t *vdb = sharding_vdbs_get_by_table(db, table);
    if (!vdb) {
        return NULL;
    }
    GPtrArray *all_partitions = vdb->partitions;
    int i;
    for (i = 0; i < all_partitions->len; i++) {
        sharding_partition_t *part = g_ptr_array_index(all_partitions, i);
        g_ptr_array_add(partitions, part);
    }
    return partitions;
}

sharding_table_t *
shard_conf_get_info(const char *db_name, const char *table)
{
    return sharding_tables_get(db_name, table);
}

gboolean
shard_conf_is_shard_table(const char *db, const char *table)
{
    return sharding_tables_get(db, table) ? TRUE : FALSE;
}

GPtrArray *
shard_conf_get_fixed_group(int partition, GPtrArray *groups, guint64 fixture)
{
    if (partition) {
        g_ptr_array_add(groups, parition_super_group);
        return groups;
    } else {
        int len = g_list_length(shard_conf_all_groups);
        if (len == 0) {
            return groups;
        }
        int index = fixture % len;
        GString *grp = g_list_nth_data(shard_conf_all_groups, index);
        g_ptr_array_add(groups, grp);
        return groups;
    }
}

void
single_table_free(struct single_table_t *t)
{
    if (t) {
        g_string_free(t->name, TRUE);
        g_string_free(t->schema, TRUE);
        g_string_free(t->group, TRUE);
        g_free(t);
    }
}

static void
shard_conf_set_vdb_list(GList *vdbs)
{
    g_list_free_full(shard_conf_vdbs, (GDestroyNotify) sharding_vdb_free);
    shard_conf_vdbs = vdbs;
}

GList* shard_conf_get_vdb_list()
{
    return shard_conf_vdbs;
}

static void
shard_conf_set_tables(GHashTable *tables)
{
    if (shard_conf_tables)
        g_hash_table_destroy(shard_conf_tables);
    shard_conf_tables = tables;
}

static gboolean
sharding_table_equal(gconstpointer v1, gconstpointer v2)
{
  const sharding_table_t *st1 = v1;
  const sharding_table_t *st2 = v2;
  int a = strcasecmp(st1->schema->str, st2->schema->str);
  if (a == 0) {
      return strcasecmp(st1->name->str, st2->name->str);
  } else {
      return a;
  }
}

GList* shard_conf_get_tables()
{
    GList* tables = g_hash_table_get_values(shard_conf_tables);
    tables = g_list_sort(tables, sharding_table_equal);
    return tables;
}

GList* shard_conf_get_single_tables()
{
    return shard_conf_single_tables;
}

GString *partition_get_super_group()
{
    return parition_super_group; 
}

static void
shard_conf_set_single_tables(GList *tables)
{
    g_list_free_full(shard_conf_single_tables, (GDestroyNotify) single_table_free);
    shard_conf_single_tables = tables;
}

static void
shard_conf_set_all_groups(GList *groups)
{
    g_list_free_full(shard_conf_all_groups, g_string_true_free);
    shard_conf_all_groups = groups;
}

static GList *
string_list_distinct_append(GList *strlist, const GString *str)
{
    GList *l = strlist;
    for (; l; l = l->next) {
        GString* s = l->data;
        if (g_string_equal(s, str))
            return strlist;
    }
    return g_list_append(strlist, g_string_new(str->str));
}
/**
 * setup index & validate configurations
 */
static gboolean
shard_conf_try_setup(int is_partition_mode, GList *vdbs, GList *tables, GList *single_tables, int num_groups)
{
    if (!vdbs || !tables) {
        g_critical("empty vdb/table list");
        return FALSE;
    }
    GList *l = vdbs;
    for (; l != NULL; l = l->next) {
        sharding_vdb_t *vdb = l->data;
        if (!sharding_vdb_is_valid(is_partition_mode, vdb, num_groups)) {
            g_warning("invalid vdb config");
            return FALSE;
        }
    }
    GList *all_groups = NULL;
    GHashTable *table_dict = g_hash_table_new_full(schema_table_hash, schema_table_equal,
                                                   (GDestroyNotify)schema_table_free,
                                                   sharding_table_free);
    l = tables;
    for (; l != NULL; l = l->next) {
        sharding_table_t *table = l->data;
        sharding_vdb_t *vdb = shard_vdbs_get_by_id(vdbs, table->vdb_id);

        /* Fill table with vdb info */
        if (vdb) {
            table->vdb_ref = vdb;
            table->shard_key_type = vdb->key_type;
        } else {
            g_critical(G_STRLOC " table:%s VDB ID cannot be found: %d",
                       table->name->str, table->vdb_id);
            g_hash_table_destroy(table_dict);
            return FALSE;
        }
        int i = 0;
        for (i = 0; i < vdb->partitions->len; ++i) {
            sharding_partition_t *part = g_ptr_array_index(vdb->partitions, i);
            all_groups = string_list_distinct_append(all_groups, part->group_name);
        }
        struct schema_table_t *st = schema_table_new(table->schema->str, table->name->str);
        g_hash_table_insert(table_dict, st, table);
    }
    /* `tables` has been transferred to `table_dict`, free it */
    g_list_free(tables);

    shard_conf_set_vdb_list(vdbs);
    shard_conf_set_tables(table_dict);
    shard_conf_set_single_tables(single_tables);
    shard_conf_set_all_groups(all_groups);

    parition_super_group = g_string_new(PARTITION_SUPER_GROUP);

    return TRUE;
}

void
shard_conf_destroy(void)
{
    if (shard_conf_vdbs) {
        g_list_free_full(shard_conf_vdbs, (GDestroyNotify) sharding_vdb_free);
    }
    if (parition_super_group) {
        g_string_free(parition_super_group, TRUE);
    }
    if (shard_conf_tables) {
        g_hash_table_destroy(shard_conf_tables);
    }
    if (shard_conf_single_tables) {
        g_list_free_full(shard_conf_single_tables, (GDestroyNotify) single_table_free);
    }
    if (shard_conf_all_groups) {
        g_list_free_full(shard_conf_all_groups, g_string_true_free);
    }
}

static GHashTable *load_shard_from_json(gchar *json_str);

gboolean
shard_conf_load(int partition_mode, char *json_str, int num_groups)
{
    GHashTable *ht = load_shard_from_json(json_str);
    if (!ht)
        return FALSE;

    GList *tables = g_hash_table_lookup(ht, "table_list");
    GList *vdbs = g_hash_table_lookup(ht, "vdb_list");
    GList *single_tables = g_hash_table_lookup(ht, "single_tables");
    gboolean success = shard_conf_try_setup(partition_mode, vdbs, tables, single_tables, num_groups);
    if (!success) {
        g_list_free_full(vdbs, (GDestroyNotify) sharding_vdb_free);
        g_list_free_full(tables, (GDestroyNotify) sharding_table_free);
    }
    g_hash_table_destroy(ht);
    return success;
}

static struct single_table_t *
shard_conf_get_single_table(const char *db, const char *name)
{
    GList *l = shard_conf_single_tables;
    for (; l; l = l->next) {
        struct single_table_t *t = l->data;
        if (strcasecmp(t->name->str, name) == 0 && strcasecmp(t->schema->str, db) == 0) {
            return t;
        }
    }
    return NULL;
}

gboolean
shard_conf_is_single_table(int partition_mode, const char *db, const char *name)
{
    if (!partition_mode) {
        struct single_table_t *t = shard_conf_get_single_table(db, name);
        return t != NULL;
    } else {
        return FALSE;
    }
}

static gboolean
shard_conf_group_contains(GPtrArray *groups, GString *match)
{
    int i;
    for (i = 0; i < groups->len; ++i) {
        GString *gp = g_ptr_array_index(groups, i);
        if (g_string_equal(gp, match)) {
            return TRUE;
        }
    }
    return FALSE;
}

GPtrArray *
shard_conf_get_single_table_distinct_group(GPtrArray *groups, const char *db, const char *name)
{
    struct single_table_t *t = shard_conf_get_single_table(db, name);
    if (t && !shard_conf_group_contains(groups, t->group)) {
        g_ptr_array_add(groups, t->group);
    }
    return groups;
}

struct code_map_t {
    const char *name;
    int code;
} key_type_map[] = {
    {"INT", SHARD_DATA_TYPE_INT},
    {"CHAR", SHARD_DATA_TYPE_STR},
    {"STR", SHARD_DATA_TYPE_STR},
    {"STRING", SHARD_DATA_TYPE_STR},
    {"DATE", SHARD_DATA_TYPE_DATE},
    {"DATETIME", SHARD_DATA_TYPE_DATETIME},
};

int sharding_key_type(const char *str)
{
    int i;
    for (i = 0; i < sizeof(key_type_map) / sizeof(*key_type_map); ++i) {
        if (strcasecmp(key_type_map[i].name, str) == 0)
            return key_type_map[i].code;
    }
    g_critical("Wrong sharding setting <key_type:%s>", str);
    return -1;
}

const char* sharding_key_type_str(int type)
{
    int i;
    for (i = 0; i < sizeof(key_type_map) / sizeof(*key_type_map); ++i) {
        if (key_type_map[i].code == type)
            return key_type_map[i].name;
    }
    return "error";
}

static int
sharding_method(const char *str)
{
    if (strcasecmp(str, "hash") == 0) {
        return SHARD_METHOD_HASH;
    } else if (strcasecmp(str, "range") == 0) {
        return SHARD_METHOD_RANGE;
    } else {
        return SHARD_METHOD_UNKNOWN;
    }
}

/*
 * Parse partitions from JSON to Hash Table
 * exmpale:
 *   {"data1":[0], "data2":[1], "data3":[2], "data4":[3]}
 */
static void
parse_partitions(cJSON *root, const sharding_vdb_t *vdb, GPtrArray *partitions /* out */ )
{
    cJSON *cur = root->child;
    sharding_partition_t *item;
    for (; cur; cur = cur->next) {  /* { "groupA":xx, "groupB":xx, "groupC":xx} */
        /* null means unlimited */
        switch (cur->type) {
        case cJSON_NULL:       /* range: null */
            item = sharding_partition_new(cur->string, vdb);
            if (vdb->key_type == SHARD_DATA_TYPE_STR) {
                item->value = NULL;
            } else {
                item->value = (void *)(uint64_t)INT_MAX;
            }
            g_ptr_array_add(partitions, item);
            break;
        case cJSON_Number:     /* range > 123 */
            item = sharding_partition_new(cur->string, vdb);
            item->value = (void *)(uint64_t)cur->valuedouble;
            g_ptr_array_add(partitions, item);
            break;
        case cJSON_String:     /* range > "str" */
            item = sharding_partition_new(cur->string, vdb);
            if (vdb->key_type == SHARD_DATA_TYPE_DATETIME
                || vdb->key_type == SHARD_DATA_TYPE_DATE) {
                gboolean ok;
                int epoch = chassis_epoch_from_string(cur->valuestring, &ok);
                if (ok)
                    item->value = (void *)(uint64_t)epoch;
                else
                    g_warning("Wrong sharding setting <datetime format:%s>", cur->valuestring);
            } else {
                item->value = g_strdup(cur->valuestring);
            }
            g_ptr_array_add(partitions, item);
            break;
        case cJSON_Array:{
            cJSON *elem = cur->child;
            if (cJSON_Number == elem->type) {   /* hash in [0,3,5] */
                item = sharding_partition_new(cur->string, vdb);
                for (; elem; elem = elem->next) {
                    if (elem->type != cJSON_Number) {
                        g_critical(G_STRLOC "array has different type");
                        continue;
                    }
                    if (elem->valueint >= 0 && elem->valueint < vdb->logic_shard_num) {
                        SetBit(item->hash_set, elem->valueint);
                    } else {
                        g_critical(G_STRLOC "hash value exceeds logic_shard_num");
                    }
                }
                g_ptr_array_add(partitions, item);
            } else if (cJSON_String == elem->type) {    /* TODO: range in [0, 100, 200] */
                while (elem != NULL) {
                    item = sharding_partition_new(cur->string, vdb);
                    if (vdb->key_type == SHARD_DATA_TYPE_DATETIME
                        || vdb->key_type == SHARD_DATA_TYPE_DATE) {
                        gboolean ok;
                        int epoch = chassis_epoch_from_string(elem->valuestring, &ok);
                        if (ok)
                            item->value = (void *)(uint64_t)epoch;
                        else
                            g_warning("Wrong sharding setting <datetime format:%s>", elem->valuestring);
                    } else {
                        item->value = g_strdup(elem->valuestring);
                    }
                    g_ptr_array_add(partitions, item);
                    elem = elem->next;
                }
            }
            break;
        }
        default:
            g_warning("JSON TYPE: %d, GROUP: %s", cur->type, cur->string);
        }                       /* end switch */
    }
}

static gint
cmp_shard_range_groups_int(gconstpointer a, gconstpointer b)
{
    sharding_partition_t *item1 = *(sharding_partition_t **)a;
    sharding_partition_t *item2 = *(sharding_partition_t **)b;
    int64_t n1 = (int64_t) item1->value;
    int64_t n2 = (int64_t) item2->value;
    if (n1 > n2) {
        return 1;
    } else if (n1 == n2) {
        return 0;
    } else {
        return -1;
    }
}

static gint
cmp_shard_range_groups_str(gconstpointer a, gconstpointer b)
{
    sharding_partition_t *item1 = *(sharding_partition_t **)a;
    sharding_partition_t *item2 = *(sharding_partition_t **)b;
    const char *s1 = item1->value;
    const char *s2 = item2->value;
    if (s1 == NULL) {
        return 1;
    } else if (s2 == NULL) {
        return -1;
    }
    return strcmp(s1, s2);
}

static void setup_partitions(GPtrArray *partitions, sharding_vdb_t *vdb)
{
    if (vdb->method == SHARD_METHOD_RANGE) {
        /* sort partitions */
        if (vdb->key_type == SHARD_DATA_TYPE_INT
            || vdb->key_type == SHARD_DATA_TYPE_DATETIME
            || vdb->key_type == SHARD_DATA_TYPE_DATE)
        {
            g_ptr_array_sort(partitions, cmp_shard_range_groups_int);
        } else {
            g_ptr_array_sort(partitions, cmp_shard_range_groups_str);
        }
        /* record the lower range, get from previous group */
        int64_t prev_value = INT_MIN;
        char *prev_str = NULL;
        int i;
        for (i = 0; i < partitions->len; ++i) {
            sharding_partition_t *part = g_ptr_array_index(partitions, i);
            part->key_type = vdb->key_type;
            if (vdb->key_type == SHARD_DATA_TYPE_STR) {
                part->low_value = prev_str;
                if (i != partitions->len - 1) {
                    prev_str = g_strdup(part->value);
                }
            } else {
                part->low_value = (void *)prev_value;
                prev_value = (int64_t) part->value;
            }
        }
    } else {
        int i;
        for (i = 0; i < partitions->len; ++i) {
            sharding_partition_t *part = g_ptr_array_index(partitions, i);
            part->key_type = vdb->key_type;
            part->hash_count = vdb->logic_shard_num;
        }
    }
}

/**
 * @return GList<sharding_vdb_t *>
 */
static GList *
parse_vdbs(cJSON *vdb_root)
{
    GList *vdb_list = NULL;
    cJSON *p = vdb_root->child;
    for (; p != NULL; p = p->next) {
        cJSON *id = cJSON_GetObjectItem(p, "id");
        cJSON *key_type = cJSON_GetObjectItem(p, "type");
        cJSON *method = cJSON_GetObjectItem(p, "method");
        cJSON *num = cJSON_GetObjectItem(p, "num");
        cJSON *partitions = cJSON_GetObjectItem(p, "partitions");
        if (!(id && key_type && method && num && partitions)) {
            g_critical("parse vdbs error, neglected");
            continue;
        }

        struct sharding_vdb_t *vdb = sharding_vdb_new();
        if (id->type == cJSON_Number) {
            vdb->id = id->valueint;
        } else {
            vdb->id = atoi(id->valuestring);
        }
        vdb->key_type = sharding_key_type(key_type->valuestring);
        if (vdb->key_type < 0) {
            g_critical("Wrong sharding settings <key_type:%s>", key_type->valuestring);
        }
        vdb->method = sharding_method(method->valuestring);
        if (vdb->method < 0) {
            g_critical("Wrong sharding settings <key_type:%s>", method->valuestring);
        }

        if (num->type == cJSON_Number) {
            vdb->logic_shard_num = num->valueint;
        } else {
            g_critical("no match num: %s", num->valuestring);
        }

        parse_partitions(partitions, vdb, vdb->partitions);
        setup_partitions(vdb->partitions, vdb);

        vdb_list = g_list_append(vdb_list, vdb);
    }
    return vdb_list;
}

static GList *
parse_tables(cJSON *root)
{
    GList *tables = NULL;
    cJSON *p = root->child;
    while (p) {
        cJSON *db = cJSON_GetObjectItem(p, "db");
        cJSON *table_root = cJSON_GetObjectItem(p, "table");
        cJSON *pkey = cJSON_GetObjectItem(p, "pkey");
        cJSON *vdb = cJSON_GetObjectItem(p, "vdb");
        if (db && table_root && pkey && vdb) {
            sharding_table_t *table = g_new0(sharding_table_t, 1);
            if (vdb->type == cJSON_String) {
                table->vdb_id = atoi(vdb->string);
            } else if (vdb->type == cJSON_Number) {
                table->vdb_id = vdb->valueint;
            }
            table->schema = g_string_new(db->valuestring);
            table->name = g_string_new(table_root->valuestring);
            table->pkey = g_string_new(pkey->valuestring);

            tables = g_list_append(tables, table);
        } else {
            g_critical("parse_tables error");
        }
        p = p->next;
    }
    return tables;
}

static GList *
parse_single_tables(cJSON *root)
{
    GList *tables = NULL;
    cJSON *p = root->child;
    while (p) {
        cJSON *name = cJSON_GetObjectItem(p, "table");
        cJSON *db = cJSON_GetObjectItem(p, "db");
        cJSON *group = cJSON_GetObjectItem(p, "group");
        if (name && db && group) {
            struct single_table_t *table = g_new0(struct single_table_t, 1);
            table->group = g_string_new(group->valuestring);
            table->schema = g_string_new(db->valuestring);
            table->name = g_string_new(name->valuestring);
            tables = g_list_append(tables, table);
        } else {
            g_critical("single_table parse error");
        }
        p = p->next;
    }
    return tables;
}

static GHashTable *
load_shard_from_json(gchar *json_str)
{
    cJSON *root = cJSON_Parse(json_str);
    if (!root) {
        g_critical("JSON format is not correct:%s", json_str);
        return NULL;
    }

    /* parse vdbs */
    cJSON *vdb_root = cJSON_GetObjectItem(root, "vdb");
    if (!vdb_root) {
        g_critical(G_STRLOC "vdb config file error");
    }
    GList *vdb_list = parse_vdbs(vdb_root);

    /* parse tables */
    cJSON *table_root = cJSON_GetObjectItem(root, "table");
    if (!table_root) {
        g_critical(G_STRLOC "table config error");
    }
    GList *table_list = parse_tables(table_root);

    /* parse single tables */
    cJSON *single_root = cJSON_GetObjectItem(root, "single_tables");
    GList *single_list = NULL;
    if (single_root) {
        single_list = parse_single_tables(single_root);
    }

    cJSON_Delete(root);

    GHashTable *shard_hash = g_hash_table_new(g_str_hash, g_str_equal);
    g_hash_table_insert(shard_hash, "table_list", table_list);
    g_hash_table_insert(shard_hash, "vdb_list", vdb_list);
    g_hash_table_insert(shard_hash, "single_tables", single_list);  /* NULLable */
    return shard_hash;
}

gboolean shard_conf_add_vdb(sharding_vdb_t* vdb)
{
    GList* l = shard_conf_vdbs;
    for (l; l; l = l->next) {
        sharding_vdb_t* base = l->data;
        if (base->id == vdb->id) {
            g_warning("add vdb dup id");
            return FALSE;
        }
    }
    setup_partitions(vdb->partitions, vdb);
    shard_conf_vdbs = g_list_append(shard_conf_vdbs, vdb);
    return TRUE;
}

gboolean shard_conf_add_sharded_table(sharding_table_t* t)
{
    sharding_vdb_t* vdb = shard_vdbs_get_by_id(shard_conf_vdbs, t->vdb_id);
    if (vdb) {
        t->vdb_ref = vdb;
        t->shard_key_type = vdb->key_type;
        return sharding_tables_add(t);
    } else {
        return FALSE;
    }
}

static cJSON* json_create_vdb_object(sharding_vdb_t* vdb)
{
    cJSON *node = cJSON_CreateObject();
    cJSON_AddNumberToObject(node, "id", vdb->id);
    cJSON_AddStringToObject(node, "type", sharding_key_type_str(vdb->key_type));
    cJSON_AddStringToObject(node, "method", vdb->method==SHARD_METHOD_HASH?"hash":"range");
    cJSON_AddNumberToObject(node, "num", vdb->logic_shard_num);
    cJSON* pob = cJSON_CreateObject();
    int i=0;
    for (i=0; i < vdb->partitions->len; ++i) {
        sharding_partition_t* p = g_ptr_array_index(vdb->partitions, i);
        if (vdb->method == SHARD_METHOD_RANGE) {
            if (vdb->key_type == SHARD_DATA_TYPE_STR) {
                cJSON_AddStringToObject(pob, p->group_name->str, (char*)p->value);
            } else if (vdb->key_type == SHARD_DATA_TYPE_INT) {
                cJSON_AddNumberToObject(pob, p->group_name->str, (int64_t)p->value);
            } else { /*datetime*/
                char time_str[32] = {0};
                time_t t = (time_t)p->value;
                chassis_epoch_to_string(&t, C(time_str));
                cJSON_AddStringToObject(pob, p->group_name->str, time_str);
            }
        } else { /*hash*/
            GArray* numbers = g_array_new(0,0,sizeof(int));
            int j;
            for (j = 0; j < p->hash_count; ++j) {
                if (TestBit(p->hash_set, j)) {
                    g_array_append_val(numbers, j);
                }
            }
            cJSON* num_array = cJSON_CreateIntArray((int*)numbers->data, numbers->len);
            g_array_free(numbers, TRUE);
            cJSON_AddItemToObject(pob, p->group_name->str, num_array);
        }
    }
    cJSON_AddItemToObject(node, "partitions", pob);
    return node;
}

gboolean shard_conf_write_json(chassis_config_t* conf_manager)
{
    cJSON* vdb_array = cJSON_CreateArray();
    GList* l;
    for (l = shard_conf_vdbs; l; l = l->next) {
        sharding_vdb_t* vdb = l->data;
        cJSON* node = json_create_vdb_object(vdb);
        cJSON_AddItemToArray(vdb_array, node);
    }

    cJSON* table_array = cJSON_CreateArray();
    GList* tables = shard_conf_get_tables();
    for (l = tables; l; l = l->next) {
        sharding_table_t* t = l->data;
        cJSON* node = cJSON_CreateObject();
        cJSON_AddStringToObject(node, "db", t->schema->str);
        cJSON_AddStringToObject(node, "table", t->name->str);
        cJSON_AddStringToObject(node, "pkey", t->pkey->str);
        cJSON_AddNumberToObject(node, "vdb", t->vdb_id);
        cJSON_AddItemToArray(table_array, node);
    }
    cJSON* root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "vdb", vdb_array);
    cJSON_AddItemToObject(root, "table", table_array);

    if (shard_conf_single_tables) {
        cJSON* single_table_array = cJSON_CreateArray();
        for (l = shard_conf_single_tables; l; l = l->next) {
            struct single_table_t* t = l->data;
            cJSON* node = cJSON_CreateObject();
            cJSON_AddStringToObject(node, "table", t->name->str);
            cJSON_AddStringToObject(node, "db", t->schema->str);
            cJSON_AddStringToObject(node, "group", t->group->str);
            cJSON_AddItemToArray(single_table_array, node);
        }
        cJSON_AddItemToObject(root, "single_tables", single_table_array);
    }

    char* json_str = cJSON_Print(root);
    chassis_config_write_object(conf_manager, "sharding", json_str);
    g_message("Update sharding.json");
    cJSON_Delete(root);
    g_free(json_str);
    return TRUE;
}

gboolean shard_conf_add_single_table(const char* schema,
                                     const char* table, const char* group)
{
    g_assert(schema && table && group);
    if (shard_conf_is_single_table(0, schema, table)) {
        g_critical("try adding duplicate single table %s.%s", schema, table);
        return FALSE;
    }
    gboolean found = FALSE;
    GList* l;
    for (l = shard_conf_all_groups; l; l = l->next) {
        GString* gp = l->data;
        if (strcmp(gp->str, group) == 0) {
            found = TRUE;
        }
    }
    if (!found) {
        g_critical("try adding single table to non-existed group: %s", group);
        return FALSE;
    }
    struct single_table_t *st = g_new0(struct single_table_t, 1);
    st->group = g_string_new(group);
    st->schema = g_string_new(schema);
    st->name = g_string_new(table);
    shard_conf_single_tables = g_list_append(shard_conf_single_tables, st);
    return TRUE;
}
