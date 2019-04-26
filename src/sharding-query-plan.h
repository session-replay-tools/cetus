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

#ifndef SHARDING_QUERY_PLAN
#define SHARDING_QUERY_PLAN

#include "glib-ext.h"

struct _group_sql_pair {
    /* group names references sharding_partition_t.group_name */
    const GString *gp_name;

    /* sql references con->orig_sql or sharding_plan_t.sql_list */
    const GString *sql;
};

enum sharding_table_type_t {
    GLOBAL_TABLE,
    SHARDED_TABLE,
    SINGLE_TABLE,
};

typedef struct sharding_plan_t {
    GPtrArray *groups;          /* GPtrArray<GString *> */
    GList *sql_list;            /* GList<GString *> */
    GList *mapping;             /* GList<struct _group_sql_pair *> */
    const GString *orig_sql;
    const GString *modified_sql;
    enum sharding_table_type_t table_type;
    unsigned int is_partition_mode:1;
    unsigned int is_modified:1;
    unsigned int is_sql_rewrite_completely:1;
} sharding_plan_t;

sharding_plan_t *sharding_plan_new(const GString *orig_sql);

void sharding_plan_free(sharding_plan_t *);
void sharding_plan_free_map(sharding_plan_t *);

void sharding_plan_set_modified_sql(sharding_plan_t *, GString *sql);

const GString *sharding_plan_get_sql(sharding_plan_t *, const GString *group);

gboolean sharding_plan_has_group(sharding_plan_t *plan, const GString *gp);

/* use orig_sql or modified sql */
void sharding_plan_add_group(sharding_plan_t *, GString *gp_name);

void sharding_plan_add_groups(sharding_plan_t *, GPtrArray *groups);

void sharding_plan_clear_group(sharding_plan_t *);

/* use group-specific sql */
void sharding_plan_add_group_sql(sharding_plan_t *, GString *gp_name, GString *sql);

void sharding_plan_sort_groups(sharding_plan_t *);

#endif /* SHARDING_QUERY_PLAN */
