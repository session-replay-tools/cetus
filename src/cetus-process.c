#include <signal.h>
#include <sys/wait.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include "glib-ext.h"
#include "cetus-channel.h"
#include "cetus-process.h"
#include "network-socket.h"
#include "cetus-process-cycle.h"

typedef struct {
    int     signo;
    char   *signame;
    char   *name;
    void (*handler)(int signo, siginfo_t *siginfo, void *ucontext);
} cetus_signal_t;


static void cetus_execute_proc(cetus_cycle_t *cycle, void *data);
static void cetus_signal_handler(int signo, siginfo_t *siginfo, void *ucontext);
static void cetus_process_get_status(void);


int              cetus_argc;
char           **cetus_argv;
char           **cetus_os_argv;

int              cetus_process_slot;
int              cetus_channel;
int              cetus_last_process;
int              cetus_process_id;
struct event     cetus_channel_event;
cetus_process_t  cetus_processes[CETUS_MAX_PROCESSES];


cetus_signal_t  signals[] = {
    { cetus_signal_value(CETUS_NOACCEPT_SIGNAL),
      "SIG" cetus_value(CETUS_NOACCEPT_SIGNAL),
      "",
      cetus_signal_handler },

    { cetus_signal_value(CETUS_TERMINATE_SIGNAL),
      "SIG" cetus_value(CETUS_TERMINATE_SIGNAL),
      "stop",
      cetus_signal_handler },

    { cetus_signal_value(CETUS_SHUTDOWN_SIGNAL),
      "SIG" cetus_value(CETUS_SHUTDOWN_SIGNAL),
      "quit",
      cetus_signal_handler },

    { cetus_signal_value(CETUS_CHANGEBIN_SIGNAL),
      "SIG" cetus_value(CETUS_CHANGEBIN_SIGNAL),
      "",
      cetus_signal_handler },

    { SIGALRM, "SIGALRM", "", cetus_signal_handler },

    { SIGINT, "SIGINT", "", cetus_signal_handler },

    { SIGIO, "SIGIO", "", NULL},

    { SIGCHLD, "SIGCHLD", "", cetus_signal_handler },

    { SIGSYS, "SIGSYS, SIG_IGN", "", NULL },

    { SIGPIPE, "SIGPIPE, SIG_IGN", "", NULL },

    { 0, NULL, "", NULL }
};

static int
create_channel(int channel[])
{
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, channel) == -1)
    {
        g_critical("%s: socketpair() failed ", G_STRLOC);
        return -1;
    }

    g_message("%s: create channel %d:%d", G_STRLOC,
            channel[0], channel[1]);

    if (fcntl(channel[0], F_SETFL, O_NONBLOCK | O_RDWR) != 0) {
        g_critical("%s: nonblock failed while spawning: %s (%d)",
                G_STRLOC, g_strerror(errno), errno);
        cetus_close_channel(channel);
        return -1;
    }

    if (fcntl(channel[1], F_SETFL, O_NONBLOCK | O_RDWR) != 0) {
        g_critical("%s: nonblock failed while spawning: %s (%d)",
                G_STRLOC, g_strerror(errno), errno);
        cetus_close_channel(channel);
        return -1;
    }

    int on = 1;
    if (ioctl(channel[0], FIOASYNC, &on) == -1) {
        g_critical("%s: ioctl(FIOASYNC) failed",
                G_STRLOC);
        cetus_close_channel(channel);
        return -1;
    }

    if (fcntl(channel[0], F_SETOWN, cetus_pid) == -1) {
        g_critical("%s: fcntl(F_SETOWN) failed while spawning:%s, cetus_pid:%d",
                G_STRLOC, strerror(errno), cetus_pid);
        cetus_close_channel(channel);
        return -1;
    }

    if (fcntl(channel[0], F_SETFD, FD_CLOEXEC) == -1) {
        g_critical("%s: fcntl(FD_CLOEXEC) failed",
                G_STRLOC);
        cetus_close_channel(channel);
        return -1;
    }

    if (fcntl(channel[1], F_SETFD, FD_CLOEXEC) == -1) {
        g_critical("%s: fcntl(FD_CLOEXEC) failed",
                G_STRLOC);
        cetus_close_channel(channel);
        return -1;
    }

    return 0;
}


