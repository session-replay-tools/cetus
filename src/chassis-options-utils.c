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

#include "chassis-timings.h"
#include "chassis-options-utils.h"
#include "chassis-plugin.h"
#include "cetus-util.h"
#include <glib-ext.h>
#include <errno.h>

gchar*
show_verbose_shutdown(gpointer param) {
    struct external_param *opt_param = (struct external_param *)param;
    chassis *srv = opt_param->chas;
    gint opt_type = opt_param->opt_type;
    if(CAN_SHOW_OPTS_PROPERTY(opt_type)) {
        return g_strdup_printf("%s", srv->verbose_shutdown ? "true" : "false");
    }
    if(CAN_SAVE_OPTS_PROPERTY(opt_type)) {
        return srv->verbose_shutdown ? g_strdup("true") : NULL;
    }
    return NULL;
}

gchar* show_daemon_mode(gpointer param) {
    struct external_param *opt_param = (struct external_param *)param;
    chassis *srv = opt_param->chas;
    gint opt_type = opt_param->opt_type;
    if(CAN_SHOW_OPTS_PROPERTY(opt_type)) {
        return g_strdup_printf("%s", srv->daemon_mode ? "true" : "false");
    }
    if(CAN_SAVE_OPTS_PROPERTY(opt_type)) {
        return srv->daemon_mode ? g_strdup("true") : NULL;
    }
    return NULL;
}

gchar*
show_user(gpointer param) {
    struct external_param *opt_param = (struct external_param *)param;
    chassis *srv = opt_param->chas;
    gint opt_type = opt_param->opt_type;
    if(CAN_SHOW_OPTS_PROPERTY(opt_type)) {
        return g_strdup_printf("%s", srv->user != NULL ? srv->user:"NULL");
    }
    if(CAN_SAVE_OPTS_PROPERTY(opt_type)) {
        if(srv->user) {
            return g_strdup_printf("%s", srv->user);
        }
    }
    return NULL;
}

gchar*
show_basedir(gpointer param) {
    struct external_param *opt_param = (struct external_param *)param;
    chassis *srv = opt_param->chas;
    gint opt_type = opt_param->opt_type;
    if(CAN_SHOW_OPTS_PROPERTY(opt_type)) {
        return g_strdup_printf("%s", srv->base_dir != NULL ? srv->base_dir:"NULL");
    }
    if(CAN_SAVE_OPTS_PROPERTY(opt_type)) {
        if(srv->base_dir) {
           return g_strdup_printf("%s", srv->base_dir);
        }
    }
    return NULL;
}

gchar*
show_confdir(gpointer param) {
    struct external_param *opt_param = (struct external_param *)param;
    chassis *srv = opt_param->chas;
    gint opt_type = opt_param->opt_type;
    if(CAN_SHOW_OPTS_PROPERTY(opt_type)) {
        return g_strdup_printf("%s", srv->conf_dir != NULL ? srv->conf_dir:"NULL");
    }
    if(CAN_SAVE_OPTS_PROPERTY(opt_type)) {
        if(srv->conf_dir) {
            return g_strdup_printf("%s", srv->conf_dir);
        }
    }
    return NULL;
}

gchar*
show_pidfile(gpointer param) {
    struct external_param *opt_param = (struct external_param *)param;
    chassis *srv = opt_param->chas;
    gint opt_type = opt_param->opt_type;
    if(CAN_SHOW_OPTS_PROPERTY(opt_type)) {
        return g_strdup_printf("%s", srv->pid_file != NULL ? srv->pid_file:"NULL");
    }
    if(CAN_SAVE_OPTS_PROPERTY(opt_type)) {
        if(srv->pid_file) {
            return g_strdup_printf("%s", srv->pid_file);
        }
    }
    return NULL;
}

gchar*
show_plugindir(gpointer param) {
    struct external_param *opt_param = (struct external_param *)param;
    chassis *srv = opt_param->chas;
    gint opt_type = opt_param->opt_type;
    if(CAN_SHOW_OPTS_PROPERTY(opt_type)) {
        return g_strdup_printf("%s", srv->plugin_dir != NULL ? srv->plugin_dir:"NULL");
    }
    if(CAN_SAVE_OPTS_PROPERTY(opt_type)) {
        if(srv->plugin_dir) {
            return g_strdup_printf("%s", srv->plugin_dir);
        }
    }
    return NULL;
}

gchar*
show_plugins(gpointer param) {
    char *ret = NULL;
    struct external_param *opt_param = (struct external_param *)param;
    chassis *srv = opt_param->chas;
    gint opt_type = opt_param->opt_type;
    if(CAN_SHOW_OPTS_PROPERTY(opt_type)) {
        int i = 0;
        GString *free_str = g_string_new(NULL);
        for (i = 0; srv->plugin_names[i]; ++i) {
            free_str = g_string_append(free_str, srv->plugin_names[i]);
            free_str = g_string_append(free_str, ",");
        }
        if(free_str->len) {
            free_str->str[free_str->len - 1] = '\0';
        }
        ret = g_strdup_printf("%s", free_str->len ? free_str->str : "NULL");
        g_string_free(free_str, TRUE);
    }
    if(CAN_SAVE_OPTS_PROPERTY(opt_type)) {
        int i = 0;
        GString *free_str = g_string_new(NULL);
        for (i = 0; srv->plugin_names[i]; ++i) {
            free_str = g_string_append(free_str, srv->plugin_names[i]);
            free_str = g_string_append(free_str, ",");
        }
        if(free_str->len) {
            free_str->str[free_str->len - 1] = '\0';
            ret = g_strdup_printf("%s", free_str->str);
        }
        g_string_free(free_str, TRUE);
    }
    return ret;
}

