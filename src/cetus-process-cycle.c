#include <sys/resource.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include "network-mysqld.h"
#include "cetus-channel.h"
#include "cetus-process.h"
#include "cetus-process-cycle.h"
#include "network-socket.h"
#include "chassis-event.h"
#include "chassis-plugin.h"
#include "glib-ext.h"

static void cetus_start_worker_processes(cetus_cycle_t *cycle, int n, int type);
static void cetus_pass_open_channel(cetus_cycle_t *cycle, cetus_channel_t *ch);
static void cetus_signal_worker_processes(cetus_cycle_t *cycle, int signo);
static unsigned int cetus_reap_children(cetus_cycle_t *cycle);
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

sig_atomic_t  cetus_reap;
sig_atomic_t  cetus_sigio;
sig_atomic_t  cetus_sigalrm;
sig_atomic_t  cetus_terminate;
sig_atomic_t  cetus_quit;
sig_atomic_t  cetus_debug_quit;
unsigned int  cetus_exiting;
sig_atomic_t  cetus_reconfigure;
sig_atomic_t  cetus_reopen;

unsigned int  cetus_inherited;
unsigned int  cetus_daemonized;

sig_atomic_t  cetus_noaccept;
unsigned int  cetus_noaccepting;
unsigned int  cetus_restart;


static u_char  master_process[] = "master process";

static cetus_cycle_t      cetus_exit_cycle;

static void
cetus_admin_process_cycle(cetus_cycle_t *cycle, void *data)
{
    g_message("%s: cetus_admin_process_cycle, cetus_process_slot:%d, cetus_last_process:%d",
            G_STRLOC, cetus_process_slot, cetus_last_process);

    if (close(cetus_processes[cetus_process_slot].parent_child_channel[0]) == -1) {
        g_critical("%s: close() channel failed, errno:%d", G_STRLOC, errno);
    }

    /* close all worker admin channel 1 */

    int n;
    for (n = 0; n < cetus_last_process; n++) {

        if (cetus_processes[n].pid == -1) {
            continue;
        }

        if (n == cetus_process_slot) {
            continue;
        }

        if (cetus_processes[n].admin_worker_channel[1] == -1) {
            continue;
        }

        g_debug("%s: close() admin channel one fd:%d, n:%d", 
                G_STRLOC, cetus_processes[n].admin_worker_channel[1], n);

        
        if (close(cetus_processes[n].admin_worker_channel[1]) == -1) {
            g_critical("%s: close() channel failed, errno:%d", G_STRLOC, errno);
        }

    }

    cetus_process = CETUS_PROCESS_ADMIN;

    cetus_admin_process_init(cycle);

    int i;
    for (i = 0; i < cycle->modules->len; i++) {
        chassis_plugin *p = cycle->modules->pdata[i];
        if (strcmp(p->name, "admin") == 0) {
            cycle->enable_admin_listen = 1;
            g_assert(p->apply_config);
            g_message("%s: call apply_config", G_STRLOC);
            if (0 != p->apply_config(cycle, p->config)) {
                g_critical("%s: applying config of plugin %s failed", G_STRLOC, p->name);
            }
        }
    }

    for ( ;; ) {

        if (cetus_exiting) {
            cetus_worker_process_exit(cycle);
        }

        g_debug("%s: admin cycle", G_STRLOC);

        /* call main procedures for worker */
        chassis_event_loop_t *loop = cycle->event_base;
        chassis_event_loop(loop);
        g_debug("%s: after chassis_event_loop", G_STRLOC);

        if (cetus_terminate) {
            g_message("%s: exiting", G_STRLOC);
            cetus_worker_process_exit(cycle);
        }

        if (cetus_quit) {
            cetus_quit = 0;
            g_message("%s: gracefully shutting down", G_STRLOC);

            if (!cetus_exiting) {
                cetus_exiting = 1;
                /* Call cetus shut down */
            }
        }

        if (cetus_reopen) {
            cetus_reopen = 0;
            g_message("%s: reopening logs", G_STRLOC);
        }
    }
}



