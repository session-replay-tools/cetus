#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif


#include <sys/resource.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <net/if.h>
#include <sched.h>
#include <sys/ioctl.h>

#include "chassis-sql-log.h"
#include "cetus-monitor.h"
#include "network-mysqld.h"
#include "cetus-channel.h"
#include "cetus-process.h"
#include "cetus-process-cycle.h"
#include "network-socket.h"
#include "chassis-event.h"
#include "chassis-frontend.h"
#include "glib-ext.h"

static void cetus_start_worker_processes(cetus_cycle_t *cycle, int n, int type);
static void cetus_pass_open_channel(cetus_cycle_t *cycle, cetus_channel_t *ch);
static void cetus_signal_worker_processes(cetus_cycle_t *cycle, int signo);
static unsigned int cetus_reap_children(cetus_cycle_t *cycle);
static unsigned int cetus_check_children(cetus_cycle_t *cycle);
static void cetus_master_process_exit(cetus_cycle_t *cycle);
static void cetus_worker_process_cycle(cetus_cycle_t *cycle, void *data);
static void cetus_worker_process_init(cetus_cycle_t *cycle, int worker);
static void cetus_admin_process_init(cetus_cycle_t *cycle);
static void cetus_worker_process_exit(cetus_cycle_t *cycle);
static void cetus_channel_handler(int fd, short events, void *user_data);


unsigned int    cetus_process;
unsigned int    cetus_worker;
cetus_pid_t     cetus_pid;
cetus_pid_t     cetus_parent;
cetus_pid_t     cetus_new_binary;

sig_atomic_t  cetus_reap;
sig_atomic_t  cetus_terminate;
sig_atomic_t  cetus_quit;
unsigned int  cetus_exiting;
sig_atomic_t  cetus_change_binary;

sig_atomic_t  cetus_noaccept;
unsigned int  cetus_restart;


static u_char  master_process[] = "master process";

static cetus_cycle_t      cetus_exit_cycle;


static int
open_plugins(cetus_cycle_t *cycle)
{
    int      i;

    for (i = 0; i < cycle->modules->len; i++) {
        chassis_plugin *p = cycle->modules->pdata[i];
        g_debug("%s: applying config of plugin %s", G_STRLOC, p->name);
        if (strcmp(p->name, "admin") == 0) {
            cycle->enable_admin_listen = 1;
            g_assert(p->apply_config);
            g_debug("%s: call apply_config", G_STRLOC);
            if (0 != p->apply_config(cycle, p->config)) {
                g_critical("%s: applying config of plugin %s failed", G_STRLOC, p->name);
                return -1;
            }
        }
    }

    return 0;
}


cetus_pid_t
cetus_exec_new_binary(cetus_cycle_t *cycle, char **argv)
{
    cetus_exec_ctx_t     ctx;

    ctx.path = argv[0];
    ctx.name = "new binary process";
    ctx.argv = argv;

    const char *env = getenv("LD_LIBRARY_PATH");
    ctx.envp = g_new0(char *, 2);
    char *new_env = g_new0(char, strlen(env) + 32);
    sprintf(new_env, "LD_LIBRARY_PATH=%s", env);

    ctx.envp[0] = (char *) new_env;
    ctx.envp[1] = NULL;

    if (cycle->old_pid_file == NULL) {
        int pid_file_len = strlen(cycle->pid_file);
        int len = pid_file_len + sizeof(CETUS_OLDPID_EXT) + 1;
        cycle->old_pid_file = g_new0(char, len);
        strncpy(cycle->old_pid_file, cycle->pid_file, pid_file_len);
        char *p = cycle->old_pid_file + pid_file_len;
        strncpy(p, CETUS_OLDPID_EXT, sizeof(CETUS_OLDPID_EXT));
    }

    if (cetus_rename_file(cycle->pid_file, cycle->old_pid_file) == -1) {
        g_critical("%s: rename file from %s to %s failed", G_STRLOC,
                cycle->pid_file, cycle->old_pid_file);
        return -1;
    }

    return cetus_execute(cycle, &ctx);
}

static void retrieve_user_from_remote(chassis_config_t* conf)
{
    struct config_object_t *object = chassis_config_get_object(conf, "users");
    if  (object) {
        if (chassis_config_mysql_query_object(conf, object, "users")) {
            conf->options_update_flag = 0;
            conf->options_success_flag = 1;
        } else {
            conf->options_success_flag = 0;
            conf->options_update_flag = 0;
        }
    } else {
        conf->options_success_flag = 0;
        conf->options_update_flag = 0;
    }
}

