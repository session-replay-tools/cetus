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

#include "sharding-query-plan.h"

#include <string.h>

sharding_plan_t *
sharding_plan_new(const GString *orig_sql)
{
    sharding_plan_t *plan = g_new0(sharding_plan_t, 1);
    plan->orig_sql = orig_sql;
    plan->groups = g_ptr_array_new();
    return plan;
}

void
sharding_plan_free(sharding_plan_t *plan)
{
    g_ptr_array_free(plan->groups, TRUE);
    if (plan->sql_list) {
        g_list_free_full(plan->sql_list, g_string_true_free);
    }
    if (plan->mapping) {
        GList *l = plan->mapping;
        for (; l != NULL; l = l->next) {
            g_free(l->data);
        }
        g_list_free(plan->mapping);
    }

    g_free(plan);
}

void
sharding_plan_free_map(sharding_plan_t *plan)
{
    plan->is_sql_rewrite_completely = 0;
    if (plan->sql_list) {
        g_list_free_full(plan->sql_list, g_string_true_free);
        plan->sql_list = NULL;
    }
    if (plan->mapping) {
        GList *l = plan->mapping;
        for (; l != NULL; l = l->next) {
            g_free(l->data);
        }
        g_list_free(plan->mapping);
        plan->mapping = NULL;
    }
}

static struct _group_sql_pair *
sharding_plan_get_mapping(sharding_plan_t *plan, const GString *gp)
{
    if (plan->mapping) {
        GList *l = plan->mapping;
        for (; l != NULL; l = l->next) {
            struct _group_sql_pair *group = l->data;
            if (gp == group->gp_name || g_string_equal(gp, group->gp_name)) {
                return group;
            }
        }
    }
    return NULL;
}

gboolean
sharding_plan_has_group(sharding_plan_t *plan, const GString *gp)
{
    if (plan->groups) {
        int i;
        for (i = 0; i < plan->groups->len; ++i) {
            const GString *group = g_ptr_array_index(plan->groups, i);
            if (group == gp || g_string_equal(gp, group)) {
                return TRUE;
            }
        }
    }
    return FALSE;
}

static void
sharding_plan_add_mapping(sharding_plan_t *plan, const GString *group, const GString *sql)
{
    struct _group_sql_pair *pair = sharding_plan_get_mapping(plan, group);
    if (pair) {
        pair->sql = sql;
    } else {
        struct _group_sql_pair *new_pair = g_new0(struct _group_sql_pair, 1);
        new_pair->gp_name = group;
        new_pair->sql = sql;
        plan->mapping = g_list_append(plan->mapping, new_pair);
    }
}

void
sharding_plan_add_group(sharding_plan_t *plan, GString *gp_name)
{
    if (!sharding_plan_has_group(plan, gp_name)) {
        g_ptr_array_add(plan->groups, gp_name);
    }
    sharding_plan_add_mapping(plan, gp_name, NULL);
}

void
sharding_plan_add_groups(sharding_plan_t *plan, GPtrArray *groups)
{
    if (!groups) {
        return;
    }
    int i;
    for (i = 0; i < groups->len; ++i) {
        sharding_plan_add_group(plan, g_ptr_array_index(groups, i));
    }
}

void
sharding_plan_clear_group(sharding_plan_t *plan)
{
    GPtrArray *groups = plan->groups;
    g_ptr_array_remove_range(groups, 0, groups->len);
}

void
sharding_plan_add_group_sql(sharding_plan_t *plan, GString *gp_name, GString *sql)
{
    plan->sql_list = g_list_append(plan->sql_list, sql);
    if (!sharding_plan_has_group(plan, gp_name)) {
        g_ptr_array_add(plan->groups, gp_name);
    }
    sharding_plan_add_mapping(plan, gp_name, sql);
}

const GString *
sharding_plan_get_sql(sharding_plan_t *plan, const GString *group)
{
    struct _group_sql_pair *pair = sharding_plan_get_mapping(plan, group);
    if (pair) {
        if (pair->sql) {
            return pair->sql;
        } else {
            return plan->is_modified ? plan->modified_sql : plan->orig_sql;
        }
    } else {
        return plan->is_modified ? plan->modified_sql : plan->orig_sql;
    }
}

void
sharding_plan_set_modified_sql(sharding_plan_t *plan, GString *sql)
{
    plan->is_modified = 1;
    plan->modified_sql = sql;
}

static gint
gstr_comp(gconstpointer a1, gconstpointer a2)
{
    GString *s1 = *(GString **)a1;
    GString *s2 = *(GString **)a2;
    return strcmp(s1->str, s2->str);
}

void
sharding_plan_sort_groups(sharding_plan_t *plan)
{
    g_ptr_array_sort(plan->groups, gstr_comp);
}