pid_t
cetus_spawn_process(cetus_cycle_t *cycle, cetus_spawn_proc_pt proc, void *data,
    char *name, int respawn)
{
    pid_t  pid;
    int  s;

    if (respawn >= 0) {
        s = respawn;

    } else {
        for (s = 0; s < cetus_last_process; s++) {
            if (cetus_processes[s].pid == -1) {
                break;
            }
        }

        if (s == CETUS_MAX_PROCESSES) {
            g_critical("%s: no more than %d processes can be spawned",
                    G_STRLOC, CETUS_MAX_PROCESSES);
            return CETUS_INVALID_PID;
        }
    }


    if (respawn != CETUS_PROCESS_DETACHED) {

        if (create_channel(cetus_processes[s].parent_child_channel) == -1) {
            return CETUS_INVALID_PID;
        }

        cetus_channel = cetus_processes[s].parent_child_channel[1];
    } else {
        cetus_processes[s].parent_child_channel[0] = -1;
        cetus_processes[s].parent_child_channel[1] = -1;
    }

    cetus_process_slot = s;

    pid = fork();

    switch (pid) {

    case -1:
        g_critical("%s: fork() failed while spawning \"%s\"",
                    G_STRLOC, name);
        cetus_close_channel(cetus_processes[s].parent_child_channel);
        return CETUS_INVALID_PID;

    case 0:
        cetus_parent = cetus_pid;
        cetus_pid = getpid();
        cetus_processes[cetus_process_slot].pid = cetus_pid;
        cetus_processes[s].exited = 0;
        g_message("%s: after call fork, channel:%d, pid:%d", G_STRLOC, s, cetus_processes[s].pid);
        proc(cycle, data);
        break;

    default:
        break;
    }

    g_message("%s: start %s %d, respawn:%d", G_STRLOC, name, pid, respawn);

    cetus_processes[s].pid = pid;
    cetus_processes[s].exited = 0;

    if (respawn >= 0) {
        return pid;
    }

    cetus_processes[s].proc = proc;
    cetus_processes[s].data = data;
    cetus_processes[s].name = name;
    cetus_processes[s].exiting = 0;

    switch (respawn) {

    case CETUS_PROCESS_NORESPAWN:
        cetus_processes[s].respawn = 0;
        cetus_processes[s].just_spawn = 0;
        cetus_processes[s].detached = 0;
        break;

    case CETUS_PROCESS_JUST_SPAWN:
        cetus_processes[s].respawn = 0;
        cetus_processes[s].just_spawn = 1;
        cetus_processes[s].detached = 0;
        break;

    case CETUS_PROCESS_RESPAWN:
        cetus_processes[s].respawn = 1;
        cetus_processes[s].just_spawn = 0;
        cetus_processes[s].detached = 0;
        break;

    case CETUS_PROCESS_JUST_RESPAWN:
        cetus_processes[s].respawn = 1;
        cetus_processes[s].just_spawn = 1;
        cetus_processes[s].detached = 0;
        break;

    case CETUS_PROCESS_DETACHED:
        cetus_processes[s].respawn = 0;
        cetus_processes[s].just_spawn = 0;
        cetus_processes[s].detached = 1;
        break;
    }

    if (s == cetus_last_process) {
        g_message("%s: cetus_last_process add,orig:%d, cetus_processes[s].parent_child_channel[0]:%d",
                G_STRLOC, cetus_last_process, cetus_processes[s].parent_child_channel[0]);
        cetus_last_process++;
        /* TODO may have potential problems when having too many crashes */
        cetus_process_id = (cetus_process_id + 1) % MAX_WORK_PROCESSES;
    }

    return pid;
}


pid_t
cetus_execute(cetus_cycle_t *cycle, cetus_exec_ctx_t *ctx)
{
    return cetus_spawn_process(cycle, cetus_execute_proc, ctx, ctx->name,
                             CETUS_PROCESS_DETACHED);
}


