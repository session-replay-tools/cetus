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

#ifndef _QUERY_HANDLING_H_
#define _QUERY_HANDLING_H_

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <glib.h>

#include "network-exports.h"

typedef struct {
    /**
     * the content of the OK packet 
     */
    guint16 server_status;
    guint16 warning_count;
    guint64 affected_rows;
    guint64 insert_id;

    gboolean was_resultset;  /**< if set, affected_rows and insert_id are ignored */

    /*
     * < if set, the row data is binary encoded. we need the metadata to decode 
     */
    gboolean binary_encoded;

    /**
     * MYSQLD_PACKET_OK or MYSQLD_PACKET_ERR
     */
    guint8 query_status;
} query_status;

typedef struct {
    /**< a unique id set by the scripts to map the query to a handler */
    int id;
    /**< flag to announce if we have to buffer the result for later processing */
    unsigned int resultset_is_needed:1;
    unsigned int is_fast_streamed:1;

    GString *query;

    /* the userdata's need them */
    GQueue *result_queue;                   /**< the data to parse */

    guint64 rows;
    guint64 bytes;

    guint64 ts_read_query;
    guint64 ts_read_query_result_last;

    /**< summary information about the query status */
    query_status qstat;

} injection;

/**
 * a injection_queue is GQueue for now
 *
 */
typedef GQueue network_injection_queue;

NETWORK_API network_injection_queue *network_injection_queue_new(void);
NETWORK_API void network_injection_queue_free(network_injection_queue *q);
NETWORK_API void network_injection_queue_reset(network_injection_queue *q);
NETWORK_API void network_injection_queue_prepend(network_injection_queue *q, injection *inj);
NETWORK_API void network_injection_queue_append(network_injection_queue *q, injection *inj);

/**
 * parsed result set
 */
typedef struct {
    GQueue *result_queue;   /**< where the packets are read from */

    GPtrArray *fields;      /**< the parsed fields */

    GList *rows_chunk_head; /**< pointer to the EOF packet after the fields */
    GList *row;             /**< the current row */

    query_status qstat;     /**< state of this query */

    guint64 rows;
    guint64 bytes;
} proxy_resultset_t;

NETWORK_API injection *injection_new(int id, GString *query);
NETWORK_API void injection_free(injection *i);

NETWORK_API proxy_resultset_t *proxy_resultset_new();
NETWORK_API void proxy_resultset_free(proxy_resultset_t *res);

#endif /* _QUERY_HANDLING_H_ */