static void
cetus_start_admin_processes(cetus_cycle_t *cycle, int respawn)
{
    int      i;
    cetus_channel_t  ch;

    g_debug("%s: call cetus_start_admin_processes:%d", G_STRLOC, cetus_last_process);

    memset(&ch, 0, sizeof(cetus_channel_t));

    ch.basics.command = CETUS_CMD_OPEN_CHANNEL;

    cetus_spawn_admin_process(cycle, cetus_admin_process_cycle,
            NULL, "admin process", CETUS_PROCESS_RESPAWN);


    /* master closes all admin socketpair */
    int s;
    for (s = 0; s < cetus_last_process; s++) {
        if (cetus_processes[s].pid == -1) {
            break;
        }

        if (cetus_processes[s].is_admin) {
            g_debug("%s: is_admin true", G_STRLOC);
            continue;
        }

        g_debug("%s: close() channel one fd:%d, n:%d", 
                G_STRLOC, cetus_processes[s].admin_worker_channel[0], s);

        if (close(cetus_processes[s].admin_worker_channel[0]) == -1) {
            g_critical("%s: close() channel failed, errno:%d", G_STRLOC, errno);
        }

        g_debug("%s: close() channel one fd:%d, n:%d", 
                G_STRLOC, cetus_processes[s].admin_worker_channel[1], s);


        if (close(cetus_processes[s].admin_worker_channel[1]) == -1) {
            g_critical("%s: close() channel failed, errno:%d", G_STRLOC, errno);
        }
    }

    ch.basics.pid = cetus_processes[cetus_process_slot].pid;
    ch.basics.slot = cetus_process_slot;
    ch.basics.fd = cetus_processes[cetus_process_slot].parent_child_channel[0];

    g_debug("%s: call cetus_pass_open_channel", G_STRLOC);
    cetus_pass_open_channel(cycle, &ch);
}