gpointer retrieve_remote_config_mainloop(gpointer user_data) {
    chassis *chas = user_data;
    chassis_config_t* conf = chas->config_manager;
    struct config_object_t *object;

    g_message("%s: retrieve_remote_config_mainloop visited", G_STRLOC);
    while(!chassis_is_shutdown()) {
        if (!conf->options_update_flag) {
            usleep(1000);
        } else {
            switch (chas->asynchronous_type) {
                case ASYNCHRONOUS_RELOAD:
                    chassis_config_load_options_mysql(conf);
                    break;
                case ASYNCHRONOUS_RELOAD_VARIABLES:
                    chassis_config_reload_variables(conf, "variables");
                    break;
                case ASYNCHRONOUS_RELOAD_USER:
                    retrieve_user_from_remote(conf);
                    break;
                case ASYNCHRONOUS_UPDATE_OR_DELETE_USER_PASSWORD:
                    object = chassis_config_get_object(conf, "users");
                    chassis_config_mysql_write_object(conf, object, "users", conf->user_data);
                    g_free(conf->user_data);
                    conf->user_data = NULL;
                    g_message("%s: ASYNCHRONOUS_UPDATE_OR_DELETE_USER_PASSWORD visited", G_STRLOC);
                    break;
                case ASYNCHRONOUS_CONFIG_REMOTE_SHARD:
                    object = chassis_config_get_object(conf, "sharding");
                    chassis_config_mysql_write_object(conf, object, "sharding", conf->user_data);
                    g_free(conf->user_data);
                    conf->user_data = NULL;
                    break;
                case ASYNCHRONOUS_SET_CONFIG:
                    g_message("%s: ASYNCHRONOUS_SET_CONFIG visited", G_STRLOC);
                    chassis_config_set_remote_options(conf, conf->key, conf->value);
                    break;
                case ASYNCHRONOUS_UPDATE_BACKENDS:
                    g_message("%s: ASYNCHRONOUS_UPDATE_BACKENDS visited", G_STRLOC);
                    chassis_config_set_remote_backends(conf, conf->key, conf->value, conf->reserve1, conf->reserve2);
                    break;
                default:
                    break;
            }
        }
    }

    return NULL;
}


static void
cetus_remote_config_start_thread(chassis *chas)
{
    g_message("%s: cetus_remote_config_start_thread visited", G_STRLOC);

    GThread *new_thread = NULL;
#if !GLIB_CHECK_VERSION(2, 32, 0)
    GError *error = NULL;
    new_thread = g_thread_create(retrieve_remote_config_mainloop, chas, TRUE, &error);
    if (new_thread == NULL && error != NULL) {
        g_critical("%s:Create thread error: %s", G_STRLOC, error->message);
        g_clear_error(&error);
    }
#else
    new_thread = g_thread_new("remote-config-thread", retrieve_remote_config_mainloop, chas);
    if (new_thread == NULL) {
        g_critical("%s:Create thread error.", G_STRLOC);
    }
#endif
}


