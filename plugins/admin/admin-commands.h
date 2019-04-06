#ifndef ADMIN_COMMANDS_H
#define ADMIN_COMMANDS_H

#include "glib-ext.h"
#include "network-mysqld.h"

typedef struct token_t {
  char* z;
  int n;
} token_t;

void admin_clear_error(network_mysqld_con*);
int admin_get_error(network_mysqld_con*);
void admin_syntax_error(network_mysqld_con*);
void admin_stack_overflow(network_mysqld_con*);

void admin_select_conn_details(network_mysqld_con* con);
void admin_select_all_backends(network_mysqld_con*);
void admin_select_all_groups(network_mysqld_con* con);
void admin_show_connectionlist(network_mysqld_con *admin_con, int show_count);
void admin_acl_show_rules(network_mysqld_con *con, gboolean is_white);
void admin_acl_add_rule(network_mysqld_con *con, gboolean is_white, char *addr);
void admin_acl_delete_rule(network_mysqld_con *con, gboolean is_white, char* ip);
void admin_set_reduce_conns(network_mysqld_con* con, int mode);
void admin_set_maintain(network_mysqld_con* con, int mode);
void admin_set_charset_check(network_mysqld_con* con, int mode);
void admin_show_maintain(network_mysqld_con* con);
void admin_show_variables(network_mysqld_con* con, const char* like);
void admin_set_server_conn_refresh(network_mysqld_con* con);
void admin_select_version(network_mysqld_con* con);
void admin_select_connection_stat(network_mysqld_con* con, int backend_ndx, char *user);
void admin_select_user_password(network_mysqld_con* con, char* from_table, char *user);
void admin_update_user_password(network_mysqld_con* con, char *from_table,
                                char *user, char *password);
void admin_delete_user_password(network_mysqld_con* con, char* user);
void admin_insert_backend(network_mysqld_con* con, char *addr, char *type, char *state);
void admin_update_backend(network_mysqld_con* con, GList* equations,
                          char *cond_key, char *cond_val);
void admin_delete_backend(network_mysqld_con* con, char *key, char *val);
void admin_get_stats(network_mysqld_con* con, char* p);
void admin_get_config(network_mysqld_con* con, char* p);
void admin_set_config(network_mysqld_con* con, char* key, char* value);
void admin_config_reload(network_mysqld_con* con, char* object);
void admin_reset_stats(network_mysqld_con* con);
void admin_select_help(network_mysqld_con* con);
void admin_send_overview(network_mysqld_con* con);
enum sharding_method_t;
void admin_create_vdb(network_mysqld_con* con, int id, GPtrArray* partitions,
                      enum sharding_method_t method, int key_type, int shard_num);
void admin_create_sharded_table(network_mysqld_con*, const char* schema, const char* table,
                                const char* key, int vdb_id);

void admin_select_vdb(network_mysqld_con* con);
void admin_select_sharded_table(network_mysqld_con* con);
void admin_save_settings(network_mysqld_con* con);
void admin_compatible_cmd(network_mysqld_con* con);
void admin_show_databases(network_mysqld_con* con);
void admin_create_single_table(network_mysqld_con*, const char* schema, const char* table,
                               const char* group);
void admin_select_single_table(network_mysqld_con*);

void admin_sql_log_start(network_mysqld_con* con);
void admin_sql_log_stop(network_mysqld_con* con);
void admin_sql_log_status(network_mysqld_con* con);
void admin_kill_query(network_mysqld_con* con, guint32);
void admin_comment_handle(network_mysqld_con* con);
void admin_select_version_comment(network_mysqld_con* con);
char* admin_get_value_by_key(network_mysqld_con* con, const char *key);
#endif // ADMIN_COMMANDS_H