gchar*
show_log_level(gpointer param) {
    struct external_param *opt_param = (struct external_param *)param;
    chassis *srv = opt_param->chas;
    gint opt_type = opt_param->opt_type;
    if(CAN_SHOW_OPTS_PROPERTY(opt_type)) {
        return g_strdup_printf("%s", srv->log_level != NULL ? srv->log_level:"NULL");
    }
    if(CAN_SAVE_OPTS_PROPERTY(opt_type)) {
        if(srv->log_level) {
             return g_strdup_printf("%s", srv->log_level);
        }
    }
    return NULL;
}

gint
assign_log_level(const gchar *newval, gpointer param) {
    gint ret = ASSIGN_ERROR;
    struct external_param *opt_param = (struct external_param *)param;
    chassis *srv = opt_param->chas;
    gint opt_type = opt_param->opt_type;
    if(CAN_ASSIGN_OPTS_PROPERTY(opt_type)) {
        if (0 == chassis_log_set_level(srv->log, newval)) {
            if(srv->log_level) {
                g_free(srv->log_level);
            }
            srv->log_level = g_strdup(newval);

            ret = ASSIGN_OK;
        } else {
            ret = ASSIGN_VALUE_INVALID;
        }
    }
    return ret;
}

gchar*
show_log_file(gpointer param) {
    struct external_param *opt_param = (struct external_param *)param;
    chassis *srv = opt_param->chas;
    gint opt_type = opt_param->opt_type;
    if(CAN_SHOW_OPTS_PROPERTY(opt_type)) {
        return g_strdup_printf("%s", srv->log->log_filename != NULL ? srv->log->log_filename:"NULL");
    }
    if(CAN_SAVE_OPTS_PROPERTY(opt_type)) {
        if(srv->log->log_filename) {
            return g_strdup_printf("%s", srv->log->log_filename);
        }
    }
    return NULL;
}

gchar*
show_log_xa_file(gpointer param) {
    struct external_param *opt_param = (struct external_param *)param;
    chassis *srv = opt_param->chas;
    gint opt_type = opt_param->opt_type;
    if(CAN_SHOW_OPTS_PROPERTY(opt_type)) {
        return g_strdup_printf("%s", srv->log_xa_filename != NULL ? srv->log_xa_filename:"NULL");
    }
    if(CAN_SAVE_OPTS_PROPERTY(opt_type)) {
        if(srv->log_xa_filename) {
            return g_strdup_printf("%s", srv->log_xa_filename);
        }
    }
    return NULL;
}

gchar*
show_log_backtrace_on_crash(gpointer param) {
    struct external_param *opt_param = (struct external_param *)param;
    chassis *srv = opt_param->chas;
    gint opt_type = opt_param->opt_type;
    if(CAN_SHOW_OPTS_PROPERTY(opt_type)) {
        return g_strdup_printf("%s", srv->invoke_dbg_on_crash ? "true":"false");
    }
    if(CAN_SAVE_OPTS_PROPERTY(opt_type)) {
#ifdef HAVE_SIGACTION
        return srv->invoke_dbg_on_crash ? NULL:g_strdup("false");
#else
        return srv->invoke_dbg_on_crash ? g_strdup("true"):NULL;
#endif

    }
    return NULL;
}

gchar*
show_keepalive(gpointer param) {
    struct external_param *opt_param = (struct external_param *)param;
    chassis *srv = opt_param->chas;
    gint opt_type = opt_param->opt_type;
    if(CAN_SHOW_OPTS_PROPERTY(opt_type)) {
        return g_strdup_printf("%s", srv->auto_restart ? "true":"false");
    }
    if(CAN_SAVE_OPTS_PROPERTY(opt_type)) {
        return srv->auto_restart ? g_strdup("true"):NULL;
    }
    return NULL;
}

gchar*
show_max_open_files(gpointer param) {
    struct external_param *opt_param = (struct external_param *)param;
    chassis *srv = opt_param->chas;
    gint opt_type = opt_param->opt_type;
    if(CAN_SHOW_OPTS_PROPERTY(opt_type)) {
        return g_strdup_printf("%d", srv->max_files_number);
    }
    if(CAN_SAVE_OPTS_PROPERTY(opt_type)) {
        if(srv->max_files_number == 0) {
            return NULL;
        }
            return g_strdup_printf("%d", srv->max_files_number);
    }
    return NULL;
}