void
cetus_master_process_cycle(cetus_cycle_t *cycle)
{
    int                try_cnt, mutex_set;
    unsigned int       live;

    cetus_pid = getpid();

    cycle->cpus = sysconf(_SC_NPROCESSORS_ONLN);
    cycle->active_worker_processes = cycle->worker_processes;

    cetus_start_worker_processes(cycle, cycle->worker_processes, CETUS_PROCESS_RESPAWN);

    if (open_plugins(cycle) == -1) {
        return;
    }

    if (cycle->pid_file) {
        GError *gerr = NULL;
        if (0 != chassis_frontend_write_pidfile(cycle->pid_file, &gerr)) {
            g_critical("%s", gerr->message);
            g_clear_error(&gerr);
            return;
        }
    }

    live = 1;
    try_cnt = 0;
    mutex_set = 0;

    for ( ;; ) {

        if (!cetus_terminate) {
            if (mutex_set) {
                cycle->socketpair_mutex = 0;
            }
            chassis_event_loop_t *loop = cycle->event_base;
            chassis_event_loop(loop, &(cycle->socketpair_mutex));
            cycle->socketpair_mutex = 1;
            mutex_set = 1;
        }

        if (cetus_terminate) {
            g_message("%s: call cetus_check_children", G_STRLOC);
            live = cetus_check_children(cycle);
            usleep(10 * 1000);
            try_cnt++;
        } else {
            if (cetus_reap) {
                if (cycle->config_changed) {
                    g_message("%s: config changed and no respawn process here", G_STRLOC);
                    cycle->active_worker_processes--;
                    if (cycle->active_worker_processes < 1) {
                        cetus_change_binary = 1;
                    }
                }
                g_message("%s: cetus_reap is true", G_STRLOC);
                cetus_reap = 0;
                cycle->current_time = time(0);
                if (cycle->child_exit_time == 0) {
                    cycle->child_exit_time = cycle->current_time;
                    cycle->child_instant_exit_times = 1;
                } else {
                    cycle->child_instant_exit_times++;
                    int diff = cycle->current_time - cycle->child_exit_time;
                    if (diff > 1) {
                        cycle->child_exit_time = 0;
                        g_message("%s: reset child_exit_time to zero", G_STRLOC);
                    } else {
                        if (cycle->child_instant_exit_times >= cetus_last_process) {
                            cetus_terminate = 1;
                            g_message("%s: set cetus_terminate is true:%d, workers:%d",
                                    G_STRLOC, cycle->child_instant_exit_times, cetus_last_process);
                        }
                    }
                }
                if (!cetus_terminate) {
                    live = cetus_reap_children(cycle);
                }
            }
        }

        if (!live && (cetus_terminate || cetus_quit)) {
            cetus_master_process_exit(cycle);
        }

        if (cetus_terminate) {
            if (live && try_cnt >= 10) {
                try_cnt = 0;
                if (cycle->worker_processes) {
                    cetus_signal_worker_processes(cycle,
                            cetus_signal_value(CETUS_TERMINATE_SIGNAL));
                }
            }
            continue;
        }

        if (cetus_quit) {
            if (cycle->worker_processes) {
                cetus_signal_worker_processes(cycle,
                        cetus_signal_value(CETUS_SHUTDOWN_SIGNAL));
            }
            live = 0;
            continue;
        }

        if (cetus_restart) {
            cetus_restart = 0;
            cetus_start_worker_processes(cycle, cycle->worker_processes, CETUS_PROCESS_RESPAWN);
            open_plugins(cycle);
    
            live = 1;
        }

        if (cetus_noaccept) {
            g_message("%s: cetus_noaccept is set true", G_STRLOC);
            cetus_noaccept = 0;

            int i;
            for (i = 0; i < cycle->modules->len; i++) {
                chassis_plugin *p = cycle->modules->pdata[i];
                p->stop_listening(cycle, p->config);
            }

            if (cycle->worker_processes) {
                cetus_signal_worker_processes(cycle, cetus_signal_value(CETUS_NOACCEPT_SIGNAL));
            }
        }

        if (cetus_change_binary) {
            g_message("%s: changing binary", G_STRLOC);
#if defined(SO_REUSEPORT)
            unlink(cycle->unix_socket_name);
            g_free(cycle->unix_socket_name);
            cycle->unix_socket_name = NULL;
#endif
            cetus_new_binary = cetus_exec_new_binary(cycle, cycle->argv);
            cetus_change_binary = 0;
	    if (cycle->active_worker_processes > 0) {
                g_message("%s: changing binary when active processes:%d", G_STRLOC, cycle->active_worker_processes);
	    } else {
                cetus_quit = 1;
	    }
        }
    }
}

static void
cetus_start_worker_processes(cetus_cycle_t *cycle, int n, int type)
{
    int      i;
    cetus_channel_t  ch;

    memset(&ch, 0, sizeof(cetus_channel_t));

    ch.basics.command = CETUS_CMD_OPEN_CHANNEL;

    for (i = 0; i < n; i++) {
        g_debug("%s: before call cetus_spawn_process", G_STRLOC);
        cetus_spawn_process(cycle, cetus_worker_process_cycle,
                          (void *) (intptr_t) i, "worker process", type);

        ch.basics.pid = cetus_processes[cetus_process_slot].pid;
        ch.basics.slot = cetus_process_slot;
        ch.basics.fd = cetus_processes[cetus_process_slot].parent_child_channel[0];

        g_debug("%s: call cetus_pass_open_channel, cetus_process_slot:%d, pid:%d, fd:%d", 
                G_STRLOC, cetus_process_slot, ch.basics.pid, ch.basics.fd);
        cetus_pass_open_channel(cycle, &ch);
    }
}


