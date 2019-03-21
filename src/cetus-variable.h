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

#ifndef CETUS_VARIABLE_H
#define CETUS_VARIABLE_H

#include <inttypes.h>

enum cetus_variable_type_t {
    VAR_INT,
    VAR_INT64,
    VAR_FLOAT,
    VAR_STRING,
};

struct chassis;
typedef struct cetus_variable_t {
    char *name;
    void *value;
    enum cetus_variable_type_t type;
} cetus_variable_t;

/**
 @return newly allocated string, must be freed
 */
char *cetus_variable_get_value_str(cetus_variable_t *var);

#endif /* CETUS_VARIABLES_H */