gchar*
show_default_charset(gpointer param) {
    struct external_param *opt_param = (struct external_param *)param;
    chassis *srv = opt_param->chas;
    gint opt_type = opt_param->opt_type;
    if(CAN_SHOW_OPTS_PROPERTY(opt_type)) {
        return g_strdup_printf("%s", srv->default_charset != NULL ? srv->default_charset : "NULL");
    }
    if(CAN_SAVE_OPTS_PROPERTY(opt_type)) {
        if(srv->default_charset) {
            return g_strdup_printf("%s", srv->default_charset);
        }
    }
    return NULL;
}

gint
assign_default_charset(const gchar *newval, gpointer param) {
    gint ret = ASSIGN_ERROR;
    struct external_param *opt_param = (struct external_param *)param;
    chassis *srv = opt_param->chas;
    gint opt_type = opt_param->opt_type;
    if(CAN_ASSIGN_OPTS_PROPERTY(opt_type)) {
        if(NULL != newval) {
            if(srv->default_charset) {
                g_free(srv->default_charset);
            }
            srv->default_charset = g_strdup(newval);
            ret = ASSIGN_OK;
        } else {
           ret = ASSIGN_VALUE_INVALID;
        }
    }
    return ret;
}

gchar*
show_default_username(gpointer param) {
    struct external_param *opt_param = (struct external_param *)param;
    chassis *srv = opt_param->chas;
    gint opt_type = opt_param->opt_type;
    if(CAN_SHOW_OPTS_PROPERTY(opt_type)) {
        return g_strdup_printf("%s", srv->default_username != NULL ? srv->default_username : "NULL");
    }
    if(CAN_SAVE_OPTS_PROPERTY(opt_type)) {
        if(srv->default_username) {
            return g_strdup_printf("%s", srv->default_username);
        }
    }
    return NULL;
}

gint
assign_default_username(const gchar *newval, gpointer param) {
    gint ret = ASSIGN_ERROR;
    struct external_param *opt_param = (struct external_param *)param;
    chassis *srv = opt_param->chas;
    gint opt_type = opt_param->opt_type;
    if(CAN_ASSIGN_OPTS_PROPERTY(opt_type)) {
        if(NULL != newval) {
            if(srv->default_username) {
                g_free(srv->default_username);
            }
            srv->default_username = g_strdup(newval);
            ret = ASSIGN_OK;
        } else {
            ret = ASSIGN_VALUE_INVALID;
        }
    }
    return ret;
}

gchar*
show_default_db(gpointer param) {
    struct external_param *opt_param = (struct external_param *)param;
    chassis *srv = opt_param->chas;
    gint opt_type = opt_param->opt_type;
    if(CAN_SHOW_OPTS_PROPERTY(opt_type)) {
        return g_strdup_printf("%s", srv->default_db != NULL ? srv->default_db : "NULL");
    }
    if(CAN_SAVE_OPTS_PROPERTY(opt_type)) {
        if(srv->default_db) {
            return g_strdup_printf("%s", srv->default_db);
        }
    }
    return NULL;
}

gint
assign_default_db(const gchar *newval, gpointer param) {
    gint ret = ASSIGN_ERROR;
    struct external_param *opt_param = (struct external_param *)param;
    chassis *srv = opt_param->chas;
    gint opt_type = opt_param->opt_type;
    if(CAN_ASSIGN_OPTS_PROPERTY(opt_type)) {
        if(NULL != newval) {
            if(srv->default_db) {
                g_free(srv->default_db);
            }
            srv->default_db = g_strdup(newval);
            ret = ASSIGN_OK;
        } else {
            ret = ASSIGN_VALUE_INVALID;
        }
    }
    return ret;
}

gchar*
show_default_pool_size(gpointer param) {
    struct external_param *opt_param = (struct external_param *)param;
    chassis *srv = opt_param->chas;
    gint opt_type = opt_param->opt_type;
    if(CAN_SHOW_OPTS_PROPERTY(opt_type)) {
        return g_strdup_printf("%d", srv->mid_idle_connections);
    }
    if(CAN_SAVE_OPTS_PROPERTY(opt_type)) {
        if(srv->mid_idle_connections == 100) {
            return NULL;
        }
        return g_strdup_printf("%d", srv->mid_idle_connections);
    }
    return NULL;
}

gint
assign_default_pool_size(const gchar *newval, gpointer param) {
    gint ret = ASSIGN_ERROR;
    struct external_param *opt_param = (struct external_param *)param;
    chassis *srv = opt_param->chas;
    gint opt_type = opt_param->opt_type;
    if(CAN_ASSIGN_OPTS_PROPERTY(opt_type)) {
        if(NULL != newval) {
            gint value = 0;
                if(try_get_int_value(newval, &value)) {
                    if(value >= 0) {
                        srv->mid_idle_connections = value;
                        ret = ASSIGN_OK;
                    } else {
                        ret = ASSIGN_VALUE_INVALID;
                    }
                } else {
                    ret = ASSIGN_VALUE_INVALID;
                }
        } else {
            ret = ASSIGN_VALUE_INVALID;
        }
    }
    return ret;
}

