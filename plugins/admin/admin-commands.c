#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <ctype.h>
#include <errno.h>
#include <sys/stat.h>

#include "admin-commands.h"
#include "cetus-users.h"
#include "cetus-util.h"
#include "cetus-variable.h"
#include "cetus-acl.h"
#include "character-set.h"
#include "chassis-event.h"
#include "chassis-options.h"
#include "cetus-monitor.h"
#include "glib-ext.h"
#include "network-mysqld-packet.h"
#include "network-mysqld-proto.h"
#include "network-mysqld.h"
#include "server-session.h"
#include "sys-pedantic.h"
#include "admin-plugin.h"
#include "sharding-config.h"
#include "chassis-options-utils.h"
#include "chassis-sql-log.h"
#include "cetus-acl.h"

static gint save_setting(chassis *srv, gint *effected_rows);
static void send_result(network_socket *client, gint ret, gint affected);

static const char *get_conn_xa_state_name(network_mysqld_con_dist_tran_state_t state) {
    switch (state) {
    case NEXT_ST_XA_START: return "XS";
    case NEXT_ST_XA_QUERY: return "XQ";
    case NEXT_ST_XA_END: return "XE";
    case NEXT_ST_XA_PREPARE: return "XP";
    case NEXT_ST_XA_COMMIT: return "XC";
    case NEXT_ST_XA_ROLLBACK: return "XR";
    case NEXT_ST_XA_CANDIDATE_OVER: return "XCO";
    case NEXT_ST_XA_OVER: return "XO";
    default: return "NX";
    }
}

static void g_table_free_all(gpointer q) {
    GHashTable *table = q;
    g_hash_table_destroy(table);
}

typedef struct used_conns {
    int num;
} used_conns_t;

void admin_syntax_error(network_mysqld_con* con)
{
    *(int*)(con->plugin_con_state) = -1;
}
void admin_stack_overflow(network_mysqld_con* con)
{
    *(int*)(con->plugin_con_state) = -1;
}
int admin_get_error(network_mysqld_con* con)
{
    return *(int*)(con->plugin_con_state);
}
void admin_clear_error(network_mysqld_con* con)
{
    *(int*)(con->plugin_con_state) = 0;
}
void admin_select_all_backends(network_mysqld_con* con)
{
    if (con->is_admin_client) {
        con->admin_read_merge = 1;
        return;
    }
    chassis *chas = con->srv;
    chassis_private *priv = chas->priv;
    chassis_plugin_config *config = con->config;

    GPtrArray *fields = g_ptr_array_new_with_free_func(
        (GDestroyNotify)network_mysqld_proto_fielddef_free);

    MYSQL_FIELD *field;
    
    field = network_mysqld_proto_fielddef_new();
    field->name = g_strdup("PID");
    field->type = MYSQL_TYPE_STRING;
    g_ptr_array_add(fields, field);

    field = network_mysqld_proto_fielddef_new();
    field->name = g_strdup("backend_ndx");
    field->type = MYSQL_TYPE_STRING;
    g_ptr_array_add(fields, field);

    field = network_mysqld_proto_fielddef_new();
    field->name = g_strdup("address");
    field->type = MYSQL_TYPE_STRING;
    g_ptr_array_add(fields, field);

    field = network_mysqld_proto_fielddef_new();
    field->name = g_strdup("state");
    field->type = MYSQL_TYPE_STRING;
    g_ptr_array_add(fields, field);

    field = network_mysqld_proto_fielddef_new();
    field->name = g_strdup("type");
    field->type = MYSQL_TYPE_STRING;
    g_ptr_array_add(fields, field);

    field = network_mysqld_proto_fielddef_new();
    field->name = g_strdup("slave delay(ms)");
    field->type = MYSQL_TYPE_STRING;
    g_ptr_array_add(fields, field);

    field = network_mysqld_proto_fielddef_new();
    field->name = g_strdup("idle_conns");
    field->type = MYSQL_TYPE_STRING;
    g_ptr_array_add(fields, field);

    field = network_mysqld_proto_fielddef_new();
    field->name = g_strdup("used_conns");
    field->type = MYSQL_TYPE_STRING;
    g_ptr_array_add(fields, field);

    field = network_mysqld_proto_fielddef_new();
    field->name = g_strdup("total_conns");
    field->type = MYSQL_TYPE_STRING;
    g_ptr_array_add(fields, field);

    if (config->has_shard_plugin) {
        field = network_mysqld_proto_fielddef_new();
        field->name = g_strdup("group");
        field->type = MYSQL_TYPE_STRING;
        g_ptr_array_add(fields, field);
    }

    GPtrArray *rows = g_ptr_array_new_with_free_func(
        (GDestroyNotify)network_mysqld_mysql_field_row_free);

    network_backends_t *bs = priv->backends;
    int len = bs->backends->len;
    int i;
    char buffer[32];
    static char *states[] = {"unknown", "up", "down", "maintaining", "deleted", "offline"};
    static char *types[] = {"unknown", "rw", "ro"};

    cetus_pid_t process_id = getpid();

    for (i = 0; i < len; i++) {
        network_backend_t *backend = bs->backends->pdata[i];
        GPtrArray *row = g_ptr_array_new_with_free_func(g_free);

        sprintf(buffer, "%d", process_id);
        g_ptr_array_add(row, g_strdup(buffer));

        sprintf(buffer, "%d", i + 1);
        g_ptr_array_add(row, g_strdup(buffer));

        g_ptr_array_add(row, g_strdup(backend->addr->name->str));
        g_ptr_array_add(row, g_strdup(states[(int) (backend->state)]));
        g_ptr_array_add(row, g_strdup(types[(int) (backend->type)]));

        sprintf(buffer, "%d", (backend->slave_delay_msec >= 10)  ? (backend->slave_delay_msec - 10) : 0);
        g_ptr_array_add(row, (backend->type == BACKEND_TYPE_RO && chas->check_slave_delay == 1) ? g_strdup(buffer) : NULL);

        sprintf(buffer, "%d", backend->pool->cur_idle_connections); 
        g_ptr_array_add(row, g_strdup(buffer));

        sprintf(buffer, "%d", backend->connected_clients); 
        g_ptr_array_add(row, g_strdup(buffer));

        sprintf(buffer, "%d", backend->pool->cur_idle_connections + backend->connected_clients); 
        g_ptr_array_add(row, g_strdup(buffer));
        if (config->has_shard_plugin) {
            g_ptr_array_add(row, backend->server_group->len ? g_strdup(backend->server_group->str) : NULL);
        }
        g_ptr_array_add(rows, row);

    }

    network_mysqld_con_send_resultset(con->client, fields, rows);

    /* Free data */
    g_ptr_array_free(rows, TRUE);
    g_ptr_array_free(fields, TRUE);
}

void admin_select_conn_details(network_mysqld_con *con)
{
    if (con->is_admin_client) {
        con->admin_read_merge = 1;
        return;
    }

    chassis *chas = con->srv;
    chassis_private *priv = chas->priv;

    int i, j, len;

    char buffer[32];
    GPtrArray *fields;
    GPtrArray *rows;
    GPtrArray *row;
    MYSQL_FIELD *field;

    GHashTable *back_user_conn_hash_table = g_hash_table_new_full(g_str_hash, 
            g_str_equal, g_free, g_table_free_all);

    network_backends_t *bs = priv->backends;
    len = bs->backends->len;

    for (i = 0; i < len; i++) {
        network_backend_t *backend = bs->backends->pdata[i];
        GHashTable *table = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
        g_hash_table_insert(back_user_conn_hash_table, g_strdup(backend->addr->name->str), table);
    }
 
    len = priv->cons->len;

    for (i = 0; i < len; i++) {
        network_mysqld_con *con = priv->cons->pdata[i];

        if (!con->client || !con->client->response) {
            continue;
        }

#ifndef SIMPLE_PARSER
        if (con->servers == NULL) {
            continue;
        }

        for (j = 0; j < con->servers->len; j++) {
            server_session_t  *pmd = g_ptr_array_index(con->servers, j);

            GHashTable *table = g_hash_table_lookup(back_user_conn_hash_table, 
                                                    pmd->backend->addr->name->str);
            if (table == NULL) {
                g_warning("%s: table is null for backend:%s", G_STRLOC, 
                          pmd->backend->addr->name->str);
                continue;
            }

            used_conns_t *total_used = g_hash_table_lookup(table,
                                                           con->client->response->username->str);
            if (total_used == NULL) {
                total_used = g_new0(used_conns_t, 1);
                g_hash_table_insert(table, 
                                    g_strdup(con->client->response->username->str), total_used);
            }
            total_used->num++;
        }
            
#else
        if (con->servers != NULL) {
            for (j = 0; j < con->servers->len; j++) {
                network_socket *sock = g_ptr_array_index(con->servers, j);
                GHashTable *table = g_hash_table_lookup(back_user_conn_hash_table, 
                                                        sock->dst->name->str);
                if (table == NULL) {
                    g_warning("%s: table is null for backend:%s", G_STRLOC, 
                              sock->dst->name->str);
                    continue;
                }

                used_conns_t *total_used = g_hash_table_lookup(table,
                                                               con->client->response->username->str);
                if (total_used == NULL) {
                    total_used = g_new0(used_conns_t, 1);
                    g_hash_table_insert(table, 
                                        g_strdup(con->client->response->username->str), total_used);
                }
                total_used->num++;
            }
        } else {
            if (con->server == NULL) {
                continue;
            }

            GHashTable *table = g_hash_table_lookup(back_user_conn_hash_table, 
                                                    con->server->dst->name->str);
            if (table == NULL) {
                g_warning("%s: table is null for backend:%s", G_STRLOC, 
                          con->server->dst->name->str);
                continue;
            }
            used_conns_t *total_used = g_hash_table_lookup(table,
                                                           con->client->response->username->str);
            if (total_used == NULL) {
                total_used = g_new0(used_conns_t, 1);
                g_hash_table_insert(table, 
                                    g_strdup(con->client->response->username->str), total_used);
            }

            total_used->num++;
        }
#endif
    }


    fields = g_ptr_array_new_with_free_func((void *) network_mysqld_proto_fielddef_free);

    field = network_mysqld_proto_fielddef_new();
    field->name = g_strdup("PID");
    field->type = MYSQL_TYPE_STRING;
    g_ptr_array_add(fields, field);


    field = network_mysqld_proto_fielddef_new();
    field->name = g_strdup("backend_ndx");
    field->type = MYSQL_TYPE_STRING;
    g_ptr_array_add(fields, field);

    field = network_mysqld_proto_fielddef_new();
    field->name = g_strdup("username");
    field->type = MYSQL_TYPE_STRING;
    g_ptr_array_add(fields, field);

    field = network_mysqld_proto_fielddef_new();
    field->name = g_strdup("idle_conns");
    field->type = MYSQL_TYPE_STRING;
    g_ptr_array_add(fields, field);

    field = network_mysqld_proto_fielddef_new();
    field->name = g_strdup("used_conns");
    field->type = MYSQL_TYPE_STRING;
    g_ptr_array_add(fields, field);

    field = network_mysqld_proto_fielddef_new();
    field->name = g_strdup("total_conns");
    field->type = MYSQL_TYPE_STRING;
    g_ptr_array_add(fields, field);

    rows = g_ptr_array_new_with_free_func((void *) network_mysqld_mysql_field_row_free);

    len = bs->backends->len;

    cetus_pid_t process_id = getpid();
    
    for (i = 0; i < len; i++) {
        network_backend_t *backend = bs->backends->pdata[i];
        
        GHashTable *table = g_hash_table_lookup(back_user_conn_hash_table, 
                backend->addr->name->str);
        if (table == NULL) {
            g_warning("%s: table is null for backend:%s", G_STRLOC, 
                    backend->addr->name->str);
            continue;
        }

        GHashTable *users = backend->pool->users;
        if (users != NULL) {
            GHashTableIter iter;
            GString *key;
            GQueue *queue;
            g_hash_table_iter_init(&iter, users);
            /* count all users' pooled connections */
            while (g_hash_table_iter_next(&iter, (void **)&key, (void **)&queue)) {
                row = g_ptr_array_new_with_free_func(g_free);

                sprintf(buffer, "%d", process_id);
                g_ptr_array_add(row, g_strdup(buffer));

                sprintf(buffer, "%d", i); 
                g_ptr_array_add(row, g_strdup(buffer));
                g_ptr_array_add(row, g_strdup(key->str));
                sprintf(buffer, "%d", queue->length);
                g_ptr_array_add(row, g_strdup(buffer));

                used_conns_t *total_used = g_hash_table_lookup(table, key->str);
                if (total_used) {
                    sprintf(buffer, "%d", total_used->num); 
                } else {
                    sprintf(buffer, "%d", 0); 
                }
                g_ptr_array_add(row, g_strdup(buffer));

                if (total_used) {
                    sprintf(buffer, "%d", queue->length + total_used->num);
                } else {
                    sprintf(buffer, "%d", queue->length);
                }
                g_ptr_array_add(row, g_strdup(buffer));

                g_ptr_array_add(rows, row);
            }
        }
    }

    network_mysqld_con_send_resultset(con->client, fields, rows);

    /* Free data */
    g_ptr_array_free(rows, TRUE);
    g_ptr_array_free(fields, TRUE);

    g_hash_table_destroy(back_user_conn_hash_table);
}