static void
cetus_pass_open_channel(cetus_cycle_t *cycle, cetus_channel_t *ch)
{
    int  i;

    g_debug("%s: call cetus_pass_open_channel, cetus_last_process:%d",
            G_STRLOC, cetus_last_process);

    for (i = 0; i < cetus_last_process; i++) {

        g_debug("%s: i:%d,pid:%d,fd:%d ",
            G_STRLOC, i, cetus_processes[i].pid, cetus_processes[i].parent_child_channel[0]);


        if (i == cetus_process_slot
            || cetus_processes[i].pid == -1
            || cetus_processes[i].parent_child_channel[0] == -1)
        {
            continue;
        }

        g_message("%s: pass channel s:%i pid:%d fd:%d to s:%i pid:%d fd:%d, ev base:%p, ev:%p", G_STRLOC,
                ch->basics.slot, ch->basics.pid, ch->basics.fd,
                i, cetus_processes[i].pid,
                cetus_processes[i].parent_child_channel[0], cycle->event_base, &cetus_channel_event);

        /* TODO: AGAIN */
        cetus_write_channel(cetus_processes[i].parent_child_channel[0],
                          ch, sizeof(cetus_channel_mininum_t));
    }
}


static void
cetus_signal_worker_processes(cetus_cycle_t *cycle, int signo)
{
    int      i;
    int      err;
    cetus_channel_t  ch;

    memset(&ch, 0, sizeof(cetus_channel_t));

    switch (signo) {

    case cetus_signal_value(CETUS_SHUTDOWN_SIGNAL):
        ch.basics.command = CETUS_CMD_QUIT;
        break;

    case cetus_signal_value(CETUS_TERMINATE_SIGNAL):
        ch.basics.command = CETUS_CMD_TERMINATE;
        break;

    default:
        ch.basics.command = 0;
    }


    ch.basics.fd = -1;

    for (i = 0; i < cetus_last_process; i++) {

        g_debug("%s: child: %i %d e:%d t:%d d:%d r:%d j:%d", G_STRLOC,
                i,
                cetus_processes[i].pid,
                cetus_processes[i].exiting,
                cetus_processes[i].exited,
                cetus_processes[i].detached,
                cetus_processes[i].respawn,
                cetus_processes[i].just_spawn);

        if (cetus_processes[i].detached || cetus_processes[i].pid == -1) {
            continue;
        }

        if (cetus_processes[i].just_spawn) {
            cetus_processes[i].just_spawn = 0;
            continue;
        }

        if (cetus_processes[i].exiting
            && signo == cetus_signal_value(CETUS_SHUTDOWN_SIGNAL))
        {
            continue;
        }

        if (ch.basics.command) {
            g_debug("%s: call cetus_pass_open_channel", G_STRLOC);
            if (cetus_write_channel(cetus_processes[i].parent_child_channel[0],
                                  &ch, sizeof(cetus_channel_mininum_t))
                == NETWORK_SOCKET_SUCCESS)
            {
                cetus_processes[i].exiting = 1;
                continue;
            }
        }

        g_debug("%s: kill (%d, %d)", G_STRLOC, cetus_processes[i].pid, signo);

        if (kill(cetus_processes[i].pid, signo) == -1) {
            err = errno;
            g_critical("%s: kill (%d, %d) failed", G_STRLOC, cetus_processes[i].pid, signo);

            if (err == ESRCH) {
                cetus_processes[i].exited = 1;
                cetus_processes[i].exiting = 0;
                if (!cycle->config_changed) {
                    g_message("%s: set reap true", G_STRLOC);
                    cetus_reap = 1;
                }
            }

            continue;
        }

        if (signo != cetus_signal_value(CETUS_REOPEN_SIGNAL)) {
            cetus_processes[i].exiting = 1;
        }
    }
}

static unsigned int
cetus_check_children(cetus_cycle_t *cycle)
{
    int           i;
    unsigned int  live;

    live = 0;
    for (i = 0; i < cetus_last_process; i++) {

        g_debug("%s: child: %i %d e:%d t:%d d:%d r:%d j:%d", G_STRLOC,
                       i,
                       cetus_processes[i].pid,
                       cetus_processes[i].exiting,
                       cetus_processes[i].exited,
                       cetus_processes[i].detached,
                       cetus_processes[i].respawn,
                       cetus_processes[i].just_spawn);

        if (cetus_processes[i].pid == -1) {
            continue;
        }

        if (cetus_processes[i].exited) {
            continue;
        }

        if (cetus_processes[i].exiting || !cetus_processes[i].detached) {
            live = 1;
        }
    }

    return live;
}


