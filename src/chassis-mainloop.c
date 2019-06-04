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

#include <sys/types.h>
#include <signal.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>           /* event.h need struct timeval */
#endif

#ifdef HAVE_PWD_H
#include <pwd.h>                /* getpwnam() */
#endif
#include <sys/socket.h>         /* for SOCK_STREAM and AF_UNIX/AF_INET */

#include <glib.h>

#include "glib-ext.h"
#include "network-mysqld.h"
#include "chassis-plugin.h"
#include "chassis-mainloop.h"
#include "chassis-event.h"
#include "chassis-log.h"
#include "chassis-timings.h"
#include "cetus-process-cycle.h"
#include "chassis-sql-log.h"

static volatile sig_atomic_t signal_shutdown;
extern int cetus_process_id;

/**
 * check if the libevent headers we built against match the
 * library we run against
 */
int
chassis_check_version(const char *lib_version, const char *hdr_version)
{
    int lib_maj, lib_min;
    int hdr_maj, hdr_min;
    int scanned_fields;

    if (2 != (scanned_fields = sscanf(lib_version, "%d.%d", &lib_maj, &lib_min))) {
        g_critical("%s: library version %s failed to parse: %d", G_STRLOC, lib_version, scanned_fields);
        return -1;
    }
    if (2 != (scanned_fields = sscanf(hdr_version, "%d.%d", &hdr_maj, &hdr_min))) {
        g_critical("%s: header version %s failed to parse: %d", G_STRLOC, hdr_version, scanned_fields);
        return -1;
    }

    if (lib_maj == hdr_maj && lib_min == hdr_min) {
        return 0;
    }

    return -1;
}

/**
 * create a global context
 */
chassis *
chassis_new()
{
    chassis *chas;

    if (0 != chassis_check_version(event_get_version(), _EVENT_VERSION)) {
        g_critical("%s: chassis is built against libevent %s, but now runs against %s",
                   G_STRLOC, _EVENT_VERSION, event_get_version());
        return NULL;
    }

    chas = g_new0(chassis, 1);

    chas->modules = g_ptr_array_new();

    chas->event_hdr_version = g_strdup(_EVENT_VERSION);

    chas->shutdown_hooks = chassis_shutdown_hooks_new();

    chas->startup_time = time(0);

    chas->pid_file = NULL;
    chas->log_level = NULL;
    chas->log_xa_filename = NULL;
    chas->remote_config_url = NULL;
    chas->default_file = NULL;

    chas->group_replication_mode = 0;

    chas->sql_mgr = sql_log_alloc();

    return chas;
}

static void
g_queue_free_cache_index(gpointer q)
{
    GQueue *queue = q;

    query_cache_index_item *index;

    while ((index = g_queue_pop_head(queue))) {
        g_free(index->key);
        g_free(index);
    }

    g_queue_free(queue);
}

/**
 * free the global scope
 *
 * closes all open connections, cleans up all plugins
 *
 * @param chas      global context
 */
void
chassis_free(chassis *chas)
{
    guint i;
#ifdef HAVE_EVENT_BASE_FREE
    const char *version;
#endif

    if (!chas)
        return;

    /* init the shutdown, without freeing share structures */
    if (chas->priv_shutdown)
        chas->priv_shutdown(chas, chas->priv);

    /* call the destructor for all plugins */
    for (i = 0; i < chas->modules->len; i++) {
        chassis_plugin *p = g_ptr_array_index(chas->modules, i);

        g_assert(p->destroy);
        p->destroy(chas, p->config);
    }

    /* cleanup the global 3rd party stuff before we unload the modules */
    chassis_shutdown_hooks_call(chas->shutdown_hooks);

    for (i = 0; i < chas->modules->len; i++) {
        chassis_plugin *p = chas->modules->pdata[i];

        chassis_plugin_free(p);
    }

    g_ptr_array_free(chas->modules, TRUE);

    if (chas->base_dir)
        g_free(chas->base_dir);
    if (chas->conf_dir)
        g_free(chas->conf_dir);
    if (chas->plugin_dir)
        g_free(chas->plugin_dir);
    if (chas->user)
        g_free(chas->user);
    if (chas->default_db)
        g_free(chas->default_db);
    if (chas->ifname)
        g_free(chas->ifname);
    if (chas->default_username)
        g_free(chas->default_username);
    if (chas->default_hashed_pwd)
        g_free(chas->default_hashed_pwd);
    if (chas->query_cache_table)
        g_hash_table_destroy(chas->query_cache_table);
    if (chas->cache_index)
        g_queue_free_cache_index(chas->cache_index);
    if  (chas->unix_socket_name) {
        g_free(chas->unix_socket_name);
    }
    if (chas->trx_isolation_level) {
        g_free(chas->trx_isolation_level);
    }
    if (chas->default_charset) {
       g_free(chas->default_charset);
    }

    g_free(chas->event_hdr_version);

    chassis_shutdown_hooks_free(chas->shutdown_hooks);

    if (chas->priv_finally_free_shared)
        chas->priv_finally_free_shared(chas, chas->priv);

    /* free the pointers _AFTER_ the modules are shutdown */
    if (chas->priv_free)
        chas->priv_free(chas, chas->priv);
#ifdef HAVE_EVENT_BASE_FREE
    /* only recent versions have this call */

    version = event_get_version();

    /* libevent < 1.3e doesn't cleanup its own fds from the event-queue in signal_init()
     * calling event_base_free() would cause a assert() on shutdown
     */
    if (version && (strcmp(version, "1.3e") >= 0)) {
        if (chas->event_base) {
            event_base_free(chas->event_base);
        }
    }
#endif
    if (chas->config_manager)
        chassis_config_free(chas->config_manager);

    if(chas->pid_file) {
        g_free(chas->pid_file);
    }

    if (chas->old_pid_file) {
        g_free(chas->old_pid_file);
    }

    if(chas->log_level) {
        g_free(chas->log_level);
    }

    if (chas->plugin_names) {
        g_strfreev(chas->plugin_names);
    }

    if(chas->log_xa_filename) {
        g_free(chas->log_xa_filename);
    }

    if(chas->remote_config_url) {
        g_free(chas->remote_config_url);
    }

    if(chas->default_file) {
        g_free(chas->default_file);
    }

    if(chas->sql_mgr) {
        sql_log_free(chas->sql_mgr);
    }

    if (chas->argv) {
        for (i = 0; i < chas->argc; i++) {
            free(chas->argv[i]);
        }
        free(chas->argv);
    }

    g_free(chas);
}