void admin_show_connectionlist(network_mysqld_con *con, int show_count)
{
    if (con->is_admin_client) {
        con->admin_read_merge = 1;
        return;
    }

    int number = 256;

    if (show_count > 0) {
        if (show_count < 256) {
            number = show_count;
        }
    }
    chassis *chas = con->srv;
    chassis_private *priv = chas->priv;
    chassis_plugin_config *config = con->config;
    int i, len;
    char buffer[32];
    GPtrArray *fields;
    GPtrArray *rows;
    GPtrArray *row;
    MYSQL_FIELD *field;

    fields = g_ptr_array_new_with_free_func((void *) network_mysqld_proto_fielddef_free);

    field = network_mysqld_proto_fielddef_new();
    field->name = g_strdup("PID");
    field->type = MYSQL_TYPE_STRING;
    g_ptr_array_add(fields, field);

    field = network_mysqld_proto_fielddef_new();
    field->name = g_strdup("ThreadID");
    field->type = MYSQL_TYPE_STRING;
    g_ptr_array_add(fields, field);

    field = network_mysqld_proto_fielddef_new();
    field->name = g_strdup("User");
    field->type = MYSQL_TYPE_STRING;
    g_ptr_array_add(fields, field);

    field = network_mysqld_proto_fielddef_new();
    field->name = g_strdup("Host");
    field->type = MYSQL_TYPE_STRING;
    g_ptr_array_add(fields, field);

    field = network_mysqld_proto_fielddef_new();
    field->name = g_strdup("db");
    field->type = MYSQL_TYPE_STRING;
    g_ptr_array_add(fields, field);

    field = network_mysqld_proto_fielddef_new();
    field->name = g_strdup("Command");
    field->type = MYSQL_TYPE_STRING;
    g_ptr_array_add(fields, field);

    field = network_mysqld_proto_fielddef_new();
    field->name = g_strdup("ProcessTime");
    field->type = MYSQL_TYPE_STRING;
    g_ptr_array_add(fields, field);

    field = network_mysqld_proto_fielddef_new();
    field->name = g_strdup("Trans");
    field->type = MYSQL_TYPE_STRING;
    g_ptr_array_add(fields, field);

    field = network_mysqld_proto_fielddef_new();
    field->name = g_strdup("PS");
    field->type = MYSQL_TYPE_STRING;
    g_ptr_array_add(fields, field);

    field = network_mysqld_proto_fielddef_new();
    field->name = g_strdup("State");
    field->type = MYSQL_TYPE_STRING;
    g_ptr_array_add(fields, field);

    if (config->has_shard_plugin) {
        field = network_mysqld_proto_fielddef_new();
        field->name = g_strdup("Xa");
        field->type = MYSQL_TYPE_STRING;
        g_ptr_array_add(fields, field);

        field = network_mysqld_proto_fielddef_new();
        field->name = g_strdup("Xid");
        field->type = MYSQL_TYPE_STRING;
        g_ptr_array_add(fields, field);
    }
    field = network_mysqld_proto_fielddef_new();
    field->name = g_strdup("Server");
    field->type = MYSQL_TYPE_STRING;
    g_ptr_array_add(fields, field);

    field = network_mysqld_proto_fielddef_new();
    field->name = g_strdup("Info");
    field->type = MYSQL_TYPE_STRING;
    g_ptr_array_add(fields, field);

    rows = g_ptr_array_new_with_free_func((void *) network_mysqld_mysql_field_row_free);

    struct timeval now;
    gettimeofday(&(now), NULL);

    len = priv->cons->len;
    int count = 0;

    cetus_pid_t process_id = getpid();

    for (i = 0; i < len; i++) {
        network_mysqld_con *con = priv->cons->pdata[i];

        if (!con->client) {
            continue;
        }

        if (count >= number) {
            break;
        }

        count++;

        row = g_ptr_array_new_with_free_func(g_free);

        sprintf(buffer, "%d", process_id);
        g_ptr_array_add(row, g_strdup(buffer));

        if (con->client->challenge)  {
            sprintf(buffer, "%d", con->client->challenge->thread_id);
            g_ptr_array_add(row, g_strdup(buffer));
        } else {
            g_ptr_array_add(row, NULL);
        }

        if (con->client->response != NULL) {
            g_ptr_array_add(row, g_strdup(con->client->response->username->str));
        } else {
            g_ptr_array_add(row, NULL);
        }
        g_ptr_array_add(row, g_strdup(con->client->src->name->str));

        g_ptr_array_add(row, g_strdup(con->client->default_db->str));

        if (con->state <= ST_READ_QUERY) {
            g_ptr_array_add(row, g_strdup("Sleep"));
            g_ptr_array_add(row, g_strdup("0"));
        } else {
            g_ptr_array_add(row, g_strdup("Query"));
            int diff = (now.tv_sec - con->req_recv_time.tv_sec) * 1000;
            diff += (now.tv_usec - con->req_recv_time.tv_usec) / 1000;
            sprintf(buffer, "%d", diff);
            g_ptr_array_add(row, g_strdup(buffer));
        }
        g_ptr_array_add(row, g_strdup(con->is_in_transaction ? "Y" : "N"));
        g_ptr_array_add(row, g_strdup(con->is_prepared ? "Y" : "N"));

        g_ptr_array_add(row, g_strdup(network_mysqld_con_st_name(con->state)));

        if (config->has_shard_plugin) {
            g_ptr_array_add(row, g_strdup(get_conn_xa_state_name(con->dist_tran_state)));
            if (con->dist_tran) {
                snprintf(buffer, sizeof(buffer), "%lld", con->xa_id);
                g_ptr_array_add(row, g_strdup(buffer));
            } else {
                g_ptr_array_add(row, NULL);
            }
        }

        if (con->servers != NULL) {
            int j;
            GString *servers = g_string_new(NULL);
            for (j = 0; j < con->servers->len; j++) {
                server_session_t  *pmd = g_ptr_array_index(con->servers, j);
                if (pmd && pmd->server) {
                    if (pmd->server->src) {
                        g_string_append_len(servers, S(pmd->server->src->name));
                        char *delim = "->";
                        g_string_append_len(servers, delim, strlen(delim));
                    } 
                    g_string_append_len(servers, S(pmd->server->dst->name));
                    g_string_append_c(servers, ' ');
                }
            }
            g_ptr_array_add(row, g_strdup(servers->str));
            g_string_free(servers, TRUE);

        } else {
            if (con->server != NULL) {
                GString *server = g_string_new(NULL);
                if (con->server->src) {
                    g_string_append_len(server, S(con->server->src->name));
                    char *delim = "->";
                    g_string_append_len(server, delim, strlen(delim));
                }
                g_string_append_len(server, S(con->server->dst->name));
                g_ptr_array_add(row, g_strdup(server->str));
                g_string_free(server, TRUE);
            } else {
                g_ptr_array_add(row, NULL);
            }
        }

        if (con->orig_sql->len) {
            if (con->state == ST_READ_QUERY) {
                g_ptr_array_add(row, NULL);
            } else {
                g_ptr_array_add(row, g_strdup(con->orig_sql->str));
            }
        } else {
            g_ptr_array_add(row, NULL);
        }

        g_ptr_array_add(rows, row);
    }

    network_mysqld_con_send_resultset(con->client, fields, rows);

    g_ptr_array_free(rows, TRUE);
    g_ptr_array_free(fields, TRUE);
}

#define MAKE_FIELD_DEF_1_COL(fields, col_name)  \
    do {\
    MYSQL_FIELD *field = network_mysqld_proto_fielddef_new();\
    field->name = g_strdup((col_name));                      \
    field->type = FIELD_TYPE_VAR_STRING;\
    g_ptr_array_add(fields, field);         \
    }while(0)

#define MAKE_FIELD_DEF_2_COL(fields, col1_name, col2_name) \
    do {\
    MYSQL_FIELD *field = network_mysqld_proto_fielddef_new();\
    field->name = g_strdup((col1_name));                      \
    field->type = FIELD_TYPE_VAR_STRING;\
    g_ptr_array_add(fields, field);         \
    field = network_mysqld_proto_fielddef_new();     \
    field->name = g_strdup((col2_name));                      \
    field->type = FIELD_TYPE_VAR_STRING;\
    g_ptr_array_add(fields, field);         \
    }while(0)