void
cetus_master_process_cycle(cetus_cycle_t *cycle)
{
    u_char            *p;
    size_t             size;
    int                i;
    unsigned int       n, sigio;
    sigset_t           set;
    struct itimerval   itv;
    unsigned int       live;
    unsigned int       delay;

    sigemptyset(&set);
    sigaddset(&set, SIGCHLD);
    sigaddset(&set, SIGALRM);
    sigaddset(&set, SIGIO);
    sigaddset(&set, SIGINT);
    sigaddset(&set, cetus_signal_value(CETUS_RECONFIGURE_SIGNAL));
    sigaddset(&set, cetus_signal_value(CETUS_REOPEN_SIGNAL));
    sigaddset(&set, cetus_signal_value(CETUS_NOACCEPT_SIGNAL));
    sigaddset(&set, cetus_signal_value(CETUS_TERMINATE_SIGNAL));
    sigaddset(&set, cetus_signal_value(CETUS_SHUTDOWN_SIGNAL));
    sigaddset(&set, cetus_signal_value(CETUS_CHANGEBIN_SIGNAL));

    if (sigprocmask(SIG_BLOCK, &set, NULL) == -1) {
        g_critical("%s: sigprocmask() failed, errno:%d", G_STRLOC, errno);
    }

    sigemptyset(&set);


    size = sizeof(master_process);

    for (i = 0; i < cetus_argc; i++) {
        size += strlen(cetus_argv[i]) + 1;
    }


    cetus_start_worker_processes(cycle, cycle->worker_processes,
                               CETUS_PROCESS_RESPAWN);

    cetus_start_admin_processes(cycle, 0);

    delay = 0;
    sigio = 0;
    live = 1;

    for ( ;; ) {
        if (delay) {
            if (cetus_sigalrm) {
                sigio = 0;
                delay *= 2;
                cetus_sigalrm = 0;
            }

            g_debug("%s: termination cycle: %d", G_STRLOC, delay);

            itv.it_interval.tv_sec = 0;
            itv.it_interval.tv_usec = 0;
            itv.it_value.tv_sec = delay / 1000;
            itv.it_value.tv_usec = (delay % 1000 ) * 1000;

            if (setitimer(ITIMER_REAL, &itv, NULL) == -1) {
                g_critical("%s: setitimer() failed, errno:%d", G_STRLOC, errno);
            }
        }

        sigsuspend(&set);

        if (cetus_reap) {
            cetus_reap = 0;
            live = cetus_reap_children(cycle);
        }

        if (!live && (cetus_terminate || cetus_quit)) {
            g_message("%s: call cetus_master_process_exit", G_STRLOC);
            cetus_master_process_exit(cycle);
        }

        if (cetus_terminate) {
            if (delay == 0) {
                delay = 50;
            }

            if (sigio) {
                sigio--;
                continue;
            }

            sigio = cycle->worker_processes;

            if (delay > 1000) {
                g_message("%s: send kill sig to workers", G_STRLOC);
                cetus_signal_worker_processes(cycle, SIGKILL);
            } else {
                cetus_signal_worker_processes(cycle,
                                       cetus_signal_value(CETUS_TERMINATE_SIGNAL));
            }

            continue;
        }

        if (cetus_quit) {
            cetus_signal_worker_processes(cycle,
                                        cetus_signal_value(CETUS_SHUTDOWN_SIGNAL));
            continue;
        }

        if (cetus_reconfigure) {
            cetus_reconfigure = 0;

            g_message("%s: reconfiguring", G_STRLOC);

            /* init cycle */
            cetus_start_worker_processes(cycle, cycle->worker_processes,
                                       CETUS_PROCESS_JUST_RESPAWN);
            cetus_start_admin_processes(cycle, 1);

            /* allow new processes to start */
            usleep(100 * 1000);

            live = 1;
            cetus_signal_worker_processes(cycle,
                                        cetus_signal_value(CETUS_SHUTDOWN_SIGNAL));
        }

        if (cetus_restart) {
            cetus_restart = 0;
            cetus_start_worker_processes(cycle, cycle->worker_processes,
                                       CETUS_PROCESS_RESPAWN);
            cetus_start_admin_processes(cycle, 0);
    
            live = 1;
        }

        if (cetus_reopen) {
            cetus_reopen = 0;
            g_message("%s: reopening logs", G_STRLOC);
            cetus_signal_worker_processes(cycle,
                                        cetus_signal_value(CETUS_REOPEN_SIGNAL));
        }

        if (cetus_noaccept) {
            cetus_noaccept = 0;
            cetus_noaccepting = 1;
            cetus_signal_worker_processes(cycle,
                                        cetus_signal_value(CETUS_SHUTDOWN_SIGNAL));
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
        cetus_spawn_process(cycle, cetus_worker_process_cycle,
                          (void *) (intptr_t) i, "worker process", type);

        ch.basics.pid = cetus_processes[cetus_process_slot].pid;
        ch.basics.slot = cetus_process_slot;
        ch.basics.fd = cetus_processes[cetus_process_slot].parent_child_channel[0];

        g_debug("%s: call cetus_pass_open_channel", G_STRLOC);
        cetus_pass_open_channel(cycle, &ch);
    }
}


static void
cetus_pass_open_channel(cetus_cycle_t *cycle, cetus_channel_t *ch)
{
    int  i;

    for (i = 0; i < cetus_last_process; i++) {

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

    case cetus_signal_value(CETUS_REOPEN_SIGNAL):
        ch.basics.command = CETUS_CMD_REOPEN;
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
                if (signo != cetus_signal_value(CETUS_REOPEN_SIGNAL)) {
                    cetus_processes[i].exiting = 1;
                }

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
                cetus_reap = 1;
            }

            continue;
        }

        if (signo != cetus_signal_value(CETUS_REOPEN_SIGNAL)) {
            cetus_processes[i].exiting = 1;
        }
    }
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

            if (!cetus_processes[i].detached) {
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

                g_debug("%s: call cetus_pass_open_channel", G_STRLOC);
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
    unsigned int  i;

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

    for ( ;; ) {

        if (cetus_exiting) {
            cetus_worker_process_exit(cycle);
        }

        g_debug("%s: worker cycle", G_STRLOC);

        /* call main procedures for worker */
        chassis_event_loop_t *loop = cycle->event_base;
        chassis_event_loop(loop);
        g_debug("%s: after chassis_event_loop", G_STRLOC);

        if (cetus_terminate) {
            g_message("%s: exiting", G_STRLOC);
            cetus_worker_process_exit(cycle);
        }

        if (cetus_quit) {
            cetus_quit = 0;
            g_message("%s: gracefully shutting down", G_STRLOC);

            if (!cetus_exiting) {
                cetus_exiting = 1;
                /* Call cetus shut down */
            }
        }

        if (cetus_reopen) {
            cetus_reopen = 0;
            g_message("%s: reopening logs", G_STRLOC);
        }
    }
}