static unsigned int
cetus_reap_children(cetus_cycle_t *cycle)
{
    int         i, n;
    unsigned int        live;
    cetus_channel_t     ch;

    memset(&ch, 0, sizeof(cetus_channel_t));

    ch.basics.command = CETUS_CMD_CLOSE_CHANNEL;
    ch.basics.fd = -1;

    live = 0;
    for (i = 0; i < cetus_last_process; i++) {

        g_message("%s: child: %i %d e:%d t:%d d:%d r:%d j:%d", G_STRLOC,
                       i,
                       cetus_processes[i].pid,
                       cetus_processes[i].exiting,
                       cetus_processes[i].exited,
                       cetus_processes[i].detached,
                       cetus_processes[i].respawn,
                       cetus_processes[i].just_spawn);

        if (cetus_processes[i].pid == -1) {
            continue;
        }

        if (cetus_processes[i].exited) {

            if (cycle->config_changed) {
                cetus_processes[i].respawn = 0;
            }
            if (!cetus_processes[i].detached || cycle->config_changed) {
                cetus_close_channel(cetus_processes[i].parent_child_channel);

                cetus_processes[i].parent_child_channel[0] = -1;
                cetus_processes[i].parent_child_channel[1] = -1;

                ch.basics.pid = cetus_processes[i].pid;
                ch.basics.slot = i;

                for (n = 0; n < cetus_last_process; n++) {
                    if (cetus_processes[n].exited
                        || cetus_processes[n].pid == -1
                        || cetus_processes[n].parent_child_channel[0] == -1)
                    {
                        continue;
                    }

                    g_message("%s: pass close channel s:%i pid:%d to:%d", G_STRLOC,
                                   ch.basics.slot, ch.basics.pid, cetus_processes[n].pid);

                    /* TODO: AGAIN */
                    cetus_write_channel(cetus_processes[n].parent_child_channel[0],
                                      &ch, sizeof(cetus_channel_mininum_t));
                }
            }

            if (cetus_processes[i].respawn
                && !cetus_processes[i].exiting
                && !cetus_terminate
                && !cetus_quit)
            {

                usleep(1000 * 1000);
                g_debug("%s: before call cetus_spawn_process", G_STRLOC);
                if (cetus_spawn_process(cycle, cetus_processes[i].proc,
                                      cetus_processes[i].data,
                                      cetus_processes[i].name, i)
                    == CETUS_INVALID_PID)
                {
                    g_critical("%s: could not respawn %s", G_STRLOC, cetus_processes[i].name);
                    continue;
                }


                ch.basics.command = CETUS_CMD_OPEN_CHANNEL;
                ch.basics.pid = cetus_processes[cetus_process_slot].pid;
                ch.basics.slot = cetus_process_slot;
                ch.basics.fd = cetus_processes[cetus_process_slot].parent_child_channel[0];

                g_debug("%s: call cetus_pass_open_channel, slot:%d", G_STRLOC, cetus_process_slot);
                cetus_pass_open_channel(cycle, &ch);

                live = 1;

                continue;
            }

            if (i == cetus_last_process - 1) {
                g_message("%s: cetus_last_process sub,orig:%d", G_STRLOC, cetus_last_process);
                cetus_last_process--;

            } else {
                cetus_processes[i].pid = -1;
            }

        } else if (cetus_processes[i].exiting || !cetus_processes[i].detached) {
            live = 1;
        }
    }

    return live;
}


static void
cetus_master_process_exit(cetus_cycle_t *cycle)
{
#if defined(SO_REUSEPORT)
    if (cycle->unix_socket_name) {
        unlink(cycle->unix_socket_name);
    }
#endif

    g_message("%s: exit", G_STRLOC);

    exit(0);
}