gchar*
show_max_pool_size(gpointer param) {
    struct external_param *opt_param = (struct external_param *)param;
    chassis *srv = opt_param->chas;
    gint opt_type = opt_param->opt_type;
    if(CAN_SHOW_OPTS_PROPERTY(opt_type)) {
        return g_strdup_printf("%d", srv->max_idle_connections);
    }
    if(CAN_SAVE_OPTS_PROPERTY(opt_type)) {
        if(srv->mid_idle_connections *2 == srv->max_idle_connections) {
            return NULL;
        }
            return g_strdup_printf("%d", srv->max_idle_connections);
    }
    return NULL;
}

gint
assign_max_pool_size(const gchar *newval, gpointer param) {
    gint ret = ASSIGN_ERROR;
    struct external_param *opt_param = (struct external_param *)param;
    chassis *srv = opt_param->chas;
    gint opt_type = opt_param->opt_type;
    if(CAN_ASSIGN_OPTS_PROPERTY(opt_type)) {
        if(NULL != newval) {
            gint value = 0;
            if(try_get_int_value(newval, &value)) {
                if (value >= srv->mid_idle_connections) {
                    srv->max_idle_connections = value;
                } else {
                    srv->max_idle_connections = srv->mid_idle_connections << 1;
                }
                ret = ASSIGN_OK;
            } else {
                ret = ASSIGN_VALUE_INVALID;
            }
        } else {
            ret = ASSIGN_VALUE_INVALID;
        }
    }
    return ret;
}

gchar*
show_max_resp_len(gpointer param) {
    struct external_param *opt_param = (struct external_param *)param;
    chassis *srv = opt_param->chas;
    gint opt_type = opt_param->opt_type;
    if(CAN_SHOW_OPTS_PROPERTY(opt_type)) {
        return g_strdup_printf("%d", srv->max_resp_len);
    }
    if(CAN_SAVE_OPTS_PROPERTY(opt_type)) {
        if(10 * 1024 * 1024 == srv->max_resp_len) {
            return NULL;
        }
        return g_strdup_printf("%d", srv->max_resp_len);
    }
    return NULL;
}

gint
assign_max_resp_len(const gchar *newval, gpointer param) {
    gint ret = ASSIGN_ERROR;
    struct external_param *opt_param = (struct external_param *)param;
    chassis *srv = opt_param->chas;
    gint opt_type = opt_param->opt_type;
    if(CAN_ASSIGN_OPTS_PROPERTY(opt_type)) {
        if(NULL != newval) {
            gint value = 0;
            if(try_get_int_value(newval, &value)) {
                if(value >= 0) {
                    srv->max_resp_len = value;
                    ret = ASSIGN_OK;
                } else {
                    ret = ASSIGN_VALUE_INVALID;
                }
            } else {
                ret = ASSIGN_VALUE_INVALID;
            }
        } else {
            ret = ASSIGN_VALUE_INVALID;
        }
    }
    return ret;
}

gchar*
show_max_alive_time(gpointer param) {
    struct external_param *opt_param = (struct external_param *)param;
    chassis *srv = opt_param->chas;
    gint opt_type = opt_param->opt_type;
    if(CAN_SHOW_OPTS_PROPERTY(opt_type)) {
        return g_strdup_printf("%d (s)", srv->max_alive_time);
    }
    if(CAN_SAVE_OPTS_PROPERTY(opt_type)) {
        if(srv->max_alive_time == DEFAULT_LIVE_TIME) {
            return NULL;
        }
        return g_strdup_printf("%d", srv->max_alive_time);
    }
    return NULL;
}

gint
assign_max_alive_time(const gchar *newval, gpointer param) {
    gint ret = ASSIGN_ERROR;
    struct external_param *opt_param = (struct external_param *)param;
    chassis *srv = opt_param->chas;
    gint opt_type = opt_param->opt_type;
    if(CAN_ASSIGN_OPTS_PROPERTY(opt_type)) {
        if(NULL != newval) {
            gint value = 0;
            if(try_get_int_value(newval, &value)) {
                if(value >= 0) {
                    if (value < 600) {
                        value = 600;
                    }
                    srv->max_alive_time = value;
                    ret = ASSIGN_OK;
                } else {
                    ret = ASSIGN_VALUE_INVALID;
                }
            } else {
                ret = ASSIGN_VALUE_INVALID;
            }
        } else {
            ret = ASSIGN_VALUE_INVALID;
        }
    }
    return ret;
}

gchar*
show_merged_output_size(gpointer param) {
    struct external_param *opt_param = (struct external_param *)param;
    chassis *srv = opt_param->chas;
    gint opt_type = opt_param->opt_type;
    if(CAN_SHOW_OPTS_PROPERTY(opt_type)) {
        return g_strdup_printf("%d", srv->merged_output_size);
    }
    if(CAN_SAVE_OPTS_PROPERTY(opt_type)) {
    if(srv->merged_output_size == 8192) {
        return NULL;
    }
        return g_strdup_printf("%d", srv->merged_output_size);
    }
    return NULL;
}