static void
cetus_admin_process_init(cetus_cycle_t *cycle)
{
    int                n;

    g_debug("%s: cetus_admin_process_init, cetus_last_process:%d", 
                G_STRLOC, cetus_last_process);

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
            g_critical("%s: close() channel failed, errno:%d", G_STRLOC, errno);
        }
        
        if (close(cetus_processes[n].admin_worker_channel[1]) == -1) {
            g_critical("%s: close() channel failed, errno:%d", G_STRLOC, errno);
        }

    }

    chassis_event_loop_t *mainloop = chassis_event_loop_new();
    cycle->event_base = mainloop;
    g_assert(cycle->event_base);

    event_set(&cetus_channel_event, cetus_channel, EV_READ | EV_PERSIST, cetus_channel_handler, cycle);
    chassis_event_add(cycle, &cetus_channel_event);
    g_debug("%s: cetus_channel:%d is waiting for read, event base:%p, ev:%p",
            G_STRLOC, cetus_channel, cycle->event_base, &cetus_channel_event);

}


static void
cetus_worker_process_init(cetus_cycle_t *cycle, int worker)
{
    sigset_t           set;
    int                n;
    unsigned int       i;
    cetus_cpuset_t    *cpu_affinity;
    struct rlimit      rlmt;

    if (worker >= 0) {
        if (cpu_affinity) {
            cetus_setaffinity(cpu_affinity);
        }
    }

    sigemptyset(&set);

    if (sigprocmask(SIG_SETMASK, &set, NULL) == -1) {
        g_critical("%s: sigprocmask() failed, errno:%d", G_STRLOC, errno);
    }

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
            g_critical("%s: close() channel failed, errno:%d", G_STRLOC, errno);
        }
        
        if (close(cetus_processes[n].admin_worker_channel[1]) == -1) {
            g_critical("%s: close() channel failed, errno:%d", G_STRLOC, errno);
        }

    }

    g_debug("%s: close() channel zero fd:%d, n:%d", 
            G_STRLOC, cetus_processes[cetus_process_slot].parent_child_channel[0], cetus_process_slot);
    if (close(cetus_processes[cetus_process_slot].parent_child_channel[0]) == -1) {
        g_critical("%s: close() channel failed, strerr:%s", G_STRLOC, strerror(errno));
    }

    g_debug("%s: close() channel zero fd:%d, n:%d", 
            G_STRLOC, cetus_processes[cetus_process_slot].admin_worker_channel[0], cetus_process_slot);
    if (close(cetus_processes[cetus_process_slot].admin_worker_channel[0]) == -1) {
        g_critical("%s: close() channel failed, strerr:%s", G_STRLOC, strerror(errno));
    }

    chassis_event_loop_t *mainloop = chassis_event_loop_new();
    cycle->event_base = mainloop;
    g_assert(cycle->event_base);

    event_set(&cetus_channel_event, cetus_channel, EV_READ | EV_PERSIST, cetus_channel_handler, cycle);
    chassis_event_add(cycle, &cetus_channel_event);
    g_debug("%s: cetus_channel:%d is waiting for read, event base:%p, ev:%p",
            G_STRLOC, cetus_channel, cycle->event_base, &cetus_channel_event);
    
    event_set(&cetus_admin_channel_event, cetus_admin_channel, EV_READ | EV_PERSIST, cetus_channel_handler, cycle);
    chassis_event_add(cycle, &cetus_admin_channel_event);
    g_debug("%s: cetus_admin_channel:%d is waiting for read, event base:%p, ev:%p",
            G_STRLOC, cetus_admin_channel, cycle->event_base, &cetus_admin_channel_event);

}


