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

#ifndef _SQL_FILTER_VARIABLES_H_
#define _SQL_FILTER_VARIABLES_H_

#include <glib.h>

gboolean sql_filter_vars_load_str_rules(const char *json_str);

void sql_filter_vars_load_default_rules();

void sql_filter_vars_shard_load_default_rules();

void sql_filter_vars_destroy();

gboolean sql_filter_vars_is_silent(const char *name, const char *val);

gboolean sql_filter_vars_is_allowed(const char *name, const char *val);

#endif /*_SQL_FILTER_VARIABLES_H_*/