gint
assign_merged_output_size(const gchar *newval, gpointer param) {
    gint ret = ASSIGN_ERROR;
    struct external_param *opt_param = (struct external_param *)param;
    chassis *srv = opt_param->chas;
    gint opt_type = opt_param->opt_type;
    if(CAN_ASSIGN_OPTS_PROPERTY(opt_type)) {
        if(NULL != newval) {
            gint value = 0;
            if(try_get_int_value(newval, &value)) {
                if(value >= 0) {
                    srv->merged_output_size = value;
                    srv->compressed_merged_output_size = srv->merged_output_size << 3;
                    ret = ASSIGN_OK;
                } else {
                    ret = ASSIGN_VALUE_INVALID;
                }
            } else {
                ret = ASSIGN_VALUE_INVALID;
            }
        } else {
            ret = ASSIGN_VALUE_INVALID;
        }
    }
    return ret;
}

gchar*
show_max_header_size(gpointer param) {
    struct external_param *opt_param = (struct external_param *)param;
    chassis *srv = opt_param->chas;
    gint opt_type = opt_param->opt_type;
    if(CAN_SHOW_OPTS_PROPERTY(opt_type)) {
        return g_strdup_printf("%d", srv->max_header_size);
    }
    if(CAN_SAVE_OPTS_PROPERTY(opt_type)) {
        if(srv->max_header_size == 65536) {
            return NULL;
        }
        return g_strdup_printf("%d", srv->max_header_size);
    }
    return NULL;
}

gint
assign_max_header_size(const gchar *newval, gpointer param) {
    gint ret = ASSIGN_ERROR;
    struct external_param *opt_param = (struct external_param *)param;
    chassis *srv = opt_param->chas;
    gint opt_type = opt_param->opt_type;
    if(CAN_ASSIGN_OPTS_PROPERTY(opt_type)) {
        if(NULL != newval) {
            gint value = 0;
            if(try_get_int_value(newval, &value)) {
                if(value >= 0) {
                    srv->max_header_size = value;
                    ret = ASSIGN_OK;
                } else {
                    ret = ASSIGN_VALUE_INVALID;
                }
            } else {
                ret = ASSIGN_VALUE_INVALID;
            }
        } else {
            ret = ASSIGN_VALUE_INVALID;
        }
    }
    return ret;
}

gchar*
show_worker_id(gpointer param) {
    struct external_param *opt_param = (struct external_param *)param;
    chassis *srv = opt_param->chas;
    gint opt_type = opt_param->opt_type;
    if(CAN_SHOW_OPTS_PROPERTY(opt_type)) {
        return g_strdup_printf("%d", srv->guid_state.worker_id);
    }
    if(CAN_SAVE_OPTS_PROPERTY(opt_type)) {
        return g_strdup_printf("%d", srv->guid_state.worker_id);
    }
    return NULL;
}

gchar*
show_disable_threads(gpointer param) {
    struct external_param *opt_param = (struct external_param *)param;
    chassis *srv = opt_param->chas;
    gint opt_type = opt_param->opt_type;
    if(CAN_SHOW_OPTS_PROPERTY(opt_type)) {
        return g_strdup_printf("%s", srv->disable_threads ? "true" : "false");
    }
    if(CAN_SAVE_OPTS_PROPERTY(opt_type)) {
        return srv->disable_threads ? g_strdup("true") : NULL;
    }
    return NULL;
}

gchar*
show_enable_back_compress(gpointer param) {
    struct external_param *opt_param = (struct external_param *)param;
    chassis *srv = opt_param->chas;
    gint opt_type = opt_param->opt_type;
    if(CAN_SHOW_OPTS_PROPERTY(opt_type)) {
        return g_strdup_printf("%s", srv->is_back_compressed ? "true" : "false");
    }
    if(CAN_SAVE_OPTS_PROPERTY(opt_type)) {
        return srv->is_back_compressed ? g_strdup("true") : NULL;
    }
    return NULL;
}

gchar*
show_enable_client_compress(gpointer param) {
    struct external_param *opt_param = (struct external_param *)param;
    chassis *srv = opt_param->chas;
    gint opt_type = opt_param->opt_type;
    if(CAN_SHOW_OPTS_PROPERTY(opt_type)) {
        return g_strdup_printf("%s", srv->compress_support ? "true" : "false");
    }
    if(CAN_SAVE_OPTS_PROPERTY(opt_type)) {
        return srv->compress_support ? g_strdup("true") : NULL;
    }
    return NULL;
}

gchar*
show_check_slave_delay(gpointer param) {
    struct external_param *opt_param = (struct external_param *)param;
    chassis *srv = opt_param->chas;
    gint opt_type = opt_param->opt_type;
    if(CAN_SHOW_OPTS_PROPERTY(opt_type)) {
        return g_strdup_printf("%s", srv->check_slave_delay ? "true" : "false");
    }
    if(CAN_SAVE_OPTS_PROPERTY(opt_type)) {
        return srv->check_slave_delay ? g_strdup("true") : NULL;
    }
    return NULL;
}

gchar*
show_slave_delay_down(gpointer param) {
    struct external_param *opt_param = (struct external_param *)param;
    chassis *srv = opt_param->chas;
    gint opt_type = opt_param->opt_type;
    if(CAN_SHOW_OPTS_PROPERTY(opt_type)) {
        return g_strdup_printf("%lf (s)", srv->slave_delay_down_threshold_sec);
    }
    if(CAN_SAVE_OPTS_PROPERTY(opt_type)) {
        if(srv->slave_delay_down_threshold_sec == 60) {
            return NULL;
        }
        return g_strdup_printf("%lf", srv->slave_delay_down_threshold_sec);
    }
    return NULL;
}

