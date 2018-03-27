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

#ifndef _CETUS_QUERY_QUEUE_H_
#define _CETUS_QUERY_QUEUE_H_

#include <glib.h>

typedef struct query_queue_t {
    GQueue *chunks;
    int max_len;
} query_queue_t;

query_queue_t *query_queue_new(int max_len);
void query_queue_free(query_queue_t *);
void query_queue_append(query_queue_t *, GString *);
void query_queue_dump(query_queue_t *);

#endif /* _CETUS_QUERY_QUEUE_H_ */