#define MAKE_FIELD_DEF_3_COL(fields, col1_name, col2_name, col3_name)   \
    do {\
    MYSQL_FIELD *field = network_mysqld_proto_fielddef_new();\
    field->name = g_strdup((col1_name));                      \
    field->type = FIELD_TYPE_VAR_STRING;\
    g_ptr_array_add(fields, field);         \
    field = network_mysqld_proto_fielddef_new();     \
    field->name = g_strdup((col2_name));                      \
    field->type = FIELD_TYPE_VAR_STRING;\
    g_ptr_array_add(fields, field);         \
    field = network_mysqld_proto_fielddef_new();     \
    field->name = g_strdup((col3_name));                      \
    field->type = FIELD_TYPE_VAR_STRING;\
    g_ptr_array_add(fields, field);  \
    }while(0)

#define MAKE_FIELD_DEF_4_COL(fields, col1_name, col2_name, col3_name, col4_name)   \
    do {\
    MYSQL_FIELD *field = network_mysqld_proto_fielddef_new();\
    field->name = g_strdup((col1_name));                      \
    field->type = FIELD_TYPE_VAR_STRING;\
    g_ptr_array_add(fields, field);         \
    field = network_mysqld_proto_fielddef_new();     \
    field->name = g_strdup((col2_name));                      \
    field->type = FIELD_TYPE_VAR_STRING;\
    g_ptr_array_add(fields, field);         \
    field = network_mysqld_proto_fielddef_new();     \
    field->name = g_strdup((col3_name));                      \
    field->type = FIELD_TYPE_VAR_STRING;\
    g_ptr_array_add(fields, field);  \
    field = network_mysqld_proto_fielddef_new();     \
    field->name = g_strdup((col4_name));                      \
    field->type = FIELD_TYPE_VAR_STRING;\
    g_ptr_array_add(fields, field);  \
    }while(0)


#define APPEND_ROW_1_COL(rows, row_data) \
    do {\
    GPtrArray* row = g_ptr_array_new();\
    g_ptr_array_add(row, (row_data));  \
    g_ptr_array_add(rows, row);\
    }while(0)

#define APPEND_ROW_2_COL(rows, col1, col2)           \
    do {\
    GPtrArray* row = g_ptr_array_new();\
    g_ptr_array_add(row, (col1));  \
    g_ptr_array_add(row, (col2));  \
    g_ptr_array_add(rows, row);\
    }while(0)

#define APPEND_ROW_3_COL(rows, col1, col2, col3)    \
    do {\
    GPtrArray *row = g_ptr_array_new();\
    g_ptr_array_add(row, (col1));  \
    g_ptr_array_add(row, (col2));  \
    g_ptr_array_add(row, (col3));  \
    g_ptr_array_add(rows, row);\
    }while(0)

#define APPEND_ROW_4_COL(rows, col1, col2, col3, col4)    \
    do {\
    GPtrArray *row = g_ptr_array_new();\
    g_ptr_array_add(row, (col1));  \
    g_ptr_array_add(row, (col2));  \
    g_ptr_array_add(row, (col3));  \
    g_ptr_array_add(row, (col4));  \
    g_ptr_array_add(rows, row);\
    }while(0)

void admin_acl_show_rules(network_mysqld_con *con, gboolean is_white)
{
    if (con->is_admin_client) {
        con->ask_one_worker = 1;
        con->admin_read_merge = 1;
        return;
    }
    GPtrArray *fields = network_mysqld_proto_fielddefs_new();
    MAKE_FIELD_DEF_2_COL(fields, "User", "Host");

    GPtrArray *rows = g_ptr_array_new_with_free_func(
        (void *)network_mysqld_mysql_field_row_free);

    chassis_private *priv = con->srv->priv;
    GList* l = is_white ? priv->acl->whitelist : priv->acl->blacklist;
    for (; l; l = l->next) {
        struct cetus_acl_entry_t* entry = l->data;
        APPEND_ROW_2_COL(rows, entry->username, entry->host);
    }

    network_mysqld_con_send_resultset(con->client, fields, rows);

    g_ptr_array_free(rows, TRUE);
    network_mysqld_proto_fielddefs_free(fields);
}

void admin_acl_add_rule(network_mysqld_con *con, gboolean is_white, char *addr)
{
    if (con->is_admin_client) {
        return;
    }

    chassis *chas = con->srv;
    int affected = cetus_acl_add_rule_str(chas->priv->acl,
                                         is_white?ACL_WHITELIST:ACL_BLACKLIST, addr);
    if(chas->config_manager->type == CHASSIS_CONF_MYSQL) {
        network_mysqld_con_send_ok_full(con->client, affected,
                                        0, SERVER_STATUS_AUTOCOMMIT, 0);        
    } else {
        gint ret = CHANGE_SAVE_ERROR;
        gint effected_rows = 0;
        if (affected)
            ret = save_setting(chas, &effected_rows);
        send_result(con->client, ret, affected);
    }
}

void admin_acl_delete_rule(network_mysqld_con *con, gboolean is_white, char *addr)
{
    if (con->is_admin_client) {
        return;
    }
    chassis *chas = con->srv;
    int affected = cetus_acl_delete_rule_str(chas->priv->acl,
                                             is_white?ACL_WHITELIST:ACL_BLACKLIST, addr);
    if(chas->config_manager->type == CHASSIS_CONF_MYSQL) {
        network_mysqld_con_send_ok_full(con->client, affected,
                                        0, SERVER_STATUS_AUTOCOMMIT, 0);
    } else {
        gint ret = CHANGE_SAVE_ERROR;
        gint effected_rows = 0;
        if (affected)
            ret = save_setting(chas, &effected_rows);
        send_result(con->client, ret, affected);
    }
}

/* only match % wildcard, case insensitive */
static gboolean sql_pattern_like(const char* pattern, const char* string)
{
    if (!pattern || pattern[0] == '\0')
        return TRUE;
    char *glob = g_strdup(pattern);
    int i = 0;
    for (i=0; glob[i]; ++i) {
        if (glob[i] == '%') glob[i] = '*';
        glob[i] = tolower(glob[i]);
    }
    char* lower_str = g_ascii_strdown(string, -1);
    gboolean rc = g_pattern_match_simple(glob, lower_str);
    g_free(glob);
    g_free(lower_str);
    return rc;
}

/* returned list must be freed */
static GList* admin_get_all_options(chassis* chas)
{
    GList* options = g_list_copy(chas->options->options); /* shallow copy */
    return options;
}

void admin_show_variables(network_mysqld_con* con, const char* like)
{
    if (con->is_admin_client) {
        con->ask_one_worker = 1; 
        con->admin_read_merge = 1;
        return;
    }

    const char* pattern = like ? like : "%";
    GList *options = admin_get_all_options(con->srv);

    GPtrArray *fields = network_mysqld_proto_fielddefs_new();
    MAKE_FIELD_DEF_3_COL(fields, "Variable_name", "Value", "Property");

    GPtrArray *rows = g_ptr_array_new_with_free_func((void *)network_mysqld_mysql_field_row_free);

    GList *freelist = NULL;
    GList *l = NULL;
    for (l = options; l; l = l->next) {
        chassis_option_t *opt = l->data;
        /* just support these for now */
        if (sql_pattern_like(pattern, opt->long_name)) {
            struct external_param param = {0};
            param.chas = con->srv;
            param.opt_type = opt->opt_property;
            char *value = opt->show_hook != NULL? opt->show_hook(&param) : NULL;
            if(NULL == value) {
                continue;
            }
            freelist = g_list_append(freelist, value);
            APPEND_ROW_3_COL(rows, (char *)opt->long_name, value,
                    (CAN_ASSIGN_OPTS_PROPERTY(opt->opt_property)? "Dynamic" : "Static"));
        }
    }
    network_mysqld_con_send_resultset(con->client, fields, rows);

    network_mysqld_proto_fielddefs_free(fields);
    g_ptr_array_free(rows, TRUE);
    g_list_free_full(freelist, g_free);
    g_list_free(options);
}

void admin_show_status(network_mysqld_con* con, const char* like)
{
    if (con->is_admin_client) {
        con->admin_read_merge = 1;
        return;
    }

    const char* pattern = like ? like : "%";
    cetus_variable_t* variables = con->srv->priv->stats_variables;

    GPtrArray *fields = network_mysqld_proto_fielddefs_new();
    MAKE_FIELD_DEF_3_COL(fields, "PID", "Variable_name", "Value");

    GPtrArray *rows = g_ptr_array_new_with_free_func(
        (void*)network_mysqld_mysql_field_row_free);

    char buffer[32];
    cetus_pid_t process_id = getpid();
    sprintf(buffer, "%d", process_id);

    GList *freelist = NULL;
    int i = 0;
    for (i = 0; variables[i].name; ++i) {
        if (sql_pattern_like(pattern, variables[i].name)) {
            char* value = cetus_variable_get_value_str(&variables[i]);
            freelist = g_list_append(freelist, value);
            APPEND_ROW_3_COL(rows, g_strdup(buffer), variables[i].name, value);
        }
    }
    network_mysqld_con_send_resultset(con->client, fields, rows);

    network_mysqld_proto_fielddefs_free(fields);
    g_ptr_array_free(rows, TRUE);
    g_list_free_full(freelist, g_free);
}

void admin_set_reduce_conns(network_mysqld_con* con, int mode)
{
    if (con->is_admin_client) {
        return;
    }

    int affected = 0;
    if (con->srv->is_reduce_conns != mode) {
        con->srv->is_reduce_conns = mode;
        affected = 1;
    }
    if(con->srv->config_manager->type == CHASSIS_CONF_MYSQL) {
        network_mysqld_con_send_ok_full(con->client, affected,
                                        0, SERVER_STATUS_AUTOCOMMIT, 0);
    } else {
        gint ret = CHANGE_SAVE_ERROR;
        gint effected_rows = 0;
        if (affected)
            ret = save_setting(con->srv, &effected_rows);
        send_result(con->client, ret, affected);
    }
}

void admin_set_maintain(network_mysqld_con* con, int mode)
{
    if (con->is_admin_client) {
        return;
    }

    int affected = 0;
    if (con->srv->maintain_close_mode != mode) {
        con->srv->maintain_close_mode = mode;
        affected = 1;
    }
    network_mysqld_con_send_ok_full(con->client, affected, 0,2,0);
}

void admin_show_maintain(network_mysqld_con* con)
{
    if (con->is_admin_client) {
        con->admin_read_merge = 1;
        return;
    }

    char buffer[32];
    cetus_pid_t process_id = getpid();
    sprintf(buffer, "%d", process_id);

    GPtrArray *fields = network_mysqld_proto_fielddefs_new();
    GPtrArray *rows = g_ptr_array_new_with_free_func((void *)network_mysqld_mysql_field_row_free);
    MAKE_FIELD_DEF_2_COL(fields, "PID", "Cetus maintain status");
    if(con->srv->maintain_close_mode == 1) {
        APPEND_ROW_2_COL(rows, g_strdup(buffer), "true");
    } else {
        APPEND_ROW_2_COL(rows, g_strdup(buffer), "false");
    }
    network_mysqld_con_send_resultset(con->client, fields, rows);
    network_mysqld_proto_fielddefs_free(fields);
    g_ptr_array_free(rows, TRUE);
}