static void
cetus_execute_proc(cetus_cycle_t *cycle, void *data)
{
    int i;
    for (i = 0; i < cycle->modules->len; i++) {
        chassis_plugin *p = cycle->modules->pdata[i];
        p->stop_listening(cycle, p->config);
    }

    cetus_exec_ctx_t  *ctx = data;

    if (execve(ctx->path, (char * const*) ctx->argv, (char * const*) ctx->envp) == -1) {
        g_critical("%s: execve() failed while executing %s \"%s\"",
                G_STRLOC, ctx->name, ctx->path);
    }

    free(ctx->envp[0]);

    exit(1);
}


int
cetus_init_signals()
{
    cetus_signal_t      *sig;
    struct sigaction     sa;

    for (sig = signals; sig->signo != 0; sig++) {
        memset(&sa, 0, sizeof(struct sigaction));

        if (sig->handler) {
            sa.sa_sigaction = sig->handler;
            sa.sa_flags = SA_SIGINFO;

        } else {
            sa.sa_handler = SIG_IGN;
        }

        sigemptyset(&sa.sa_mask);
        if (sigaction(sig->signo, &sa, NULL) == -1) {
            g_critical("%s: sigaction(%s) failed", G_STRLOC, sig->signame);
            return -1;
        }
    }

    return 0;
}


static void 
cetus_signal_handler(int signo, siginfo_t *siginfo, void *ucontext)
{
    int              err;
    cetus_signal_t  *sig;

    err =  errno;

    for (sig = signals; sig->signo != 0; sig++) {
        if (sig->signo == signo) {
            break;
        }
    }

    switch (cetus_process) {

    case CETUS_PROCESS_MASTER:
    case CETUS_PROCESS_SINGLE:
        switch (signo) {

        case cetus_signal_value(CETUS_SHUTDOWN_SIGNAL):
            cetus_quit = 1;
            break;

        case cetus_signal_value(CETUS_TERMINATE_SIGNAL):
        case SIGINT:
            cetus_terminate = 1;
            break;

        case cetus_signal_value(CETUS_NOACCEPT_SIGNAL):
            cetus_noaccept = 1;
            break;

        case cetus_signal_value(CETUS_CHANGEBIN_SIGNAL):
            if (getppid() == cetus_parent || cetus_new_binary > 0) {
                break;
            }

            cetus_change_binary = 1;
            break;

        case SIGALRM:
            break;

        case SIGIO:
            break;

        case SIGCHLD:
            cetus_reap = 1;
            break;
        }

        break;

    case CETUS_PROCESS_WORKER:
    case CETUS_PROCESS_HELPER:
        switch (signo) {

        case cetus_signal_value(CETUS_NOACCEPT_SIGNAL):
            cetus_noaccept = 1;
            break;
        case cetus_signal_value(CETUS_SHUTDOWN_SIGNAL):
            cetus_quit = 1;
            break;

        case cetus_signal_value(CETUS_TERMINATE_SIGNAL):
        case SIGINT:
            cetus_terminate = 1;
            break;

        case cetus_signal_value(CETUS_CHANGEBIN_SIGNAL):
        case SIGIO:
            break;
        }

        break;
    }

    if (signo == SIGCHLD) {
        cetus_process_get_status();
    }

    errno = err;
}


static void
cetus_process_get_status(void)
{
    int            status;
    pid_t          pid;
    int            err;
    int            i;
    unsigned int   one;

    one = 0;

    for ( ;; ) {
        pid = waitpid(-1, &status, WNOHANG);

        if (pid == 0) {
            return;
        }

        if (pid == -1) {
            err = errno;

            if (err == EINTR) {
                continue;
            }

            if (err == ECHILD && one) {
                return;
            }

            if (err == ECHILD) {
                return;
            }

            return;
        }


        one = 1;
        for (i = 0; i < cetus_last_process; i++) {
            if (cetus_processes[i].pid == pid) {
                cetus_processes[i].status = status;
                cetus_processes[i].exited = 1;
                break;
            }
        }

        if (WEXITSTATUS(status) == 2 && cetus_processes[i].respawn) {
            cetus_processes[i].respawn = 0;
        }
    }
}

