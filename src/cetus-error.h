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

#ifndef _CETUS_ERROR_H_
#define _CETUS_ERROR_H_

/**
 * ERROR codes extends ER_xx from mysqld_error.h
 */

enum {
    ER_CETUS_UNKNOWN = 5001,
    ER_CETUS_RESULT_MERGE,
    ER_CETUS_LONG_RESP,
    ER_CETUS_PARSE_SHARDING,
    ER_CETUS_NOT_SUPPORTED,
    ER_CETUS_SINGLE_NODE_FAIL,
    ER_CETUS_NO_GROUP,
};

#endif /*_CETUS_ERROR_H_*/