static void
cetus_worker_process_cycle(cetus_cycle_t *cycle, void *data)
{

    g_message("%s: call cetus_worker_process_cycle", G_STRLOC);

    int worker = (intptr_t) data;

    cetus_process = CETUS_PROCESS_WORKER;
    cetus_worker = worker;

    cetus_worker_process_init(cycle, worker);

    int i;
    for (i = 0; i < cycle->modules->len; i++) {
        chassis_plugin *p = cycle->modules->pdata[i];
        g_assert(p->apply_config);
        g_message("%s: call apply_config", G_STRLOC);
        if (0 != p->apply_config(cycle, p->config)) {
            g_critical("%s: applying config of plugin %s failed", G_STRLOC, p->name);
        }
    }

    cycle->priv->thread_id = 1 + (cetus_last_process << 24);
    cycle->priv->max_thread_id = (cetus_last_process << 24) + (1 << 24) - 1;
    g_message("%s: first thread id:%d, max thread id:%d", G_STRLOC,
            cycle->priv->thread_id, cycle->priv->max_thread_id);

#ifndef SIMPLE_PARSER
    cycle->dist_tran_id = g_random_int_range(0, 100000000);
    struct ifreq buffer;
    int s = socket(PF_INET, SOCK_DGRAM, 0);
    if (s == -1) {
        g_message("%s: socket error:%s", G_STRLOC, strerror(errno));
        exit(0);
    }
    memset(&buffer, 0, sizeof(buffer));
    strncpy(buffer.ifr_name, cycle->ifname, IFNAMSIZ - 1);
    ioctl(s, SIOCGIFHWADDR, &buffer);
    close(s);

    char mac[32];

    sprintf(mac, "%02x:%02x:%02x:%02x:%02x:%02x",
            (unsigned char) buffer.ifr_hwaddr.sa_data[0],
            (unsigned char) buffer.ifr_hwaddr.sa_data[1],
            (unsigned char) buffer.ifr_hwaddr.sa_data[2],
            (unsigned char) buffer.ifr_hwaddr.sa_data[3],
            (unsigned char) buffer.ifr_hwaddr.sa_data[4],
            (unsigned char) buffer.ifr_hwaddr.sa_data[5]);

    if (strcmp(mac, "00:00:00:00:00:00") == 0) {
        snprintf(cycle->dist_tran_prefix, MAX_DIST_TRAN_PREFIX, "clt-%d-%s-%d",
                cycle->guid_state.worker_id, cycle->proxy_address, getpid());
        g_critical("wrong inferface name:%s", cycle->ifname);
    } else {
        snprintf(cycle->dist_tran_prefix, MAX_DIST_TRAN_PREFIX, "clt-%s-%s-%d",
                mac, cycle->proxy_address, getpid());
    }

    g_message("Initial dist_tran_id:%llu", cycle->dist_tran_id);
    g_message("dist_tran_prefix:%s, process id:%d", cycle->dist_tran_prefix, cetus_process_id);
    incremental_guid_init(&(cycle->guid_state));
#endif

#ifdef BPF_ENABLED
    if (cycle->cpus > 0) {
        cpu_set_t cpu_set;
        memset(&cpu_set, 0, sizeof(cpu_set));
        int cpu_ndx = cycle->guid_state.worker_id % cycle->cpus;
        CPU_SET(cpu_ndx, &cpu_set);
        if (sched_setaffinity(0, sizeof(cpu_set), &cpu_set) < 0) {
            g_critical("%s: failed to pin to cpu:%s, cpu index:%d",
                    G_STRLOC, strerror(errno), cpu_ndx);
        }
    }
#endif

    cetus_monitor_start_thread(cycle->priv->monitor, cycle);
    cetus_remote_config_start_thread(cycle);
    cetus_sql_log_start_thread_once(cycle->sql_mgr);


    for ( ;; ) {

        if (cetus_exiting) {
            cetus_worker_process_exit(cycle);
        }

        if (cetus_reap) {
            cetus_reap = 0;
            g_message("%s: cetus reap is true for child", G_STRLOC);
        }

        g_debug("%s: worker cycle", G_STRLOC);

        /* call main procedures for worker */
        chassis_event_loop_t *loop = cycle->event_base;
        chassis_event_loop(loop, NULL);

        if (cetus_terminate) {
            g_message("%s: exiting", G_STRLOC);
            cetus_worker_process_exit(cycle);
        }

        if (cetus_noaccept) {
            g_message("%s: cetus_noaccept is set true", G_STRLOC);
            cetus_noaccept = 0;
            for (i = 0; i < cycle->modules->len; i++) {
                chassis_plugin *p = cycle->modules->pdata[i];
                p->stop_listening(cycle, p->config);
            }

            cycle->maintain_close_mode = 1;
        }

        if (cetus_quit) {
            cetus_quit = 0;
            g_message("%s: gracefully shutting down", G_STRLOC);

            if (!cetus_exiting) {
                cetus_exiting = 1;
                /* Call cetus shut down */
            }
        }
    }
}


