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

#ifndef _CHASSIS_PERM_H_
#define _CHASSIS_PERM_H_

#include <glib.h>
#include "chassis-exports.h"

#include <sys/stat.h>
#define CHASSIS_FILEMODE_SECURE_MASK (S_IROTH|S_IWOTH|S_IXOTH)

CHASSIS_API int chassis_filemode_check_full(const gchar *, int, GError **);

#endif