gint
assign_slave_delay_down(const gchar *newval, gpointer param) {
    gint ret = ASSIGN_ERROR;
    struct external_param *opt_param = (struct external_param *)param;
    chassis *srv = opt_param->chas;
    gint opt_type = opt_param->opt_type;
    if(CAN_ASSIGN_OPTS_PROPERTY(opt_type)) {
        if(NULL != newval) {
            gdouble value = 0;
            if(try_get_double_value(newval, &value)) {
                if(value >= 0) {
                    srv->slave_delay_down_threshold_sec = value;
                    if(srv->slave_delay_recover_threshold_sec < 0) {
                        srv->slave_delay_recover_threshold_sec = srv->slave_delay_down_threshold_sec/2;
                    }
                    ret = ASSIGN_OK;
                } else {
                    ret = ASSIGN_VALUE_INVALID;
                }
            } else {
                ret = ASSIGN_VALUE_INVALID;
            }
        } else {
            ret = ASSIGN_VALUE_INVALID;
        }
    }
    return ret;
}

gchar*
show_slave_delay_recover(gpointer param) {
    struct external_param *opt_param = (struct external_param *)param;
    chassis *srv = opt_param->chas;
    gint opt_type = opt_param->opt_type;
    if(CAN_SHOW_OPTS_PROPERTY(opt_type)) {
        return g_strdup_printf("%lf (s)", srv->slave_delay_recover_threshold_sec);
    }
    if(CAN_SAVE_OPTS_PROPERTY(opt_type)) {
        if(srv->slave_delay_recover_threshold_sec * 2 == srv->slave_delay_down_threshold_sec) {
            return NULL;
        }
        return g_strdup_printf("%lf", srv->slave_delay_recover_threshold_sec);
    }
    return NULL;
}

gint
assign_slave_delay_recover(const gchar *newval, gpointer param) {
    gint ret = ASSIGN_ERROR;
    struct external_param *opt_param = (struct external_param *)param;
    chassis *srv = opt_param->chas;
    gint opt_type = opt_param->opt_type;
    if(CAN_ASSIGN_OPTS_PROPERTY(opt_type)) {
        if(NULL != newval) {
            gdouble value = 0;
            if(try_get_double_value(newval, &value)) {
                if(value >= 0) {
                    srv->slave_delay_recover_threshold_sec = value < srv->slave_delay_down_threshold_sec ? value:srv->slave_delay_down_threshold_sec;
                    ret = ASSIGN_OK;
                } else {
                    ret = ASSIGN_VALUE_INVALID;
                }
            } else {
                ret = ASSIGN_VALUE_INVALID;
            }
        } else {
            ret = ASSIGN_VALUE_INVALID;
        }
    }
    return ret;
}

gchar*
show_default_query_cache_timeout(gpointer param) {
    struct external_param *opt_param = (struct external_param *)param;
    chassis *srv = opt_param->chas;
    gint opt_type = opt_param->opt_type;
    if(CAN_SHOW_OPTS_PROPERTY(opt_type)) {
        return g_strdup_printf("%d (ms)", srv->default_query_cache_timeout);
    }
    if(CAN_SAVE_OPTS_PROPERTY(opt_type)) {
        if(srv->default_query_cache_timeout == 100) {
            return NULL;
        }
        return g_strdup_printf("%d", srv->default_query_cache_timeout);
    }
    return NULL;
}

gint
assign_default_query_cache_timeout(const gchar *newval, gpointer param) {
    gint ret = ASSIGN_ERROR;
    struct external_param *opt_param = (struct external_param *)param;
    chassis *srv = opt_param->chas;
    gint opt_type = opt_param->opt_type;
    if(CAN_ASSIGN_OPTS_PROPERTY(opt_type)) {
        if(NULL != newval) {
            int value = 0;
            if(try_get_int_value(newval, &value)) {
                if(value >= 0) {
                    srv->default_query_cache_timeout = value;
                    ret = ASSIGN_OK;
                } else {
                    ret = ASSIGN_VALUE_INVALID;
                }
            } else {
                ret = ASSIGN_VALUE_INVALID;
            }
        } else {
            ret = ASSIGN_VALUE_INVALID;
        }
    }
    return ret;
}

gchar*
show_default_client_idle_timeout(gpointer param) {
    struct external_param *opt_param = (struct external_param *)param;
    chassis *srv = opt_param->chas;
    gint opt_type = opt_param->opt_type;
    if(CAN_SHOW_OPTS_PROPERTY(opt_type)) {
        return g_strdup_printf("%d (ms)", srv->client_idle_timeout);
    }
    if(CAN_SAVE_OPTS_PROPERTY(opt_type)) {
        if(srv->client_idle_timeout == 8 * HOURS) {
            return NULL;
        }
        return g_strdup_printf("%d", srv->client_idle_timeout);
    }
    return NULL;
}