static void
cetus_worker_process_init(cetus_cycle_t *cycle, int worker)
{
    sigset_t           set;
    int                n;

    sigemptyset(&set);

    if (sigprocmask(SIG_SETMASK, &set, NULL) == -1) {
        g_critical("%s: sigprocmask() failed, errno:%d", G_STRLOC, errno);
    }

    g_debug("%s: cetus_last_process:%d", G_STRLOC, cetus_last_process);

    for (n = 0; n < cetus_last_process; n++) {

        if (cetus_processes[n].pid == -1) {
            continue;
        }

        if (n == cetus_process_slot) {
            continue;
        }

        if (cetus_processes[n].parent_child_channel[1] == -1) {
            continue;
        }

        g_debug("%s: close() channel one fd:%d, n:%d", 
                G_STRLOC, cetus_processes[n].parent_child_channel[1], n);

        if (close(cetus_processes[n].parent_child_channel[1]) == -1) {
            g_critical("%s: close() channel failed, err:%s", G_STRLOC, strerror(errno));
        }
    }

    g_debug("%s: close() channel zero fd:%d, n:%d", 
            G_STRLOC, cetus_processes[cetus_process_slot].parent_child_channel[0], cetus_process_slot);
    if (close(cetus_processes[cetus_process_slot].parent_child_channel[0]) == -1) {
        g_critical("%s: close() channel failed, strerr:%s", G_STRLOC, strerror(errno));
    }

    g_debug("%s: channel fd for recving:%d, n:%d", 
            G_STRLOC, cetus_processes[cetus_process_slot].parent_child_channel[1], cetus_process_slot);

    chassis_event_loop_t *mainloop = chassis_event_loop_new();
    cycle->event_base = mainloop;
    g_assert(cycle->event_base);

    event_set(&cetus_channel_event, cetus_channel, EV_READ | EV_PERSIST, cetus_channel_handler, cycle);
    chassis_event_add(cycle, &cetus_channel_event);
    g_debug("%s: cetus_channel:%d is waiting for read, event base:%p, ev:%p",
            G_STRLOC, cetus_channel, cycle->event_base, &cetus_channel_event);

}


static void
cetus_worker_process_exit(cetus_cycle_t *cycle)
{
    cetus_monitor_stop_thread(cycle->priv->monitor);

    g_message("%s: exit", G_STRLOC);

    exit(0);
}

static
cetus_channel_t *retrieve_admin_resp(network_mysqld_con *con)
{
    GList *chunk;
    g_debug("%s:call retrieve_admin_resp", G_STRLOC);
    int total = sizeof(cetus_channel_t); 
    int resp_len = 0;
    for (chunk = con->client->send_queue->chunks->head; chunk; chunk = chunk->next) {
        GString *s = chunk->data;
        resp_len += s->len; 
        g_debug("%s:s->len:%d, resp len:%d", G_STRLOC, (int) s->len, resp_len);
    }

    total = total + resp_len;

    cetus_channel_t *ch = (cetus_channel_t *) g_new0(char, total);
    ch->admin_sql_resp_len = resp_len;

    unsigned char *p = ch->admin_sql_resp;
    for (chunk = con->client->send_queue->chunks->head; chunk; chunk = chunk->next) {
        GString *s = chunk->data;
        memcpy(p, s->str, s->len);
        p = p + s->len;
    }

    g_debug("%s:call retrieve_admin_resp end", G_STRLOC);
    
    return ch;
}


void send_admin_resp(chassis *cycle, network_mysqld_con *con)
{
    if (cycle->worker_processes) {
        g_debug("%s:call send_admin_resp, cetus_process_slot:%d", G_STRLOC, cetus_process_slot);
        cetus_channel_t  *ch = retrieve_admin_resp(con); 
        ch->basics.command = CETUS_CMD_ADMIN_RESP;
        ch->basics.pid = cetus_processes[cetus_process_slot].pid;
        ch->basics.slot = cetus_process_slot;
        ch->basics.fd = cetus_processes[cetus_process_slot].parent_child_channel[1];

        g_debug("%s:send resp to admin, cetus_process_slot:%d", G_STRLOC, cetus_process_slot);
        g_debug("%s: pass sql resp channel s:%i pid:%d to:%d, fd:%d", G_STRLOC,
                ch->basics.slot, ch->basics.pid, cetus_processes[cetus_process_slot].pid,
                cetus_processes[cetus_process_slot].parent_child_channel[1]);

        /* TODO: AGAIN */
        cetus_write_channel(cetus_processes[cetus_process_slot].parent_child_channel[1],
                ch, sizeof(*ch) + ch->admin_sql_resp_len);
        g_debug("%s:cetus_write_channel send:%d", G_STRLOC, (int) (sizeof(*ch) + ch->admin_sql_resp_len));
        g_free(ch);

        network_queue_clear(con->client->send_queue);
        g_free(con->plugin_con_state);
        con->plugin_con_state = NULL;
        network_mysqld_con_free(con);
    } else {
        con->state = ST_SEND_QUERY_RESULT;
        network_mysqld_con_handle(-1, 0, con);
    }
}