void admin_select_version(network_mysqld_con* con)
{
    con->direct_answer = 1;

    GPtrArray* fields = network_mysqld_proto_fielddefs_new();

    MAKE_FIELD_DEF_1_COL(fields, "cetus version");

    GPtrArray *rows = g_ptr_array_new_with_free_func(
        (void*)network_mysqld_mysql_field_row_free);

    APPEND_ROW_1_COL(rows, PLUGIN_VERSION);

    network_mysqld_con_send_resultset(con->client, fields, rows);

    network_mysqld_proto_fielddefs_free(fields);
    g_ptr_array_free(rows, TRUE);
}

void admin_select_connection_stat(network_mysqld_con* con, int backend_ndx, char *user)
{
    if (con->is_admin_client) {
        con->admin_read_merge = 1;
        return;
    }

    GPtrArray* fields = network_mysqld_proto_fielddefs_new();

    MAKE_FIELD_DEF_1_COL(fields, "connection_num");

    GPtrArray *rows = g_ptr_array_new_with_free_func(
        (void*)network_mysqld_mysql_field_row_free);

    char* numstr = NULL;
    chassis_private *g = con->srv->priv;
    backend_ndx -= 1; /* index in sql start from 1, not 0 */
    if (backend_ndx >= 0 && backend_ndx < network_backends_count(g->backends)) {
        network_backend_t* backend = network_backends_get(g->backends, backend_ndx);
        GString* user_name = g_string_new(user);

        /*TODO: if robbed, conns is not for user_name */
        GQueue* conns = network_connection_pool_get_conns(backend->pool, user_name, NULL);
        if (conns) {
            numstr = g_strdup_printf("%d", conns->length);
        }
        g_string_free(user_name, TRUE);
    }
    if (numstr) {
        APPEND_ROW_1_COL(rows, numstr);
    } else {
        APPEND_ROW_1_COL(rows, "0");
    }

    network_mysqld_con_send_resultset(con->client, fields, rows);

    network_mysqld_proto_fielddefs_free(fields);
    g_ptr_array_free(rows, TRUE);
    if (numstr)
        g_free(numstr);
}

static enum cetus_pwd_type password_type(char* table)
{
    if (strcmp(table, "user_pwd")==0) {
        return CETUS_SERVER_PWD;
    } else if (strcmp(table, "app_user_pwd")==0) {
        return CETUS_CLIENT_PWD;
    } else {
        g_assert(0);
    }
}

void admin_select_user_password(network_mysqld_con* con, char* from_table, char *user)
{
    if (con->is_admin_client) {
        con->ask_one_worker = 1;
        con->admin_read_merge = 1;
        return;
    }

    chassis_private *g = con->srv->priv;
    enum cetus_pwd_type pwd_type = password_type(from_table);
    GPtrArray* fields = network_mysqld_proto_fielddefs_new();

    MAKE_FIELD_DEF_2_COL(fields, "user", "password(sha1)");

    GPtrArray *rows = g_ptr_array_new_with_free_func(
        (void*)network_mysqld_mysql_field_row_free);

    if (user) { /* one user */
        GString* hashpwd = g_string_new(0);
        cetus_users_get_hashed_pwd(g->users, user, pwd_type, hashpwd);
        char* pwdhex = NULL;
        if (hashpwd->len > 0) {
            pwdhex = g_malloc0(hashpwd->len * 2 + 10);
            bytes_to_hex_str(hashpwd->str, hashpwd->len, pwdhex);
            APPEND_ROW_2_COL(rows, user, pwdhex);
        }
        network_mysqld_con_send_resultset(con->client, fields, rows);
        network_mysqld_proto_fielddefs_free(fields);
        g_ptr_array_free(rows, TRUE);
        g_string_free(hashpwd, TRUE);
        if (pwdhex)
            g_free(pwdhex);
    } else { /* all users */
        GList* strings_to_free = NULL;
        GHashTableIter iter;
        char* username = NULL;
        GString* hashpwd = g_string_new(0);
        char* hack = NULL; /* don't use value directly */
        g_hash_table_iter_init(&iter, g->users->records);
        while (g_hash_table_iter_next(&iter, (gpointer*)&username, (gpointer*)&hack)) {
            cetus_users_get_hashed_pwd(g->users, username, pwd_type, hashpwd);
            char* pwdhex = NULL;
            if (hashpwd->len > 0) {
                pwdhex = g_malloc0(hashpwd->len * 2 + 10);
                bytes_to_hex_str(hashpwd->str, hashpwd->len, pwdhex);
                strings_to_free = g_list_append(strings_to_free, pwdhex);
            }
            APPEND_ROW_2_COL(rows, username, pwdhex);
        }
        network_mysqld_con_send_resultset(con->client, fields, rows);
        network_mysqld_proto_fielddefs_free(fields);
        g_ptr_array_free(rows, TRUE);
        g_string_free(hashpwd, TRUE);
        if (strings_to_free)
            g_list_free_full(strings_to_free, g_free);
    }
}

/* update or insert */
void admin_update_user_password(network_mysqld_con* con, char *from_table,
                                      char *user, char *password)
{
    if (con->is_admin_client) {
        return;
    }

    chassis_private *g = con->srv->priv;
    enum cetus_pwd_type pwd_type = password_type(from_table);
    gboolean affected = cetus_users_update_record(g->users, user, password, pwd_type);
    if (affected)
        cetus_users_write_json(g->users);
    network_mysqld_con_send_ok_full(con->client, affected?1:0, 0,
                                    SERVER_STATUS_AUTOCOMMIT, 0);
}

void admin_delete_user_password(network_mysqld_con* con, char* user)
{
    if (con->is_admin_client) {
        return;
    }

    chassis_private *g = con->srv->priv;
    gboolean affected = cetus_users_delete_record(g->users, user);
    if (affected)
        cetus_users_write_json(g->users);
    network_mysqld_con_send_ok_full(con->client, affected?1:0, 0,
                                    SERVER_STATUS_AUTOCOMMIT, 0);
}

#define ERROR_PARAM -1

static backend_type_t backend_type(const char* str)
{
    backend_type_t type = ERROR_PARAM;
    if (strcasecmp(str, "ro")==0)
        type = BACKEND_TYPE_RO;
    else if (strcasecmp(str, "rw")==0)
        type = BACKEND_TYPE_RW;
    else if (strcasecmp(str, "unknown")==0)
        type = BACKEND_TYPE_UNKNOWN;
    return type;
}

static backend_state_t backend_state(const char* str)
{
    backend_state_t state = ERROR_PARAM;
    if (strcasecmp(str, "up")==0)
        state = BACKEND_STATE_UP;
    else if (strcasecmp(str, "down")==0)
        state = BACKEND_STATE_DOWN;
    else if (strcasecmp(str, "maintaining")==0)
        state = BACKEND_STATE_MAINTAINING;
    else if (strcasecmp(str, "unknown") == 0)
        state = BACKEND_STATE_UNKNOWN;
    return state;
}

void admin_insert_backend(network_mysqld_con* con, char *addr, char *type, char *state)
{
    if (con->is_admin_client) {
        return;
    }

    chassis_private *g = con->srv->priv;
  
    int ret = network_backends_add(g->backends, addr,
                                        backend_type(type),
                                        backend_state(state), con->srv);
    switch (ret) {
        case BACKEND_OPERATE_SUCCESS:
        {
            if(con->srv->config_manager->type == CHASSIS_CONF_MYSQL) {
                network_mysqld_con_send_ok_full(con->client, 1, 0,
                                                SERVER_STATUS_AUTOCOMMIT, 0);
            } else {
                gint effected_rows = 0;
                ret = save_setting(con->srv, &effected_rows);
                send_result(con->client, ret, 1);
            }
            break;
        }
        case BACKEND_OPERATE_NETERR:
        {
            network_mysqld_con_send_error(con->client, C("get network address failed"));
            break;
        }
        case BACKEND_OPERATE_DUPLICATE:
        {
            network_mysqld_con_send_error(con->client, C("backend is already known"));
            break;
        }
        case BACKEND_OPERATE_2MASTER:
        {
            network_mysqld_con_send_error(con->client, C("rw node is already exists，only one rw node is allowed"));
            break;
        }
    }
}

void admin_update_backend(network_mysqld_con* con, GList* equations,
                          char *cond_key, char *cond_val)
{
    if (con->is_admin_client) {
        return;
    }

    char* type_str = NULL;
    char* state_str = NULL;

    GList* l; /* list = [key1, val1, key2, val2, ...]*/
    for (l = equations; l && l->next; l=l->next, l=l->next) {
        char* key = l->data;
        char* val = l->next->data;
        if (strcasecmp(key, "type")==0) {
            type_str = val;
        } else if (strcasecmp(key, "state")==0) {
            state_str = val;
        } else {
            network_mysqld_con_send_error(con->client, C("parameter error"));
            return;
        }
    }

    chassis_private *g = con->srv->priv;
    int backend_ndx = -1;
    if (strcasecmp(cond_key, "backend_ndx") == 0) {
        backend_ndx = atoi(cond_val);
        backend_ndx -= 1; /* index in SQL start from 1, not 0 */
    } else if (strcasecmp(cond_key, "address")==0) {
        backend_ndx = network_backends_find_address(g->backends, cond_val);
    } else {
        network_mysqld_con_send_error(con->client, C("parameter error"));
        return;
    }

    network_backend_t* bk = network_backends_get(g->backends, backend_ndx);
    if (!bk) {
        network_mysqld_con_send_error(con->client, C("no such backend"));
        return;
    }

    if (type_str && backend_type(type_str) == BACKEND_TYPE_RW &&
            network_backend_check_available_rw(g->backends, bk->server_group))
    {
        if (backend_type(type_str) == bk->type) {
            network_mysqld_con_send_ok_full(con->client, 0, 0,
                                                SERVER_STATUS_AUTOCOMMIT, 0);
        } else {
            network_mysqld_con_send_error(con->client, C("rw node is already exists，only one rw node is allowed"));
        }
        return;
    }

    int type = type_str ? backend_type(type_str) : bk->type;
    int state = state_str ? backend_state(state_str) : bk->state;
    if (type == ERROR_PARAM || state == ERROR_PARAM) {
        network_mysqld_con_send_error(con->client, C("parameter error"));
        return;
    }
    int affected = (network_backends_modify(g->backends, backend_ndx, type, state, NO_PREVIOUS_STATE)==0)?1:0;
    if(con->srv->config_manager->type == CHASSIS_CONF_MYSQL) {
        network_mysqld_con_send_ok_full(con->client, affected,
                                        0, SERVER_STATUS_AUTOCOMMIT, 0);
    } else {
        gint ret = CHANGE_SAVE_ERROR;
        gint effected_rows = 0;
        if (affected)
            ret = save_setting(con->srv, &effected_rows);
        send_result(con->client, ret, affected);
    }
}