gint
assign_default_client_idle_timeout(const gchar *newval, gpointer param) {
    gint ret = ASSIGN_ERROR;
    struct external_param *opt_param = (struct external_param *)param;
    chassis *srv = opt_param->chas;
    gint opt_type = opt_param->opt_type;
    if(CAN_ASSIGN_OPTS_PROPERTY(opt_type)) {
        if(NULL != newval) {
            int value = 0;
            if(try_get_int_value(newval, &value)) {
                if(value >= 0) {
                    srv->client_idle_timeout = value;
                    ret = ASSIGN_OK;
                } else {
                    ret = ASSIGN_VALUE_INVALID;
                }
            } else {
                ret = ASSIGN_VALUE_INVALID;
            }
        } else {
            ret = ASSIGN_VALUE_INVALID;
        }
    }
    return ret;
}

gchar* show_long_query_time(gpointer param) {
    struct external_param *opt_param = (struct external_param *)param;
    chassis *srv = opt_param->chas;
    gint opt_type = opt_param->opt_type;
    if(CAN_SHOW_OPTS_PROPERTY(opt_type)) {
        return g_strdup_printf("%d (ms)", srv->long_query_time);
    }
    if(CAN_SAVE_OPTS_PROPERTY(opt_type)) {
        if(srv->long_query_time == MAX_QUERY_TIME) {
            return NULL;
        }
        return g_strdup_printf("%d", srv->long_query_time);
    }
    return NULL;
}

gint
assign_long_query_time(const gchar *newval, gpointer param) {
    gint ret = ASSIGN_ERROR;
    struct external_param *opt_param = (struct external_param *)param;
    chassis *srv = opt_param->chas;
    gint opt_type = opt_param->opt_type;
    if(CAN_ASSIGN_OPTS_PROPERTY(opt_type)) {
        if(NULL != newval) {
            int value = 0;
            if(try_get_int_value(newval, &value)) {
                if(value >= 0) {
                    srv->long_query_time = value;
                    ret = ASSIGN_OK;
                } else {
                    ret = ASSIGN_VALUE_INVALID;
                }
            } else {
                ret = ASSIGN_VALUE_INVALID;
            }
        } else {
            ret = ASSIGN_VALUE_INVALID;
        }
    }
    return ret;
}

gchar*
show_enable_client_found_rows(gpointer param) {
    struct external_param *opt_param = (struct external_param *)param;
    chassis *srv = opt_param->chas;
    gint opt_type = opt_param->opt_type;
    if(CAN_SHOW_OPTS_PROPERTY(opt_type)) {
        return g_strdup_printf("%s", srv->client_found_rows ? "true" : "false");
    }
    if(CAN_SAVE_OPTS_PROPERTY(opt_type)) {
        return srv->client_found_rows ? g_strdup("true") : NULL;
    }
    return NULL;
}

gchar*
show_reduce_connections(gpointer param) {
    struct external_param *opt_param = (struct external_param *)param;
    chassis *srv = opt_param->chas;
    gint opt_type = opt_param->opt_type;
    if(CAN_SHOW_OPTS_PROPERTY(opt_type)) {
        return g_strdup_printf("%s", srv->is_reduce_conns ? "true" : "false");
    }
    if(CAN_SAVE_OPTS_PROPERTY(opt_type)) {
        return srv->is_reduce_conns ? g_strdup("true") : NULL;
    }
    return NULL;
}

gchar*
show_enable_query_cache(gpointer param) {
    struct external_param *opt_param = (struct external_param *)param;
    chassis *srv = opt_param->chas;
    gint opt_type = opt_param->opt_type;
    if(CAN_SHOW_OPTS_PROPERTY(opt_type)) {
        return g_strdup_printf("%s", srv->query_cache_enabled ? "true" : "false");
    }
    if(CAN_SAVE_OPTS_PROPERTY(opt_type)) {
        return srv->query_cache_enabled ? g_strdup("true") : NULL;
    }
    return NULL;
}

gchar*
show_enable_tcp_stream(gpointer param) {
    struct external_param *opt_param = (struct external_param *)param;
    chassis *srv = opt_param->chas;
    gint opt_type = opt_param->opt_type;
    if(CAN_SHOW_OPTS_PROPERTY(opt_type)) {
        return g_strdup_printf("%s", srv->is_tcp_stream_enabled ? "true" : "false");
    }
    if(CAN_SAVE_OPTS_PROPERTY(opt_type)) {
        return srv->is_tcp_stream_enabled ? g_strdup("true") : NULL;
    }
    return NULL;
}

gchar*
show_log_xa_in_detail(gpointer param) {
    struct external_param *opt_param = (struct external_param *)param;
    chassis *srv = opt_param->chas;
    gint opt_type = opt_param->opt_type;
    if(CAN_SHOW_OPTS_PROPERTY(opt_type)) {
        return g_strdup_printf("%s", srv->xa_log_detailed ? "true" : "false");
    }
    if(CAN_SAVE_OPTS_PROPERTY(opt_type)) {
        return srv->xa_log_detailed ? g_strdup("true") : NULL;
    }
    return NULL;
}

