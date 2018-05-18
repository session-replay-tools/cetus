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

#ifndef  __SHARDING_CONFIG_H__
#define  __SHARDING_CONFIG_H__

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "glib-ext.h"
#include "cetus-util.h"

#define SHARD_DATA_TYPE_UNSUPPORTED 0
#define SHARD_DATA_TYPE_INT 1
#define SHARD_DATA_TYPE_STR 2
#define SHARD_DATA_TYPE_DATE 3
#define SHARD_DATA_TYPE_DATETIME 4

enum sharding_method_t {
    SHARD_METHOD_UNKNOWN = -1,
    SHARD_METHOD_RANGE = 0,
    SHARD_METHOD_HASH = 1,
    SHARD_METHOD_LIST = 2
};

typedef struct sharding_vdb_t sharding_vdb_t;
typedef struct sharding_table_t sharding_table_t;

#define MAX_HASH_VALUE_COUNT 1024

typedef struct sharding_partition_t {
    char *value;                /* high range OR hash value */
    char *low_value;            /* low range OR null */

    BitArray hash_set[MAX_HASH_VALUE_COUNT / 32];   /* hash values of this partition */

    GString *group_name;
    const sharding_vdb_t *vdb;  /* references the vdb it belongs to */
} sharding_partition_t;

gboolean sharding_partition_contain_hash(sharding_partition_t *, int);
//gboolean sharding_partition_cover_range(sharding_partition_t *, );

struct sharding_vdb_t {
    int id;
    enum sharding_method_t method;
    int key_type;
    int logic_shard_num;
    GPtrArray *partitions;      /* GPtrArray<sharding_partition_t *> */
};

struct sharding_table_t {
    GString *schema;
    GString *name;
    GString *pkey;
    int shard_key_type;
    int vdb_id;
    struct sharding_vdb_t *vdb_ref;
};

GPtrArray *shard_conf_get_any_group(GPtrArray *groups, const char *db, const char *table);

GPtrArray *shard_conf_get_all_groups(GPtrArray *groups);

/* same fixture will get same group */
GPtrArray *shard_conf_get_fixed_group(GPtrArray *groups, guint32 fixture);

GPtrArray *shard_conf_get_table_groups(GPtrArray *visited_groups,
                                       const char *db, const char *table);

gboolean shard_conf_is_shard_table(const char *db, const char *table);

gboolean shard_conf_is_single_table(const char *db, const char *table);

GPtrArray *shard_conf_get_single_table_distinct_group(GPtrArray *groups, const char *db, const char *table);

sharding_table_t *shard_conf_get_info(const char *db, const char *table);

/**
 * similar with shard_conf_get_table_groups
 * @return array of  group_item_t *
 */
GPtrArray *shard_conf_table_partitions(GPtrArray *partitions, const char *db, const char *table);

/**
 * find partition by group name
 * special name "all" will get all groups
 */
void shard_conf_find_groups(GPtrArray *groups, const char *match);

gboolean shard_conf_load(char *, int);

void shard_conf_destroy(void);

#endif /* __SHARDING_CONFIG_H__ */