void admin_delete_backend(network_mysqld_con* con, char *key, char *val)
{
    if (con->is_admin_client) {
        return;
    }

    chassis_private *g = con->srv->priv;
    int backend_ndx = -1;
    if (strcasecmp(key, "backend_ndx")==0) {
        backend_ndx = atoi(val);
        backend_ndx -= 1; /* index in SQL start from 1, not 0 */
    } else if (strcasecmp(key, "address")==0) {
        backend_ndx = network_backends_find_address(g->backends, val);
    } else {
        network_mysqld_con_send_error(con->client, C("parameter error"));
        return;
    }

    if (backend_ndx >= 0 && backend_ndx < network_backends_count(g->backends)) {
        network_backends_remove(g->backends, backend_ndx);/*TODO: just change state? */
        if(con->srv->config_manager->type == CHASSIS_CONF_MYSQL) {
            network_mysqld_con_send_ok_full(con->client, 1, 0,
                                            SERVER_STATUS_AUTOCOMMIT, 0);
        } else {
            gint effected_rows = 0;
            gint ret = save_setting(con->srv, &effected_rows);
            send_result(con->client, ret, 1);
        }
    } else {
        network_mysqld_con_send_ok_full(con->client, 0, 0,
                                        SERVER_STATUS_AUTOCOMMIT, 0);
    }
}

static void admin_supported_stats(network_mysqld_con* con)
{
    con->direct_answer = 1;
    GPtrArray* fields = network_mysqld_proto_fielddefs_new();
    MAKE_FIELD_DEF_1_COL(fields, "name");
    GPtrArray *rows = g_ptr_array_new_with_free_func(
        (void*)network_mysqld_mysql_field_row_free);
    APPEND_ROW_1_COL(rows, "client_query");
    APPEND_ROW_1_COL(rows, "proxyed_query");
    APPEND_ROW_1_COL(rows, "reset");
    APPEND_ROW_1_COL(rows, "query_time_table");
    APPEND_ROW_1_COL(rows, "server_query_details");
    APPEND_ROW_1_COL(rows, "query_wait_table");
    network_mysqld_con_send_resultset(con->client, fields, rows);
    network_mysqld_proto_fielddefs_free(fields);
    g_ptr_array_free(rows, TRUE);
}

void admin_get_stats(network_mysqld_con* con, char* p)
{
    if (!p) { /* just "stats get", no argument */
        admin_supported_stats(con);
        return;
    }
    
    if (con->is_admin_client) {
        con->admin_read_merge = 1;
        return;
    }
    char buffer[32];
    cetus_pid_t process_id = getpid();
    sprintf(buffer, "%d", process_id);

    GPtrArray* fields = network_mysqld_proto_fielddefs_new();
    MAKE_FIELD_DEF_3_COL(fields, "PID", "name", "value");
    GPtrArray *rows = g_ptr_array_new_with_free_func(
        (void*)network_mysqld_mysql_field_row_free);
    chassis *chas = con->srv;
    query_stats_t* stats = &(chas->query_stats);
    char buf1[32] = {0};
    char buf2[32] = {0};
    if (strcasecmp(p, "client_query") == 0) {
        snprintf(buf1, 32, "%lu", stats->client_query.ro);
        snprintf(buf2, 32, "%lu", stats->client_query.rw);
        APPEND_ROW_3_COL(rows, g_strdup(buffer), "client_query.ro", buf1);
        APPEND_ROW_3_COL(rows, g_strdup(buffer), "client_query.rw", buf2);
    } else if (strcasecmp(p, "proxyed_query") == 0) {
        snprintf(buf1, 32, "%lu", stats->proxyed_query.ro);
        snprintf(buf2, 32, "%lu", stats->proxyed_query.rw);
        APPEND_ROW_3_COL(rows, g_strdup(buffer), "proxyed_query.ro", buf1);
        APPEND_ROW_3_COL(rows, g_strdup(buffer), "proxyed_query.rw", buf2);
    } else if (strcasecmp(p, "query_time_table") == 0) {
        int i = 0;
        for (i; i < MAX_QUERY_TIME && stats->query_time_table[i]; ++i) {
            GPtrArray* row = g_ptr_array_new_with_free_func(g_free);
            g_ptr_array_add(row, g_strdup(buffer));
            g_ptr_array_add(row, g_strdup_printf("query_time_table.%d", i+1));
            g_ptr_array_add(row, g_strdup_printf("%lu", stats->query_time_table[i]));
            g_ptr_array_add(rows, row);
        }
    } else if (strcasecmp(p, "query_wait_table") == 0) {
        int i = 0;
        for (i; i < MAX_WAIT_TIME && stats->query_wait_table[i]; ++i) {
            GPtrArray* row = g_ptr_array_new_with_free_func(g_free);
            g_ptr_array_add(row, g_strdup(buffer));
            g_ptr_array_add(row, g_strdup_printf("query_wait_table.%d", i+1));
            g_ptr_array_add(row, g_strdup_printf("%lu", stats->query_wait_table[i]));
            g_ptr_array_add(rows, row);
        }
    } else if (strcasecmp(p, "server_query_details") == 0) {
        int i = 0;
        for (i; i < network_backends_count(chas->priv->backends)
                 && i < MAX_SERVER_NUM; ++i) {
            GPtrArray* row = g_ptr_array_new_with_free_func(g_free);
            g_ptr_array_add(row, g_strdup(buffer));
            g_ptr_array_add(row, g_strdup_printf("server_query_details.%d.ro", i+1));
            g_ptr_array_add(row, g_strdup_printf("%lu", stats->server_query_details[i].ro));
            g_ptr_array_add(rows, row);
            row = g_ptr_array_new_with_free_func(g_free);
            g_ptr_array_add(row, g_strdup(buffer));
            g_ptr_array_add(row, g_strdup_printf("server_query_details.%d.rw", i+1));
            g_ptr_array_add(row, g_strdup_printf("%lu", stats->server_query_details[i].rw));
            g_ptr_array_add(rows, row);
        }
    } else if (strcasecmp(p, "reset") == 0) {
        APPEND_ROW_3_COL(rows, g_strdup(buffer), "reset", "0");
    } else {
        APPEND_ROW_3_COL(rows, g_strdup(buffer), (char*)p, (char*)p);
    }
    network_mysqld_con_send_resultset(con->client, fields, rows);

    network_mysqld_proto_fielddefs_free(fields);
    g_ptr_array_free(rows, TRUE);
}

static void admin_supported_config(network_mysqld_con* con)
{
    con->direct_answer = 1;
    GPtrArray* fields = network_mysqld_proto_fielddefs_new();
    MAKE_FIELD_DEF_1_COL(fields, "name");
    GPtrArray *rows = g_ptr_array_new_with_free_func(
        (void*)network_mysqld_mysql_field_row_free);
    APPEND_ROW_1_COL(rows, "common");
    APPEND_ROW_1_COL(rows, "pool");
    network_mysqld_con_send_resultset(con->client, fields, rows);
    network_mysqld_proto_fielddefs_free(fields);
    g_ptr_array_free(rows, TRUE);
}

void admin_get_config(network_mysqld_con* con, char* p)
{
    if (con->is_admin_client) {
        con->admin_read_merge = 1;
        return;
    }

    if (!p) { /* just "config get", no argument */
        admin_supported_config(con);
        return;
    }

    char buffer[32];
    cetus_pid_t process_id = getpid();
    sprintf(buffer, "%d", process_id);

    GPtrArray* fields = network_mysqld_proto_fielddefs_new();
    MAKE_FIELD_DEF_3_COL(fields, "PID", "name", "value");
    GPtrArray *rows = g_ptr_array_new_with_free_func(
        (void*)network_mysqld_mysql_field_row_free);
    chassis *chas = con->srv;
    char buf1[32] = {0}, buf2[32] = {0};
    char buf3[32] = {0}, buf4[32] = {0};
    if (strcasecmp(p, "common") == 0) {
        snprintf(buf1, 32, "%d", chas->check_slave_delay);
        snprintf(buf2, 32, "%f", chas->slave_delay_down_threshold_sec);
        snprintf(buf3, 32, "%f", chas->slave_delay_recover_threshold_sec);
        snprintf(buf4, 32, "%d", chas->long_query_time);
        APPEND_ROW_3_COL(rows, g_strdup(buffer), "common.check_slave_delay", buf1);
        APPEND_ROW_3_COL(rows, g_strdup(buffer), "common.slave_delay_down_threshold_sec", buf2);
        APPEND_ROW_3_COL(rows, g_strdup(buffer), "common.slave_delay_recover_threshold_sec", buf3);
        APPEND_ROW_3_COL(rows, g_strdup(buffer), "common.long_query_time", buf4);
    } else if (strcasecmp(p, "pool") == 0) {
        snprintf(buf1, 32, "%d", chas->mid_idle_connections);
        snprintf(buf2, 32, "%d", chas->max_idle_connections);
        snprintf(buf3, 32, "%lld", chas->max_resp_len);
        snprintf(buf4, 32, "%d", chas->master_preferred);
        APPEND_ROW_3_COL(rows, g_strdup(buffer), "pool.default_pool_size", buf1);
        APPEND_ROW_3_COL(rows, g_strdup(buffer), "pool.max_pool_size", buf2);
        APPEND_ROW_3_COL(rows, g_strdup(buffer), "pool.max_resp_len", buf3);
        APPEND_ROW_3_COL(rows, g_strdup(buffer), "pool.master_preferred", buf4);
    } else {
        APPEND_ROW_3_COL(rows, g_strdup(buffer), (char*)p, (char*)p);
    }
    network_mysqld_con_send_resultset(con->client, fields, rows);

    network_mysqld_proto_fielddefs_free(fields);
    g_ptr_array_free(rows, TRUE);
}

