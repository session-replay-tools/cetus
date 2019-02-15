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

#ifndef __SHARDING_PARSER_H__
#define __SHARDING_PARSER_H__

#include "network-mysqld.h"
#include "sharding-query-plan.h"
#include "sql-context.h"

#define USE_NON_SHARDING_TABLE 0    /* default */
#define USE_SHARDING 1          /* default */
#define USE_DIS_TRAN 2
#define USE_ANY_SHARDINGS 3     /* default */
#define USE_ALL_SHARDINGS 4     /* default */
#define USE_ALL 5               /* default */
#define USE_SAME 6
#define USE_PREVIOUS_WARNING_CONN 7
#define USE_NONE 8
#define USE_PREVIOUS_TRAN_CONNS 9
#define ERROR_UNPARSABLE -1

NETWORK_API int check_property_has_groups(sql_context_t *);

NETWORK_API int sharding_parse_groups_by_property(GString *, sql_context_t *, sharding_plan_t *);

NETWORK_API int sharding_parse_groups(GString *, sql_context_t *, query_stats_t *, guint64, sharding_plan_t *);

NETWORK_API GString *sharding_modify_sql(sql_context_t *, having_condition_t *, int, int, int);

NETWORK_API void sharding_filter_sql(sql_context_t *);

#endif //__SHARDING_PARSER_H__