static void
process_admin_sql(cetus_cycle_t *cycle, cetus_channel_t *ch)
{
    network_mysqld_con *con = network_mysqld_con_new();
    con->plugin_con_state = g_new0(int, 1);
    network_socket *client = network_socket_new();
    con->client = client;
    con->srv = cycle;

    g_string_assign_len(con->orig_sql, ch->admin_sql, strlen(ch->admin_sql));

    g_debug("%s: call process_admin_sql", G_STRLOC);
    if (cycle->admin_plugin) {
        g_debug("%s: call admin", G_STRLOC);
        network_socket_retval_t retval = NETWORK_SOCKET_SUCCESS;
        NETWORK_MYSQLD_PLUGIN_FUNC(func) = NULL;
        network_mysqld_hooks *plugin = cycle->admin_plugin;

        con->client->last_packet_id = 0;
        con->client->packet_id_is_reset = FALSE;

        func = plugin->con_exectute_sql;
        retval = (*func) (cycle, con);
        g_debug("%s: call admin:%d", G_STRLOC, retval);
        if (!con->is_admin_waiting_resp) {
            send_admin_resp(cycle, con);
        }
    } else {
        g_free(con->plugin_con_state);
        con->plugin_con_state = NULL;
        network_mysqld_con_free(con);
    }
}

static void
cetus_channel_handler(int fd, short events, void *user_data)
{
    cetus_channel_t    ch;

    g_debug("%s: channel handler, cetus_last_process:%d", G_STRLOC, cetus_last_process);

    do {

        g_debug("%s: before cetus_read_channel channel, fd:%d", G_STRLOC, fd);
        int ret = cetus_read_channel(fd, &ch, sizeof(cetus_channel_t));
        g_debug("%s: after cetus_read_channel channel, fd:%d, ret:%d", G_STRLOC, fd, ret);

        if (ret == NETWORK_SOCKET_ERROR) {
            g_debug("%s: error, fd:%d, ret:%d", G_STRLOC, fd, ret);
            cetus_terminate = 1;
            closesocket(fd);
            return;
        }

        if (ret == NETWORK_SOCKET_WAIT_FOR_EVENT) {
            g_debug("%s: wait for event, fd:%d, ret:%d", G_STRLOC, fd, ret);
            return;
        }

        g_debug("%s: channel command: %u", G_STRLOC, ch.basics.command);

        switch (ch.basics.command) {

        case CETUS_CMD_ADMIN:
            process_admin_sql(user_data, &ch);
            break;
        case CETUS_CMD_QUIT:
            cetus_quit = 1;
            break;

        case CETUS_CMD_TERMINATE:
            cetus_terminate = 1;
            break;

        case CETUS_CMD_OPEN_CHANNEL:

            g_debug("%s: get channel s:%i pid:%d fd:%d", G_STRLOC, 
                    ch.basics.slot, ch.basics.pid, ch.basics.fd);

            cetus_processes[ch.basics.slot].pid = ch.basics.pid;
            cetus_processes[ch.basics.slot].parent_child_channel[0] = ch.basics.fd;
            break;

        case CETUS_CMD_CLOSE_CHANNEL:

            g_debug("%s: close channel s:%i pid:%d our:%d fd:%d", G_STRLOC, 
                    ch.basics.slot, ch.basics.pid, cetus_processes[ch.basics.slot].pid,
                    cetus_processes[ch.basics.slot].parent_child_channel[0]);

            if (close(cetus_processes[ch.basics.slot].parent_child_channel[0]) == -1) {
                g_critical("%s: close() channel failed:%d", G_STRLOC, errno);
            }

            cetus_processes[ch.basics.slot].parent_child_channel[0] = -1;
            break;
        }
    } while (!chassis_is_shutdown());
}