void admin_set_config(network_mysqld_con* con, char* key, char* value)
{
    if (con->is_admin_client) {
        return;
    }

    GList *options = admin_get_all_options(con->srv);
    chassis_option_t* opt = chassis_options_get(options, key);
    if (!opt) {
        char msg[128] = {0};
        snprintf(msg, sizeof(msg), "no such variable: %s", key);
        network_mysqld_con_send_error(con->client, L(msg));
        g_list_free(options);
        return;
    }
    struct external_param param = {0};
    param.chas = con->srv;
    param.opt_type = opt->opt_property;
    int ret = opt->assign_hook != NULL? opt->assign_hook(value, &param) : ASSIGN_NOT_SUPPORT;

    g_list_free(options);

    if(0 == ret && !chassis_config_set_remote_options(con->srv->config_manager, key, value)) {
        network_mysqld_con_send_error(con->client,C("Variable is set locally but cannot replace remote settings"));
        return;
    }

    if(0 == ret) {
        if(con->srv->config_manager->type == CHASSIS_CONF_MYSQL) {
            network_mysqld_con_send_ok_full(con->client, 1, 0, SERVER_STATUS_AUTOCOMMIT, 0);
        } else {
            gint effected_rows = 0;
            gint save_ret = save_setting(con->srv, &effected_rows);
            send_result(con->client, save_ret, 1);
        }
    } else if(ASSIGN_NOT_SUPPORT == ret){
        network_mysqld_con_send_error_full(con->client, C("Variable cannot be set dynamically"), 1065, "28000");
    } else if(ASSIGN_VALUE_INVALID == ret){
        network_mysqld_con_send_error_full(con->client, C("Value is illegal"), 1065, "28000");
    } else {
        network_mysqld_con_send_error_full(con->client, C("You have an error in your SQL syntax"), 1065, "28000");
    }
}

static void admin_reload_settings(network_mysqld_con* con)
{
    GList *options = admin_get_all_options(con->srv);
    gint ret = chassis_config_reload_options(con->srv->config_manager);
    if (ret == -1) {
        network_mysqld_con_send_error(con->client,
                    C("Can't connect to remote or can't get config"));
                return;
    }
    if (ret == -2) {
        network_mysqld_con_send_error(con->client,
            C("Can't load options, only support remote config"));
        return;
    }
    GHashTable *opts_table = chassis_config_get_options(con->srv->config_manager);

    int affected_rows = 0;
    GHashTableIter iter;
    char* key = NULL;
    char* value = NULL; /* don't use value directly */
    g_hash_table_iter_init(&iter, opts_table);
    while (g_hash_table_iter_next(&iter, (gpointer*)&key, (gpointer*)&value)) {
        chassis_option_t* opt = chassis_options_get(options, key);
        if (!opt) {
            g_warning(G_STRLOC "no such variable: %s", key);
            continue;
        }
        struct external_param param = {0};
        param.chas = con->srv;
        param.opt_type = opt->opt_property;
        int ret = opt->assign_hook != NULL?
            opt->assign_hook(value, &param) : ASSIGN_NOT_SUPPORT;
        if (ret == 0) {
            affected_rows += 1;
        }
    }
    g_list_free(options);
    network_mysqld_con_send_ok_full(con->client, affected_rows, 0,
                                    SERVER_STATUS_AUTOCOMMIT, 0);
}

void admin_config_reload(network_mysqld_con* con, char* object)
{
    if (con->is_admin_client) {
        return;
    }

    if (object == NULL) {
        return admin_reload_settings(con);
    } else if (strcasecmp(object, "user")==0) {
        chassis_config_t* conf = con->srv->config_manager;
        chassis_config_update_object_cache(conf, "users");

        gboolean ok = cetus_users_read_json(con->srv->priv->users, conf);
        if (ok) {
            network_mysqld_con_send_ok(con->client);
        } else {
            network_mysqld_con_send_error(con->client, C("read user failed"));
        }
    } else if (strcasecmp(object, "variables") == 0) {
        char* var_json = NULL;
        if (chassis_config_reload_variables(con->srv->config_manager, object, &var_json)) {
            if (sql_filter_vars_reload_str_rules(var_json) == FALSE) {
                g_warning("variable rule reload error");
            }
            g_free(var_json);
            network_mysqld_con_send_ok(con->client);
        } else {
            network_mysqld_con_send_error(con->client, C("reload variables failed"));
        }
    } else {
        network_mysqld_con_send_error(con->client, C("wrong parameter"));
    }
}

void admin_kill_query(network_mysqld_con* con, guint32 thread_id)
{
    if (con->is_admin_client) {
        con->process_index = thread_id >> 24;

        if (con->process_index > cetus_last_process) {
            con->direct_answer = 1;
            network_mysqld_con_send_error(con->client, C("thread id is not correct"));
        } else {
            con->ask_the_given_worker = 1;
        }

        return;
    }

    gboolean ok = network_mysqld_kill_connection(con->srv, thread_id);
    network_mysqld_con_send_ok_full(con->client, ok ? 1 : 0, 0, SERVER_STATUS_AUTOCOMMIT, 0);

}

void admin_reset_stats(network_mysqld_con* con)
{
    if (con->is_admin_client) {
        return;
    }

    query_stats_t* stats = &con->srv->query_stats;
    memset(stats, 0, sizeof(*stats));
    network_mysqld_con_send_ok_full(con->client, 1, 0,
                                    SERVER_STATUS_AUTOCOMMIT, 0);
}

void admin_select_all_groups(network_mysqld_con* con)
{
    if (con->is_admin_client) {
        con->ask_one_worker = 1;
        con->admin_read_merge = 1;
        return;
    }

    GPtrArray *fields = network_mysqld_proto_fielddefs_new();

    MYSQL_FIELD *field = network_mysqld_proto_fielddef_new();
    field->name = g_strdup("group");
    field->type = FIELD_TYPE_VAR_STRING;
    g_ptr_array_add(fields, field);
    field = network_mysqld_proto_fielddef_new();
    field->name = g_strdup(("master"));
    field->type = FIELD_TYPE_VAR_STRING;
    g_ptr_array_add(fields, field);
    field = network_mysqld_proto_fielddef_new();
    field->name = g_strdup(("slaves"));
    field->type = FIELD_TYPE_VAR_STRING;
    g_ptr_array_add(fields, field);

    GPtrArray *rows = g_ptr_array_new_with_free_func(
        (void*)network_mysqld_mysql_field_row_free);

    GList* free_list = NULL;
    network_backends_t* bs = con->srv->priv->backends;
    int i = 0;
    for (i; i < bs->groups->len; ++i) {
        network_group_t* gp = g_ptr_array_index(bs->groups, i);
        GPtrArray* row = g_ptr_array_new();
        g_ptr_array_add(row, gp->name->str);
        if (gp->master) {
            g_ptr_array_add(row, gp->master->addr->name->str);
        } else {
            g_ptr_array_add(row, "");
        }
        GString* slaves = g_string_new(0);
        network_group_get_slave_names(gp, slaves);
        g_ptr_array_add(row, slaves->str);
        free_list = g_list_append(free_list, slaves);
        g_ptr_array_add(rows, row);
    }
    network_mysqld_con_send_resultset(con->client, fields, rows);

    network_mysqld_proto_fielddefs_free(fields);
    g_ptr_array_free(rows, TRUE);
    g_list_free_full(free_list, g_string_true_free);
}

enum {
    SHARD_HELP = 0x01,
    RW_HELP = 0x02,
    ALL_HELP = 0x03,
};
static struct sql_help_entry_t {
    const char* pattern;
    const char* desc;
    int type;
} sql_help_entries[] = {
    {"select conn_details from backends", "display the idle conns", ALL_HELP},
    {"select * from backends", "list the backends and their state", ALL_HELP},
    {"show connectionlist [<num>]", "show <num> connections", ALL_HELP},
    {"select * from groups","list the backends and their groups", SHARD_HELP},
    {"show allow_ip/deny_ip", "show allow_ip rules of module, currently admin|proxy|shard", ALL_HELP},
    {"add allow_ip/deny_ip '<user>@<address>'", "add address to white list of module", ALL_HELP},
    {"delete allow_ip/deny_ip '<user>@<address>'", "delete address from white list of module", ALL_HELP},
    {"set reduce_conns (true|false)", "reduce idle connections if set to true", ALL_HELP},
    {"set maintain (true|false)", "close all client connections if set to true", ALL_HELP},
    {"show maintain status", "show maintain status", ALL_HELP},
    {"show status [like '%pattern%']", "show select/update/insert/delete statistics", ALL_HELP},
    {"show variables [like '%pattern%']", NULL, ALL_HELP},
    {"select version", "cetus version", ALL_HELP},
    {"select * from user_pwd [where user='<name>']", NULL, ALL_HELP},
    {"select * from app_user_pwd [where user='<name>']", NULL, ALL_HELP},
    {"update user_pwd set password='xx' where user='<name>'", NULL, ALL_HELP},
    {"update app_user_pwd set password='xx' where user='<name>'", NULL, ALL_HELP},
    {"delete from user_pwd where user='<name>'", NULL, ALL_HELP},
    {"delete from app_user_pwd where user='<name>'", NULL, ALL_HELP},
    {"insert into backends values ('<ip:port>', '(ro|rw)', '<state>')",
     "add mysql instance to backends list", RW_HELP},
    {"insert into backends values ('<ip:port@group>', '(ro|rw)', '<state>')",
     "add mysql instance to backends list", SHARD_HELP},
    {"update backends set (type|state)=x where (backend_ndx=<index>|address=<'ip:port'>)",
     "update mysql instance type or state", ALL_HELP},
    {"delete from backends where (backend_ndx=<index>|address=<'ip:port'>)", NULL, ALL_HELP},
    {"remove backend where (backend_ndx=<index>|address='<ip:port>')", NULL, ALL_HELP},
    {"remove backend backend_ndx", NULL, ALL_HELP},
    {"add master <'ip:port'>", NULL, RW_HELP},
    {"add master <'ip:port@group'>", NULL, SHARD_HELP},
    {"add slave <'ip:port'>", NULL, RW_HELP},
    {"add slave <'ip:port@group'>", NULL, SHARD_HELP},
    {"stats get [<item>]", "show query statistics", ALL_HELP},
    {"config get [<item>]", "show config", ALL_HELP},
    {"config set <key>=<value>", NULL, ALL_HELP},
    {"stats reset", "reset query statistics", ALL_HELP},
    {"select * from help", "show this help", ALL_HELP},
    {"select help", "show this help", ALL_HELP},
    {"cetus", "Show overall status of Cetus", ALL_HELP},
    {"create vdb <id> (groupA:xx, groupB:xx) using <method>", "Method example: hash(int,4) range(str)", SHARD_HELP},
    {"create sharded table <schema>.<table> vdb <id> shardkey <key>", "Create sharded table", SHARD_HELP},
    {"select * from vdb", "Show all vdb", SHARD_HELP},
    {"select sharded table", "Show all sharded table", SHARD_HELP},
    {"create single table <schema>.<table> on <group>", "Create single-node table", SHARD_HELP},
    {"select single table", "Show single tables", SHARD_HELP},
    {"sql log status", "show sql log status", ALL_HELP},
    {"sql log start", "start sql log thread", ALL_HELP},
    {"sql log stop", "stop sql log thread", ALL_HELP},
    {"kill query <tid>", "kill session when the thread id is equal to tid ", ALL_HELP},
    {NULL, NULL, 0}
};

