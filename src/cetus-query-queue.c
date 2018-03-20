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

#include "cetus-query-queue.h"

#include <mysql.h>
#include <stdio.h>
#include <stdlib.h>

#include "glib-ext.h"
#include "network-mysqld-proto.h"

struct query_entry_t {
    enum enum_server_command command;
    GString *sql;
    time_t recv_time;
};

static void
query_entry_free(struct query_entry_t *entry)
{
    if (entry->sql) {
        g_string_free(entry->sql, TRUE);
    }
    g_free(entry);
}

query_queue_t *
query_queue_new(int len)
{
    query_queue_t *q = g_new0(struct query_queue_t, 1);
    q->chunks = g_queue_new();
    q->max_len = len;
    return q;
}

void
query_queue_free(query_queue_t *q)
{
    g_queue_foreach(q->chunks, (GFunc) query_entry_free, NULL);
    g_queue_free(q->chunks);
    g_free(q);
}

void
query_queue_append(query_queue_t *q, GString *data)
{
    network_packet packet = { data, 0 };
    guint8 command = 0;
    if (packet.data) {
        network_mysqld_proto_skip_network_header(&packet);
        if (network_mysqld_proto_get_int8(&packet, &command) != 0) {
            return;
        }
    } else {
        g_warning("%s: packet data is nill ", G_STRLOC);
        return;
    }

    struct query_entry_t *entry = g_new0(struct query_entry_t, 1);
    entry->command = command;
    if (command == COM_QUERY) {
        gsize sql_len = packet.data->len - packet.offset;
        entry->sql = g_string_sized_new(sql_len + 1);
        network_mysqld_proto_get_gstr_len(&packet, sql_len, entry->sql);
    }
    entry->recv_time = time(0);

    if (g_queue_get_length(q->chunks) >= q->max_len) {  /* TODO: slow */
        struct query_entry_t *old = g_queue_pop_head(q->chunks);
        query_entry_free(old);
    }
    g_queue_push_tail(q->chunks, entry);
}

const char *
command_name(enum enum_server_command cmd)
{
    static char number[8];
    switch (cmd) {
    case COM_QUERY:
        return "COM_QUERY";
    case COM_QUIT:
        return "COM_QUIT";
    case COM_INIT_DB:
        return "COM_INIT_DB";
    case COM_FIELD_LIST:
        return "COM_FIELD_LIST";
    default:
        snprintf(number, sizeof(number), "COM_<%d>", cmd);  /* TODO: only works for 1 cmd */
        return number;
    }
}

static void
query_entry_dump(gpointer data, gpointer user_data)
{
    struct query_entry_t *entry = (struct query_entry_t *)data;
    struct tm *tm = localtime(&entry->recv_time);
    char tm_str[16] = { 0 };
    snprintf(tm_str, sizeof(tm_str), "%d:%d:%d", tm->tm_hour, tm->tm_min, tm->tm_sec);
    g_message("%s %s %s", tm_str, command_name(entry->command), entry->sql ? entry->sql->str : "");
}

void
query_queue_dump(query_queue_t *q)
{
    if (!g_queue_is_empty(q->chunks)) {
        g_message("recent queries:");
        g_queue_foreach(q->chunks, query_entry_dump, NULL);
    }
}