gchar*
show_disable_dns_cache(gpointer param) {
    struct external_param *opt_param = (struct external_param *)param;
    chassis *srv = opt_param->chas;
    gint opt_type = opt_param->opt_type;
    if(CAN_SHOW_OPTS_PROPERTY(opt_type)) {
        return g_strdup_printf("%s", srv->disable_dns_cache ? "true" : "false");
    }
    if(CAN_SAVE_OPTS_PROPERTY(opt_type)) {
        return srv->disable_dns_cache ? g_strdup("true") : NULL;
    }
    return NULL;
}

gchar*
show_master_preferred(gpointer param) {
    struct external_param *opt_param = (struct external_param *)param;
    chassis *srv = opt_param->chas;
    gint opt_type = opt_param->opt_type;
    if(CAN_SHOW_OPTS_PROPERTY(opt_type)) {
        return g_strdup_printf("%s", srv->master_preferred ? "true" : "false");
    }
    if(CAN_SAVE_OPTS_PROPERTY(opt_type)) {
        return srv->master_preferred ? g_strdup("true") : NULL;
    }
    return NULL;
}

gchar*
show_max_allowed_packet(gpointer param) {
    struct external_param *opt_param = (struct external_param *)param;
    chassis *srv = opt_param->chas;
    gint opt_type = opt_param->opt_type;
    if(CAN_SHOW_OPTS_PROPERTY(opt_type)) {
        return g_strdup_printf("%d", srv->cetus_max_allowed_packet);
    }
    if(CAN_SAVE_OPTS_PROPERTY(opt_type)) {
        if(srv->cetus_max_allowed_packet == MAX_ALLOWED_PACKET_DEFAULT) {
            return NULL;
        }
        return g_strdup_printf("%d", srv->cetus_max_allowed_packet);
    }
    return NULL;
}

gint
assign_max_allowed_packet(const gchar *newval, gpointer param) {
    gint ret = ASSIGN_ERROR;
    struct external_param *opt_param = (struct external_param *)param;
    chassis *srv = opt_param->chas;
    gint opt_type = opt_param->opt_type;
    if(CAN_ASSIGN_OPTS_PROPERTY(opt_type)) {
        if(NULL != newval) {
            int value = 0;
            if(try_get_int_value(newval, &value)) {
                if(value >= 0) {
                    srv->cetus_max_allowed_packet = value;
                    ret = ASSIGN_OK;
                } else {
                    ret = ASSIGN_VALUE_INVALID;
                }
            } else {
                ret = ASSIGN_VALUE_INVALID;
            }
        } else {
            ret = ASSIGN_VALUE_INVALID;
        }
    }
    return ret;
}

gchar*
show_remote_conf_url(gpointer param) {
    struct external_param *opt_param = (struct external_param *)param;
    chassis *srv = opt_param->chas;
    gint opt_type = opt_param->opt_type;
    if(CAN_SHOW_OPTS_PROPERTY(opt_type)) {
        return g_strdup_printf("%s", srv->remote_config_url != NULL ? srv->remote_config_url: "NULL");
    }
    return NULL;
}

gchar*
show_group_replication_mode(gpointer param) {
    struct external_param *opt_param = (struct external_param *)param;
    chassis *srv = opt_param->chas;
    gint opt_type = opt_param->opt_type;
    if(CAN_SHOW_OPTS_PROPERTY(opt_type)) {
        return g_strdup_printf("%d", srv->group_replication_mode);
    }
    if(CAN_SAVE_OPTS_PROPERTY(opt_type)) {
        if(srv->group_replication_mode == 0) {
            return NULL;
        } else {
            return g_strdup_printf("%d", srv->group_replication_mode);
        }
    }
    return NULL;
}

gint
assign_group_replication(const gchar *newval, gpointer param) {
    gint ret = ASSIGN_ERROR;
    struct external_param *opt_param = (struct external_param *)param;
    chassis *srv = opt_param->chas;
    gint opt_type = opt_param->opt_type;
    if(CAN_ASSIGN_OPTS_PROPERTY(opt_type)) {
        if(NULL != newval) {
            gint value = 0;
            if(try_get_int_value(newval, &value)) {
                if(value == 0 || value == 1) {
                    srv->group_replication_mode = value;
                    ret = ASSIGN_OK;
                } else {
                    ret = ASSIGN_VALUE_INVALID;
                }
            } else {
                ret = ASSIGN_VALUE_INVALID;
            }
        } else {
            ret = ASSIGN_VALUE_INVALID;
        }
    }
    return ret;
}

gint
chassis_options_save(GKeyFile *keyfile, chassis_options_t *opts, chassis  *chas)
{
    GList *node = NULL;
    gint effected_rows = 0;
    for (node = opts->options; node; node = node->next) {
        chassis_option_t *opt = node->data;
        if (opt->show_hook != NULL && CAN_SAVE_OPTS_PROPERTY(opt->opt_property)) {
            struct external_param *opt_param = (struct external_param *)g_new0(struct external_param, 1);
            opt_param->opt_type = SAVE_OPTS_PROPERTY;
            opt_param->chas = chas;
            gchar *value = NULL;

            if(opt->show_hook) {
                value = opt->show_hook(opt_param);
            }
            if (value != NULL) {
                g_key_file_set_value(keyfile, "cetus", opt->long_name, value);
                effected_rows++;
                g_free(value);
            }

            g_free(opt_param);
        }
    }
    return effected_rows;
}