void admin_select_help(network_mysqld_con* con)
{
    g_debug("%s:call admin_select_help", G_STRLOC);
    con->direct_answer = 1;

    int needed_type = con->config->has_shard_plugin ? SHARD_HELP : RW_HELP;
    GPtrArray* fields = network_mysqld_proto_fielddefs_new();
    MAKE_FIELD_DEF_2_COL(fields, "Command", "Description");
    GPtrArray *rows = g_ptr_array_new_with_free_func(
        (void*)network_mysqld_mysql_field_row_free);
    int i;
    for (i=0; sql_help_entries[i].pattern; ++i) {
        struct sql_help_entry_t* e = &(sql_help_entries[i]);
        if (e->type & needed_type) {
            char* pattern = e->pattern ? (char*)e->pattern : "";
            char* desc = e->desc ? (char*)e->desc : "";
            APPEND_ROW_2_COL(rows, pattern, desc);
        }
    }
    network_mysqld_con_send_resultset(con->client, fields, rows);
    network_mysqld_proto_fielddefs_free(fields);
    g_ptr_array_free(rows, TRUE);
}

static void get_module_names(chassis* chas, GString* plugin_names)
{
    int i;
    for (i=0; i < chas->modules->len; ++i) {
        chassis_plugin* p = g_ptr_array_index(chas->modules, i);
        g_string_append(plugin_names, p->name);
        g_string_append_c(plugin_names, ' ');
    }
}


void admin_send_overview(network_mysqld_con* con)
{
    if (con->is_admin_client) {
        con->admin_read_merge = 1;
        return;
    }

    chassis_private* g = con->srv->priv;
    chassis_plugin_config *config = con->config;
    GPtrArray *fields = network_mysqld_proto_fielddefs_new();
    MAKE_FIELD_DEF_3_COL(fields, "PID", "Status", "Value");

    char buffer[32];
    cetus_pid_t process_id = getpid();
    sprintf(buffer, "%d", process_id);

    GPtrArray *rows = g_ptr_array_new_with_free_func(
        (void*)network_mysqld_mysql_field_row_free);
    APPEND_ROW_3_COL(rows, g_strdup(buffer), "Cetus version", PLUGIN_VERSION);
    char start_time[32];
    chassis_epoch_to_string(&con->srv->startup_time, C(start_time));
    APPEND_ROW_3_COL(rows, g_strdup(buffer), "Startup time", start_time);
    GString* plugin_names = g_string_new(0);
    get_module_names(con->srv, plugin_names);
    APPEND_ROW_3_COL(rows, g_strdup(buffer), "Loaded modules", plugin_names->str);
    const int bsize = 32;
    static char buf1[32], buf2[32], buf3[32];
    snprintf(buf1, bsize, "%d", network_backends_idle_conns(g->backends));
    APPEND_ROW_3_COL(rows, g_strdup(buffer), "Idle backend connections", buf1);
    snprintf(buf2, bsize, "%d", network_backends_used_conns(g->backends));
    APPEND_ROW_3_COL(rows, g_strdup(buffer), "Used backend connections", buf2);
    snprintf(buf3, bsize, "%d", g->cons->len - 1);
    APPEND_ROW_3_COL(rows, g_strdup(buffer), "Client connections", buf3);

    query_stats_t* stats = &(con->srv->query_stats);
    char qcount[32];
    snprintf(qcount, 32, "%ld", stats->client_query.ro+stats->client_query.rw);
    APPEND_ROW_3_COL(rows, g_strdup(buffer), "Query count", qcount);

    if (config->has_shard_plugin) {
        char xacount[32];
        snprintf(xacount, 32, "%ld", stats->xa_count);
        APPEND_ROW_3_COL(rows, g_strdup(buffer), "XA count", xacount);
    }
    char qps[64];
    admin_stats_get_average(con->config->admin_stats, ADMIN_STATS_QPS, C(qps));
    APPEND_ROW_3_COL(rows, g_strdup(buffer), "QPS (1min, 5min, 15min)", qps);
    char tps[64];
    admin_stats_get_average(con->config->admin_stats, ADMIN_STATS_TPS, C(tps));
    APPEND_ROW_3_COL(rows, g_strdup(buffer), "TPS (1min, 5min, 15min)", tps);
    network_mysqld_con_send_resultset(con->client, fields, rows);

    network_mysqld_proto_fielddefs_free(fields);
    g_ptr_array_free(rows, TRUE);
    g_string_free(plugin_names, TRUE);
}

/*
  DATETIME partitions parsed from SQL originally marked as STR type,
  change it to DATETIME here, release the string memory
*/
static gboolean convert_datetime_partitions(GPtrArray *partitions, sharding_vdb_t *vdb)
{
    if (vdb->key_type == SHARD_DATA_TYPE_DATE
        || vdb->key_type == SHARD_DATA_TYPE_DATETIME)
    {
        int i;
        for (i = 0; i < partitions->len; ++i) {
            sharding_partition_t *part = g_ptr_array_index(partitions, i);
            if (part->key_type != SHARD_DATA_TYPE_STR) {
                g_warning("convert_datetime: date time not string");
                return FALSE;
            }
            if (part->method != SHARD_METHOD_RANGE) {
                g_warning("convert_datetime: date time not using range method");
                return FALSE;
            }
            gboolean ok = FALSE;
            int epoch = chassis_epoch_from_string(part->value, &ok);
            if (ok) {
                part->key_type = vdb->key_type;
                g_free(part->value);
                part->value = (void *)(uint64_t)epoch;
            } else {
                g_warning("Wrong sharding param <datetime format:%s>", part->value);
                return FALSE;
            }
        }
    }
    return TRUE;
}

void admin_create_vdb(network_mysqld_con* con, int id, GPtrArray* partitions,
                      enum sharding_method_t method, int key_type, int shard_num)
{
    if (con->is_admin_client) {
        return;
    }

    sharding_vdb_t* vdb = sharding_vdb_new();
    vdb->id = id;
    vdb->method = method;
    vdb->key_type = key_type;
    vdb->logic_shard_num = shard_num;
    int i;
    for (i = 0; i < partitions->len; ++i) {
        sharding_partition_t* part = g_ptr_array_index(partitions, i);
        g_ptr_array_add(vdb->partitions, part);
    }

    /* some verifcations */
    sharding_partition_t* part = g_ptr_array_index(vdb->partitions, 0);
    if (vdb->method != part->method) {
        network_mysqld_con_send_error(con->client, C("method mismatch"));
        sharding_vdb_free(vdb);
        return;
    }
    if (!convert_datetime_partitions(vdb->partitions, vdb)) {
        network_mysqld_con_send_error(con->client, C("date time format error"));
        sharding_vdb_free(vdb);
        return;
    }
    for (i = 0; i < partitions->len; ++i) {
        /* can't guess key_type of hash-vdb while parsing, assign it here */
        if (vdb->method == SHARD_METHOD_HASH) {
            part->key_type = key_type;
        }
    }
    chassis_private *g = con->srv->priv;
    gboolean ok = sharding_vdb_is_valid(vdb, g->backends->groups->len)
        && shard_conf_add_vdb(vdb);
    if (ok) {
        g_message("Admin: %s", con->orig_sql->str);
        shard_conf_write_json(con->srv->config_manager);
        network_mysqld_con_send_ok(con->client);
    } else {
        sharding_vdb_free(vdb);
        network_mysqld_con_send_error(con->client, C("failed to add vdb"));
    }
}

void admin_create_sharded_table(network_mysqld_con* con, const char* schema,
                                const char* table, const char* key, int vdb_id)
{
    if (con->is_admin_client) {
        return;
    }

    sharding_table_t* t = g_new0(sharding_table_t, 1);
    t->vdb_id = vdb_id;
    t->schema = g_string_new(schema);
    t->name = g_string_new(table);
    t->pkey = g_string_new(key);
    gboolean ok = shard_conf_add_sharded_table(t);
    if (ok) {
        g_message("Admin: %s", con->orig_sql->str);
        shard_conf_write_json(con->srv->config_manager);
        network_mysqld_con_send_ok(con->client);
    } else {
        network_mysqld_con_send_error(con->client, C("failed to add sharded table"));
    }
}

void admin_select_vdb(network_mysqld_con* con)
{
    if (con->is_admin_client) {
        con->ask_one_worker = 1;
        con->admin_read_merge = 1;
        return;
    }

    GList* vdb_list = shard_conf_get_vdb_list();
    GPtrArray* fields = network_mysqld_proto_fielddefs_new();
    MAKE_FIELD_DEF_3_COL(fields, "VDB id", "Method", "Partitions");
    GPtrArray *rows = g_ptr_array_new_with_free_func((void *)network_mysqld_mysql_field_row_free);
    GString* str = g_string_new(0);
    GList* freelist = NULL;
    GList* l = NULL;
    for (l = vdb_list; l; l = l->next) {
        sharding_vdb_t* vdb = l->data;
        char* vdb_id = g_strdup_printf("%d", vdb->id);
        char* method = g_strdup_printf("%s(%s)",
                 vdb->method == SHARD_METHOD_HASH?"hash":"range",
                 sharding_key_type_str(vdb->key_type));
        sharding_vdb_partitions_to_string(vdb, str);
        char* partitions = g_strdup(str->str);
        freelist = g_list_append(freelist, vdb_id);
        freelist = g_list_append(freelist, method);
        freelist = g_list_append(freelist, partitions);
        APPEND_ROW_3_COL(rows, vdb_id, method, partitions);
    }
    network_mysqld_con_send_resultset(con->client, fields, rows);
    network_mysqld_proto_fielddefs_free(fields);
    g_ptr_array_free(rows, TRUE);
    g_string_free(str, TRUE);
    g_list_free_full(freelist, g_free);
}

void admin_select_sharded_table(network_mysqld_con* con)
{
    if (con->is_admin_client) {
        con->ask_one_worker = 1;
        con->admin_read_merge = 1;
        return;
    }

    GList* tables = shard_conf_get_tables();
    GPtrArray* fields = network_mysqld_proto_fielddefs_new();
    MAKE_FIELD_DEF_3_COL(fields, "Table", "VDB id", "Key");
    GPtrArray *rows = g_ptr_array_new_with_free_func((void *)network_mysqld_mysql_field_row_free);
    GList* freelist = NULL;
    GList* l = NULL;
    for (l = tables; l; l = l->next) {
        sharding_table_t* t = l->data;
        char* vdb_id = g_strdup_printf("%d", t->vdb_id);
        char* name = g_strdup_printf("%s.%s", t->schema->str, t->name->str);
        freelist = g_list_append(freelist, vdb_id);
        freelist = g_list_append(freelist, name);
        APPEND_ROW_3_COL(rows, name, vdb_id, t->pkey->str);
    }
    network_mysqld_con_send_resultset(con->client, fields, rows);
    network_mysqld_proto_fielddefs_free(fields);
    g_ptr_array_free(rows, TRUE);
    g_list_free_full(freelist, g_free);
    g_list_free(tables);
}

