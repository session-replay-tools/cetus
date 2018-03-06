#include "sharding-config.h"

#include <stdlib.h>
#include <string.h>
#include "glib-ext.h"
#include "sys-pedantic.h"
#include "cJSON.h"
#include "chassis-timings.h"

static GList *shard_conf_vdbs = NULL;

static GList *shard_conf_tables = NULL;

static GHashTable *shard_conf_vdb_map = NULL;

static GList *shard_conf_single_tables = NULL;


typedef struct sharding_database_t {
    char *name;
    GHashTable *tables; /* <char *, const sharding_table_t *> */
} sharding_database_t;

static sharding_database_t *sharding_database_new(const char *name)
{
    sharding_database_t *db = g_new0(sharding_database_t, 1);
    db->tables = g_hash_table_new(g_str_hash, g_str_equal);
    db->name = g_strdup(name);
    return db;
}

static void sharding_database_free(sharding_database_t *db)
{
    g_free(db->name);
    g_hash_table_destroy(db->tables);
    g_free(db);
}

static void sharding_database_add_table(sharding_database_t *db, sharding_table_t *table)
{
    g_hash_table_insert(db->tables, table->name->str, table);
}

static sharding_table_t *sharding_database_get_table(sharding_database_t *db,
                                                     const char *table)
{
    if (!table)
        return NULL;
    return g_hash_table_lookup(db->tables, table);
}

static sharding_vdb_t *shard_vdbs_get_by_id(GList *vdbs, int id)
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

void sharding_table_free(gpointer q) {
    sharding_table_t *info = q;
    if (NULL != info->db) g_string_free(info->db, TRUE);
    if (NULL != info->name) g_string_free(info->name, TRUE);
    if (NULL != info->pkey) g_string_free(info->pkey, TRUE);
    g_free(info);
}

gboolean sharding_partition_contain_hash(sharding_partition_t *partition, int val)
{
    g_assert(partition->vdb->method == SHARD_METHOD_HASH);
    if (val >= partition->vdb->logic_shard_num)
        return FALSE;
    return TestBit(partition->hash_set, val);
}

static sharding_vdb_t *sharding_vdb_new()
{
    sharding_vdb_t *vdb = g_new0(struct sharding_vdb_t, 1);
    vdb->databases = g_ptr_array_new_with_free_func((GDestroyNotify)sharding_database_free);
    vdb->partitions = g_ptr_array_new();
    return vdb;
}

static void sharding_vdb_free(sharding_vdb_t *vdb)
{
    if (!vdb) {
        return;
    }

    int i;
    for (i = 0; i < vdb->partitions->len; i++) {
        sharding_partition_t *item = g_ptr_array_index(vdb->partitions, i);
        if (vdb->method == SHARD_METHOD_RANGE) {
            if (vdb->key_type == SHARD_DATA_TYPE_STR) {
                if (item->value) {
                    g_free(item->value);
                }
                if (item->low_value) {
                    g_free(item->low_value);
                }
            }
        }
        if (item->group_name) {
            g_string_free(item->group_name, TRUE);
        }
        g_free(item);
    }
    g_ptr_array_free(vdb->partitions, TRUE);

    g_ptr_array_free(vdb->databases, TRUE);
    g_free(vdb);
}