void
chassis_set_shutdown_location(const gchar *location)
{
    if (signal_shutdown == 0) {
        g_message("Initiating shutdown, requested from %s", (location != NULL ? location : "signal handler"));
    }
    signal_shutdown = 1;
    cetus_terminate = 1;
}

gboolean
chassis_is_shutdown()
{
    return signal_shutdown == 1 || cetus_terminate == 1;
}

/**
 * forward libevent messages to the glib error log
 */
static void
event_log_use_glib(int libevent_log_level, const char *msg)
{
    /* map libevent to glib log-levels */

    GLogLevelFlags glib_log_level = G_LOG_LEVEL_DEBUG;

    if (libevent_log_level == _EVENT_LOG_DEBUG)
        glib_log_level = G_LOG_LEVEL_DEBUG;
    else if (libevent_log_level == _EVENT_LOG_MSG)
        glib_log_level = G_LOG_LEVEL_MESSAGE;
    else if (libevent_log_level == _EVENT_LOG_WARN)
        glib_log_level = G_LOG_LEVEL_WARNING;
    else if (libevent_log_level == _EVENT_LOG_ERR)
        glib_log_level = G_LOG_LEVEL_CRITICAL;

    g_log(G_LOG_DOMAIN, glib_log_level, "(libevent) %s", msg);
}

int
chassis_mainloop(void *_chas)
{
    chassis *chas = _chas;

    /* redirect logging from libevent to glib */
    event_set_log_callback(event_log_use_glib);

    /* add a event-handler for the "main" events */
    chassis_event_loop_t *mainloop = chassis_event_loop_new();
    chas->event_base = mainloop;
    g_assert(chas->event_base);

    /*
     * drop root privileges if requested
     */
    if (chas->user) {
        struct passwd *user_info;
        uid_t user_id = geteuid();

        /* Don't bother if we aren't superuser */
        if (user_id) {
            g_critical("can only use the --user switch if running as root");
            return -1;
        }

        if (NULL == (user_info = getpwnam(chas->user))) {
            g_critical("unknown user: %s", chas->user);
            return -1;
        }

        if (chas->log->log_filename) {
            /* chown logfile */
            if (-1 == chown(chas->log->log_filename, user_info->pw_uid, user_info->pw_gid)) {
                g_critical("%s: chown(%s) failed: %s", G_STRLOC, chas->log->log_filename, g_strerror(errno));

                return -1;
            }
        }

        if (setgid(user_info->pw_gid) == 0) {
            if (setuid(user_info->pw_uid) == 0) {
            }
        }
        g_debug("now running as user: %s (%d/%d)", chas->user, user_info->pw_uid, user_info->pw_gid);
    }

#if !GLIB_CHECK_VERSION(2, 32, 0)
    /* GLIB below 2.32 must call thread_init if multi threads */
    g_thread_init(NULL);
#endif

    if (cetus_init_signals() == -1 ) {
        return 1;
    }

    cetus_master_process_cycle(chas);

    return 0;
}

#ifndef SIMPLE_PARSER
uint64_t
incremental_guid_get_next(struct incremental_guid_state_t *s)
{
    uint64_t uniq_id = 0;
    static uint64_t SEQ_MASK = (-1L ^ (-1L << 10L));

    struct timeval tp;
    gettimeofday(&tp, NULL);
    unsigned int msec = tp.tv_usec / 1000;
    uint64_t cur_time = tp.tv_sec;

    uniq_id = cur_time << 32;
    uniq_id |= (msec << 22);
    uniq_id |= (s->worker_id & 0xfff) << 10;

    if (cur_time == s->last_sec && msec == s->last_msec) {
        s->seq_id = (s->seq_id + 1) & SEQ_MASK;
        if (s->seq_id == 0) {
            g_critical("%s:too many calls in one millisecond", G_STRLOC);
        }
    } else {
        s->seq_id = 0;
    }

    s->last_sec = cur_time;
    s->last_msec = msec;
    uniq_id |= s->seq_id;

    return uniq_id;
}

void
incremental_guid_init(struct incremental_guid_state_t *s)
{
    struct timeval tp;
    gettimeofday(&tp, NULL);

    s->worker_id = (s->worker_id << MAX_WORK_PROCESSES_SHIFT) + cetus_process_id;
    g_message("internal worker id:%d", s->worker_id);
    s->last_sec = tp.tv_sec;
    s->last_msec = tp.tv_usec;
    s->seq_id = 0;
}
#endif