static gint save_setting(chassis *srv, gint *effected_rows)
{
    gint ret = ASSIGN_OK;

    GKeyFile *keyfile = g_key_file_new();
    g_key_file_set_list_separator(keyfile, ',');
    GString *free_path = g_string_new(NULL);

    if(srv->default_file == NULL) {
        gchar * current_dir =  g_get_current_dir();
        free_path = g_string_append(free_path, current_dir);
        free_path = g_string_append(free_path, "/default.conf");
        srv->default_file = g_strdup(free_path->str);
        g_free(current_dir);
    }

    if(!g_path_is_absolute(srv->default_file)) {
        gchar * current_dir =  g_get_current_dir();
        free_path = g_string_append(free_path, current_dir);
        free_path = g_string_append(free_path, "/");
        free_path = g_string_append(free_path, srv->default_file);
        g_free(srv->default_file);
        srv->default_file = g_strdup(free_path->str);
        g_free(current_dir);
    }
    if(free_path) {
        g_string_free(free_path, TRUE);
    }
    /* rename config file */
    if(srv->default_file) {
        GString *new_file = g_string_new(NULL);
        new_file = g_string_append(new_file, srv->default_file);
        new_file = g_string_append(new_file, ".old");

        if (remove(new_file->str)) {
            g_message("remove operate, filename:%s, errno:%d",
                    new_file->str == NULL? "":new_file->str, errno);
        }
        if (rename(srv->default_file, new_file->str)) {
            g_message("rename operate failed, filename:%s, filename:%s, errno:%d",
                    (srv->default_file == NULL ? "":srv->default_file),
                    (new_file->str == NULL ? "":new_file->str), errno);
        }
        g_string_free(new_file, TRUE);
    }
    if(ret == ASSIGN_OK) {
        /* save new config */
        *effected_rows = chassis_options_save(keyfile, srv->options, srv);
        gsize file_size = 0;
        gchar *file_buf = g_key_file_to_data(keyfile, &file_size, NULL);
        GError *gerr = NULL;
        if (FALSE == g_file_set_contents(srv->default_file, file_buf, file_size, &gerr)) {
            ret = SAVE_ERROR;
            g_clear_error(&gerr);
        } else {
            if((ret = chmod(srv->default_file, S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP))) {
                g_debug("remove operate failed, filename:%s, errno:%d",
                        (srv->default_file == NULL? "":srv->default_file), errno);
                ret = CHMOD_ERROR;
            }
        }
    }
    return ret;
}

static void send_result(network_socket *client, gint ret, gint affected)
{
    if(ret == ASSIGN_OK) {
        network_mysqld_con_send_ok_full(client, affected,
                                        0, SERVER_STATUS_AUTOCOMMIT, 0);
    } else {
        char *msg = NULL;
        switch (ret) {
        case SAVE_ERROR: msg = "save file failed"; break;
        case CHMOD_ERROR: msg = "chmod file failed"; break;
        case CHANGE_SAVE_ERROR: msg = "change config and save file failed"; break;
        default:msg = "unknown error type"; break;
        }
        network_mysqld_con_send_error_full(client, L(msg), 1066, "28000");
    }
}

void admin_save_settings(network_mysqld_con *con)
{
    if (con->is_admin_client) {
        con->ask_one_worker = 1;
        return;
    }

    chassis *srv = con->srv;
    gint effected_rows = 0;
    gint ret = save_setting(srv, &effected_rows);

    network_socket *client = con->client;
    send_result(client, ret, effected_rows);
}

void admin_compatible_cmd(network_mysqld_con* con)
{
    con->direct_answer = 1;
    network_mysqld_con_send_ok(con->client);
}

void admin_show_databases(network_mysqld_con* con)
{
    g_debug("%s:call admin_show_databases", G_STRLOC);
    con->direct_answer = 1;
    GPtrArray* fields = network_mysqld_proto_fielddefs_new();

    MAKE_FIELD_DEF_1_COL(fields, "Database");

    GPtrArray *rows = g_ptr_array_new_with_free_func(
        (void*)network_mysqld_mysql_field_row_free);

    APPEND_ROW_1_COL(rows, "cetus-admin");

    network_mysqld_con_send_resultset(con->client, fields, rows);

    network_mysqld_proto_fielddefs_free(fields);
    g_ptr_array_free(rows, TRUE);
}

void admin_create_single_table(network_mysqld_con* con, const char* schema,
                               const char* table, const char* group)
{
    if (con->is_admin_client) {
        return;
    }

    gboolean ok = shard_conf_add_single_table(schema, table, group);
    if (ok) {
        g_message("Admin: %s", con->orig_sql->str);
        shard_conf_write_json(con->srv->config_manager);
        network_mysqld_con_send_ok_full(con->client, 1, 0, SERVER_STATUS_AUTOCOMMIT, 0);
    } else {
        network_mysqld_con_send_error(con->client, C("failed to add single table"));
    }
}

void admin_select_single_table(network_mysqld_con* con)
{
    if (con->is_admin_client) {
        con->ask_one_worker = 1;
        con->admin_read_merge = 1;
        return;
    }

    GList* tables = shard_conf_get_single_tables();
    GPtrArray* fields = network_mysqld_proto_fielddefs_new();
    MAKE_FIELD_DEF_2_COL(fields, "Table", "Group");
    GPtrArray *rows = g_ptr_array_new_with_free_func((void *)network_mysqld_mysql_field_row_free);
    GList* freelist = NULL;
    GList* l = NULL;
    for (l = tables; l; l = l->next) {
        struct single_table_t* t = l->data;
        char* name = g_strdup_printf("%s.%s", t->schema->str, t->name->str);
        freelist = g_list_append(freelist, name);
        APPEND_ROW_2_COL(rows, name, t->group->str);
    }
    network_mysqld_con_send_resultset(con->client, fields, rows);
    network_mysqld_proto_fielddefs_free(fields);
    g_ptr_array_free(rows, TRUE);
    g_list_free_full(freelist, g_free);
}

void admin_sql_log_start(network_mysqld_con* con) {
    if (!con->srv->sql_mgr) {
        network_mysqld_con_send_error(con->client, C("Unexpected error"));
        return;
    }
    
    if (con->is_admin_client) {
        return;
    }

    if (con->srv->sql_mgr->sql_log_switch == OFF) {
        network_mysqld_con_send_error(con->client, C("can not start sql log thread, because sql-log-switch = OFF"));
        return;
    }
    if (con->srv->sql_mgr->sql_log_action == SQL_LOG_STOP) {
        sql_log_thread_start(con->srv->sql_mgr);
        network_mysqld_con_send_ok(con->client);
    } else {
        network_mysqld_con_send_error(con->client, C("sql log is running now"));
    }
}

void admin_sql_log_stop(network_mysqld_con* con) {
    if (!con->srv->sql_mgr) {
        network_mysqld_con_send_error(con->client, C("Unexpected error"));
        return;
    }
    
    if (con->is_admin_client) {
        return;
    }

    if (con->srv->sql_mgr->sql_log_action == SQL_LOG_START) {
        con->srv->sql_mgr->sql_log_action = SQL_LOG_UNKNOWN;
        network_mysqld_con_send_ok(con->client);
    } else {
        network_mysqld_con_send_error(con->client, C("sql log thread has been stopped"));
    }
}

void admin_sql_log_status(network_mysqld_con* con) {
    if (!con->srv->sql_mgr) {
        network_mysqld_con_send_error(con->client, C("Unexpected error"));
        return;
    }
    
    if (con->is_admin_client) {
        con->admin_read_merge = 1;
        return;
    }

    gchar* pattern = g_strdup("%sql-log-%");
    GList *options = admin_get_all_options(con->srv);

    GPtrArray *fields = network_mysqld_proto_fielddefs_new();
    MAKE_FIELD_DEF_4_COL(fields, "PID", "Variable_name", "Value", "Property");

    char buffer[32];
    cetus_pid_t process_id = getpid();
    sprintf(buffer, "%d", process_id);

    GPtrArray *rows = g_ptr_array_new_with_free_func((void *)network_mysqld_mysql_field_row_free);

    GList *freelist = NULL;
    GList *l = NULL;

    for (l = options; l; l = l->next) {
        chassis_option_t *opt = l->data;
        /* just support these for now */
        if (sql_pattern_like(pattern, opt->long_name)) {
            struct external_param param = {0};
            param.chas = con->srv;
            param.opt_type = opt->opt_property;
            char *value = opt->show_hook != NULL? opt->show_hook(&param) : NULL;
            if(NULL == value) {
                continue;
            }
            freelist = g_list_append(freelist, value);
            APPEND_ROW_4_COL(rows, g_strdup(buffer), (char *)opt->long_name, value,
                    (CAN_ASSIGN_OPTS_PROPERTY(opt->opt_property)? "Dynamic" : "Static"));
        }
    }

    APPEND_ROW_4_COL(rows, g_strdup(buffer), "sql-log-state", 
            con->srv->sql_mgr->sql_log_action == SQL_LOG_START ? "running": "stopped", "Internal");
    gchar *cached = NULL;
    if (con->srv->sql_mgr->fifo && (con->srv->sql_mgr->sql_log_action == SQL_LOG_START)) {
        cached = g_strdup_printf("%u", con->srv->sql_mgr->fifo->in - con->srv->sql_mgr->fifo->in);
    } else {
        cached = g_strdup("NULL");
    }
    APPEND_ROW_4_COL(rows, g_strdup(buffer), "sql-log-cached", cached, "Internal");
    gchar *cursize = g_strdup_printf("%lu", con->srv->sql_mgr->sql_log_cursize);
    APPEND_ROW_4_COL(rows, g_strdup(buffer), "sql-log-cursize", cursize, "Internal");

    APPEND_ROW_4_COL(rows, g_strdup(buffer), "sql-log-fullname",
            con->srv->sql_mgr->sql_log_fullname == NULL ? "NULL" : con->srv->sql_mgr->sql_log_fullname, "Internal");

    network_mysqld_con_send_resultset(con->client, fields, rows);

    network_mysqld_proto_fielddefs_free(fields);
    g_ptr_array_free(rows, TRUE);
    g_list_free_full(freelist, g_free);
    g_list_free(options);
    g_free(pattern);
    g_free(cached);
    g_free(cursize);
}

void admin_comment_handle(network_mysqld_con* con) {
    con->direct_answer = 1;
    network_mysqld_con_send_ok_full(con->client, 0, 0, 0, 0);
}

void admin_select_version_comment(network_mysqld_con* con) {
    con->direct_answer = 1;

    GPtrArray* fields = network_mysqld_proto_fielddefs_new();
    MAKE_FIELD_DEF_1_COL(fields, "@@version_comment");
    GPtrArray *rows = g_ptr_array_new_with_free_func(
        (void*)network_mysqld_mysql_field_row_free);
    APPEND_ROW_1_COL(rows, "Cetus proxy");

    network_mysqld_con_send_resultset(con->client, fields, rows);

    network_mysqld_proto_fielddefs_free(fields);
    g_ptr_array_free(rows, TRUE);
}