static gboolean sharding_vdb_is_valid(sharding_vdb_t *vdb)
{
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

static sharding_database_t *sharding_vdb_get_database(sharding_vdb_t *vdb,
                                                      const char *db_name)
{
    int i = 0;
    for (i = 0; i < vdb->databases->len; ++i) {
        sharding_database_t *db = g_ptr_array_index(vdb->databases, i);
        if (strcasecmp(db_name, db->name) == 0) {
            return db;
        }
    }
    return NULL;
}

static sharding_database_t *sharding_vdb_add_database(sharding_vdb_t *vdb,
                                                      const char *name)
{
    sharding_database_t *db = sharding_database_new(name);
    g_ptr_array_add(vdb->databases, db);
    return db;
}

GPtrArray *shard_conf_get_all_groups(GPtrArray *visited_groups, const char *db)
{
    if (!db) {
        g_warning(G_STRLOC " db name is NULL");
        return visited_groups;
    }
    sharding_vdb_t *vdb = g_hash_table_lookup(shard_conf_vdb_map, db);
    if (vdb) {
        int i = 0;
        for (i = 0; i < vdb->partitions->len; ++i) {
            sharding_partition_t *part = g_ptr_array_index(vdb->partitions, i);
            GString *gp = part->group_name;
            g_ptr_array_add(visited_groups, gp);
        }
    } else {
        g_warning(G_STRLOC " fail to get all groups for db: %s", db);
    }
    return visited_groups;
}

void shard_conf_find_groups(GPtrArray *groups, const char *match, const char *db)
{
    if (strcasecmp(match, "all") == 0) {
        shard_conf_get_all_groups(groups, db);
        return;
    }
    sharding_vdb_t *vdb = g_hash_table_lookup(shard_conf_vdb_map, db);
    if (vdb) {
        int i = 0;
        for (i = 0; i < vdb->partitions->len; ++i) {
            sharding_partition_t *part = g_ptr_array_index(vdb->partitions, i);
            GString *gp = part->group_name;
            if (strcmp(gp->str, match) == 0) {
                g_ptr_array_add(groups, gp);
                return;
            }
        }
    }
}

GPtrArray *shard_conf_get_any_group(GPtrArray *visited_groups, char *db,
                                    char *UNUSED_PARAM(table))
{
    if (!db) {
        g_warning(G_STRLOC " db name is NULL");
        return NULL;
    }
    sharding_vdb_t *vdb = g_hash_table_lookup(shard_conf_vdb_map, db);
    if (vdb == NULL) {
        return NULL;
    }

    GPtrArray *partitions = vdb->partitions;
    int i = rand() % partitions->len;

    sharding_partition_t *part = g_ptr_array_index(partitions, i);
    g_ptr_array_add(visited_groups, part->group_name);

    return visited_groups;
}

GPtrArray *shard_conf_get_table_groups(GPtrArray *visited_groups, char *db,
                                       char *UNUSED_PARAM(table))
{
    if (!db) {
        g_warning(G_STRLOC " db name is NULL");
        return NULL;
    }

    sharding_vdb_t *vdb = g_hash_table_lookup(shard_conf_vdb_map, db);
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
GPtrArray *shard_conf_table_partitions(GPtrArray *partitions,
                      const char *db, const char *UNUSED_PARAM(table)) {
    if (!db) {
        g_warning(G_STRLOC " db name is NULL");
        return NULL;
    }

    sharding_vdb_t *vdb = g_hash_table_lookup(shard_conf_vdb_map, db);
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

sharding_table_t *shard_conf_get_info(const char *db_name, const char *table)
{
    if (!db_name)
        return NULL;
    sharding_vdb_t *vdb = g_hash_table_lookup(shard_conf_vdb_map, db_name);
    if (!vdb) {
        return NULL;
    }
    sharding_database_t *db = sharding_vdb_get_database(vdb, db_name);
    if (!db) {
        return NULL;
    }
    return sharding_database_get_table(db, table);
}

gboolean shard_conf_is_shard_table(const char *db, const char *table)
{
    return shard_conf_get_info(db, table) ? TRUE : FALSE;
}

GPtrArray *shard_conf_get_fixed_group(GPtrArray *groups, const char *db, guint32 fixture)
{
    if (!db) {
        g_warning(G_STRLOC " db name is NULL");
        return groups;
    }
    sharding_vdb_t *vdb = g_hash_table_lookup(shard_conf_vdb_map, db);
    if (vdb) {
        int base = vdb->partitions->len;
        if (base == 0) {
            return groups;
        }
        int index = fixture % base;
        sharding_partition_t *part = g_ptr_array_index(vdb->partitions, index);
        g_ptr_array_add(groups, part->group_name);
    }
    return groups;
}

typedef struct single_table_t { /* single table only resides on 1 group */
    GString *name;
    GString *db;
    GString *group;
} single_table_t;

void single_table_free(single_table_t *t)
{
    if (t) {
        g_string_free(t->name, TRUE);
        g_string_free(t->db, TRUE);
        g_string_free(t->group, TRUE);
        g_free(t);
    }
}

static void shard_conf_set_vdb_list(GList *vdbs)
{
    g_list_free_full(shard_conf_vdbs, (GDestroyNotify)sharding_vdb_free);
    shard_conf_vdbs = vdbs;
}

static void shard_conf_set_table_list(GList *tables)
{
    g_list_free_full(shard_conf_tables, (GDestroyNotify)sharding_table_free);
    shard_conf_tables = tables;
}

static void shard_conf_set_vdb_map(GHashTable *vdbmap)
{
    if (shard_conf_vdb_map) {
        g_hash_table_destroy(shard_conf_vdb_map);
    }
    shard_conf_vdb_map = vdbmap;
}

static void shard_conf_set_single_tables(GList *tables)
{
    g_list_free_full(shard_conf_single_tables, (GDestroyNotify)single_table_free);
    shard_conf_single_tables = tables;
}

/**
 * setup index & validate configurations
 */
gboolean shard_conf_try_setup(GList *vdbs, GList *tables, GList *single_tables)
{
    if (!vdbs || !tables) {
        g_critical("empty vdb/table list");
        return FALSE;
    }
    GList *l = vdbs;
    for (; l != NULL; l = l->next) {
        sharding_vdb_t *vdb = l->data;
        if (!sharding_vdb_is_valid(vdb)) {
            g_warning("invalid vdb config");
            return FALSE;
        }
    }
    GHashTable *vdbmap = g_hash_table_new(g_str_hash, g_str_equal);

    l = tables;
    for (; l != NULL; l = l->next) {
        sharding_table_t *table = l->data;
        sharding_vdb_t *vdb = shard_vdbs_get_by_id(vdbs, table->vdb_id);

        /* Fill table with vdb data */
        if (!vdb) {
            g_critical(G_STRLOC " table:%s VDB ID cannot be found: %d",
                       table->name->str, table->vdb_id);
            g_hash_table_destroy(vdbmap);
            return FALSE;
        } else {
            table->shard_key_type = vdb->key_type;
            table->logic_shard_num = vdb->logic_shard_num;
            table->method = vdb->method;
            table->partitions = vdb->partitions;
        }

        /* collect database into vdb */
        sharding_database_t *database = sharding_vdb_get_database(vdb, table->db->str);
        if (!database) {
            database = sharding_vdb_add_database(vdb, table->db->str);
        }

        /* setup table map in database */
        if (sharding_database_get_table(database, table->name->str)) {
            g_hash_table_destroy(vdbmap);
            g_critical(G_STRLOC " same table name inside same db: %s", table->name->str);
            return FALSE;
        }
        sharding_database_add_table(database, table);
        
        /* setup db to vdb map */
        sharding_vdb_t *vdb_res = g_hash_table_lookup(vdbmap, database->name);
        if (vdb_res && vdb_res != vdb) {
            g_hash_table_destroy(vdbmap);
            g_critical(G_STRLOC " same db inside different vdb: %s", database->name);
            return FALSE;
        } else {
            g_hash_table_insert(vdbmap, database->name, vdb);
        }
    }
    shard_conf_set_vdb_list(vdbs);
    shard_conf_set_table_list(tables);
    shard_conf_set_vdb_map(vdbmap);
    shard_conf_set_single_tables(single_tables);
    return TRUE;
}

void shard_conf_destroy(void)
{
    if (shard_conf_vdbs) {
        g_list_free_full(shard_conf_vdbs, (GDestroyNotify)sharding_vdb_free);
    }
    g_list_free_full(shard_conf_tables, (GDestroyNotify)sharding_table_free);
    if (shard_conf_vdb_map) {
        g_hash_table_destroy(shard_conf_vdb_map);
    }
}

static GHashTable *load_shard_from_json(gchar *json_str);

gboolean shard_conf_load(char *json_str)
{
    GHashTable *ht = load_shard_from_json(json_str);
    if (!ht)
        return FALSE;

    GList *tables = g_hash_table_lookup(ht, "table_list");
    GList *vdbs = g_hash_table_lookup(ht, "vdb_list");
    GList *single_tables = g_hash_table_lookup(ht, "single_tables");
    gboolean success = shard_conf_try_setup(vdbs, tables, single_tables);
    if (!success) {
        g_list_free_full(vdbs, (GDestroyNotify)sharding_vdb_free);
        g_list_free_full(tables, (GDestroyNotify)sharding_table_free);
    }
    g_hash_table_destroy(ht);
    return success;
}

static single_table_t *shard_conf_get_single_table(const char *db, const char *name)
{
    GList *l = shard_conf_single_tables;
    for (; l; l = l->next) {
        single_table_t *t = l->data;
        if (strcasecmp(t->name->str, name) == 0
            && strcasecmp(t->db->str, db) == 0) {
            return t;
        }
    }
    return NULL;
}

gboolean shard_conf_is_single_table(const char *db, const char *name)
{
    single_table_t *t = shard_conf_get_single_table(db, name);
    return t != NULL;
}

static gboolean shard_conf_group_contains(GPtrArray *groups, GString *match)
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

GPtrArray *shard_conf_get_single_table_distinct_group(GPtrArray *groups,
                                                      const char *db, const char *name)
{
    single_table_t *t = shard_conf_get_single_table(db, name);
    if (t && !shard_conf_group_contains(groups, t->group)) {
        g_ptr_array_add(groups, t->group);
    }
    return groups;
}

static int sharding_type(const char *str) {
    struct code_map_t {
        const char *name;
        int code;
    } map[] = {
        {"INT", SHARD_DATA_TYPE_INT},
        {"STR", SHARD_DATA_TYPE_STR},
        {"DATE", SHARD_DATA_TYPE_DATE},
        {"DATETIME", SHARD_DATA_TYPE_DATETIME},
    };
    int i;
    for (i = 0; i < sizeof(map)/sizeof(*map); ++i) {
        if (strcasecmp(map[i].name, str) == 0)
            return map[i].code;
    }
    g_critical("Wrong sharding setting <key_type:%s>", str);
    return -1;
}

static int sharding_method(const char *str) {
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
static void parse_partitions(cJSON *root, const sharding_vdb_t *vdb,
                             GPtrArray *partitions /* out */)
{
    cJSON *cur = root->child;
    sharding_partition_t *item;
    for (; cur; cur = cur->next) { /* { "groupA":xx, "groupB":xx, "groupC":xx} */
        /* null means unlimited */
        switch (cur->type) {
        case cJSON_NULL:  /* range: null */
            item = g_new0(sharding_partition_t, 1);
            item->vdb = vdb;
            item->group_name = g_string_new(cur->string);
            if (vdb->key_type == SHARD_DATA_TYPE_STR) {
                item->value = NULL;
            } else {
                item->value = (void *) (uint64_t) INT_MAX;
            }
            g_ptr_array_add(partitions, item);
            break;
        case cJSON_Number:  /* range > 123 */
            item = g_new0(sharding_partition_t, 1);
            item->vdb = vdb;
            item->group_name = g_string_new(cur->string);
            item->value = (void *) (uint64_t) cur->valueint;
            g_ptr_array_add(partitions, item);
            break;
        case cJSON_String:  /* range > "str" */
            item = g_new0(sharding_partition_t, 1);
            item->vdb = vdb;
            item->group_name = g_string_new(cur->string);
            if (vdb->key_type == SHARD_DATA_TYPE_DATETIME
                || vdb->key_type == SHARD_DATA_TYPE_DATE) {
                gboolean ok;
                int epoch = chassis_epoch_from_string(cur->valuestring, &ok);
                if (ok)
                    item->value = (void *) (uint64_t)epoch;
                else
                    g_warning("Wrong sharding setting <datetime format:%s>",
                              cur->valuestring);
            } else {
                item->value = g_strdup(cur->valuestring);
            }
            g_ptr_array_add(partitions, item);
            break;
        case cJSON_Array: {
            cJSON *elem = cur->child;
            if (cJSON_Number == elem->type) { /* hash in [0,3,5] */
                item = g_new0(sharding_partition_t, 1);
                item->vdb = vdb;
                item->group_name = g_string_new(cur->string);
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
            } else if (cJSON_String == elem->type) { /* TODO: range in [0, 100, 200] */
                while (elem != NULL) {
                    item = g_new0(sharding_partition_t, 1);
                    item->vdb = vdb;
                    item->group_name = g_string_new(cur->string);
                    if (vdb->key_type == SHARD_DATA_TYPE_DATETIME
                        || vdb->key_type == SHARD_DATA_TYPE_DATE) {
                        gboolean ok;
                        int epoch = chassis_epoch_from_string(elem->valuestring, &ok);
                        if (ok)
                            item->value = (void *) (uint64_t)epoch;
                        else
                            g_warning("Wrong sharding setting <datetime format:%s>",
                                      elem->valuestring);
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
        } /* end switch */
    }
}

static gint cmp_shard_range_groups_int(gconstpointer a, gconstpointer b) {
    sharding_partition_t *item1 = *(sharding_partition_t **)a;
    sharding_partition_t *item2 = *(sharding_partition_t **)b;
    int n1 = (int) (int64_t) item1->value;
    int n2 = (int) (int64_t) item2->value;
    return n1-n2;
}

static gint cmp_shard_range_groups_str(gconstpointer a, gconstpointer b) {
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
            || vdb->key_type == SHARD_DATA_TYPE_DATE) {
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
            if (vdb->key_type == SHARD_DATA_TYPE_STR) {
                part->low_value = prev_str;
                if (i != partitions->len-1) {
                    prev_str = g_strdup(part->value);
                }
            } else {
                part->low_value = (void *)prev_value;
                prev_value = (int64_t)part->value;
            }
        }
    }
}
/**
 * @return GList<sharding_vdb_t *>
 */
static GList *parse_vdbs(cJSON *vdb_root)
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
        vdb->key_type = sharding_type(key_type->valuestring);
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

static GList *parse_tables(cJSON *root)
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
            table->db = g_string_new(db->valuestring);
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

static GList *parse_single_tables(cJSON *root)
{
    GList *tables = NULL;
    cJSON *p = root->child;
    while (p) {
        cJSON *name = cJSON_GetObjectItem(p, "table");
        cJSON *db = cJSON_GetObjectItem(p, "db");
        cJSON *group = cJSON_GetObjectItem(p, "group");
        if (name && db && group) {
            single_table_t *table = g_new0(single_table_t, 1);
            table->group = g_string_new(group->valuestring);
            table->db = g_string_new(db->valuestring);
            table->name = g_string_new(name->valuestring);
            tables = g_list_append(tables, table);
        } else {
            g_critical("single_table parse error");
        }
        p = p->next;
    }
    return tables;
}

static GHashTable *load_shard_from_json(gchar *json_str)
{
    cJSON *root = cJSON_Parse(json_str);
    if (!root) {
        g_critical("JSON format is not correct!");
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
    g_hash_table_insert(shard_hash, "single_tables", single_list); /* NULLable */
    return shard_hash;
}

