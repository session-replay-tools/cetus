#ifndef _CETUS_PROCESS_H_INCLUDED_
#define _CETUS_PROCESS_H_INCLUDED_


#include "cetus-setaffinity.h"
#include "chassis-mainloop.h"
#include "network-mysqld.h"


typedef chassis cetus_cycle_t;
typedef pid_t       cetus_pid_t;

#define CETUS_INVALID_PID  -1

typedef void (*cetus_spawn_proc_pt) (cetus_cycle_t *cycle, void *data);

typedef struct {
    cetus_pid_t           pid;
    int                   status;
    int                   parent_child_channel[2];

    struct event          event;

    cetus_spawn_proc_pt   proc;
    void                 *data;
    char                 *name;

    unsigned              respawn:1;
    unsigned              just_spawn:1;
    unsigned              detached:1;
    unsigned              exiting:1;
    unsigned              exited:1;
    unsigned              is_admin:1;
    unsigned              is_worker:1;
} cetus_process_t;


typedef struct {
    char         *path;
    char         *name;
    char        **argv;
    char        **envp;
} cetus_exec_ctx_t;


#define CETUS_MAX_PROCESSES         64

#define CETUS_PROCESS_NORESPAWN     -1
#define CETUS_PROCESS_JUST_SPAWN    -2
#define CETUS_PROCESS_RESPAWN       -3
#define CETUS_PROCESS_JUST_RESPAWN  -4
#define CETUS_PROCESS_DETACHED      -5

#define cetus_signal_helper(n)     SIG##n
#define cetus_signal_value(n)      cetus_signal_helper(n)
#define CETUS_SHUTDOWN_SIGNAL      QUIT
#define CETUS_TERMINATE_SIGNAL     TERM
#define CETUS_NOACCEPT_SIGNAL      WINCH
#define CETUS_RECONFIGURE_SIGNAL   HUP
#define CETUS_REOPEN_SIGNAL        USR1
#define CETUS_CHANGEBIN_SIGNAL     USR2

#define cetus_value_helper(n)   #n
#define cetus_value(n)          cetus_value_helper(n)

cetus_pid_t cetus_spawn_process(cetus_cycle_t *cycle,
    cetus_spawn_proc_pt proc, void *data, char *name, int respawn);
cetus_pid_t cetus_execute(cetus_cycle_t *cycle, cetus_exec_ctx_t *ctx);
int cetus_init_signals();


extern int              cetus_argc;
extern char           **cetus_argv;
extern char           **cetus_os_argv;

extern pid_t    cetus_pid;
extern pid_t    cetus_parent;
extern int      cetus_channel;
extern int      cetus_process_slot;
extern int      cetus_last_process;
extern int      cetus_process_id;
extern struct event cetus_channel_event;
extern cetus_process_t  cetus_processes[CETUS_MAX_PROCESSES];


#endif /* _CETUS_PROCESS_H_INCLUDED_ */