static void
cetus_worker_process_exit(cetus_cycle_t *cycle)
{
    unsigned int         i;

    g_message("%s: exit", G_STRLOC);

    exit(0);
}

static
cetus_channel_t *retrieve_admin_resp(network_mysqld_con *con)
{
    int resp_len = 0;
    GList *chunk;
    cetus_channel_t *ch = NULL;

    g_message("%s:call retrieve_admin_resp", G_STRLOC);
    for (chunk = con->client->send_queue->chunks->head; chunk; chunk = chunk->next) {
        GString *s = chunk->data;
        g_debug_hexdump(G_STRLOC, S(s));
        resp_len = s->len;
        g_message("%s:s->len:%d", G_STRLOC, (int) s->len);
        if (chunk->next == NULL) {
            int total = sizeof(cetus_channel_t) + resp_len; 
            ch = (cetus_channel_t *) g_new0(char, total);
            ch->admin_sql_resp_len = resp_len;
            memcpy(ch->admin_sql_resp, s->str, resp_len);
            g_message("%s:total:%d, resp_len:%d", G_STRLOC, total, resp_len);
            g_debug_hexdump(G_STRLOC, ch->admin_sql_resp, resp_len);
        }
    }
    g_message("%s:call retrieve_admin_resp end", G_STRLOC);
    
    return ch;
}


static 
void send_admin_resp(chassis *cycle, network_mysqld_con *con)
{
    g_message("%s:call send_admin_resp, cetus_process_slot:%d", G_STRLOC, cetus_process_slot);
    cetus_channel_t  *ch = retrieve_admin_resp(con); 
    ch->basics.command = CETUS_CMD_ADMIN_RESP;
    ch->basics.pid = cetus_processes[cetus_process_slot].pid;
    ch->basics.slot = cetus_process_slot;
    ch->basics.fd = cetus_processes[cetus_process_slot].admin_worker_channel[1];

    g_message("%s:send resp to admin:%d", G_STRLOC, cetus_last_process);
    int i;
    for (i = 0; i < 1; i++) {
        g_message("%s: pass sql resp channel s:%i pid:%d to:%d", G_STRLOC,
                ch->basics.slot, ch->basics.pid, cetus_processes[i].pid);

        /* TODO: AGAIN */
        cetus_write_channel(cetus_processes[i].admin_worker_channel[1],
                ch, sizeof(*ch) + ch->admin_sql_resp_len);
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
        func = plugin->con_exectute_sql;
        retval = (*func) (cycle, con);
        g_debug("%s: call admin", G_STRLOC);
        send_admin_resp(cycle, con);
    }
}

static void
cetus_channel_handler(int fd, short events, void *user_data)
{
    cetus_channel_t    ch;

    g_debug("%s: channel handler", G_STRLOC);

    do {

        int ret = cetus_read_channel(fd, &ch, sizeof(cetus_channel_t));

        g_debug("%s: cetus_read_channel channel, fd:%d", G_STRLOC, fd);

        if (ret == NETWORK_SOCKET_ERROR) {
            cetus_terminate = 1;
            closesocket(fd);
            return;
        }

        if (ret == NETWORK_SOCKET_WAIT_FOR_EVENT) {
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

        case CETUS_CMD_REOPEN:
            cetus_reopen = 1;
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

