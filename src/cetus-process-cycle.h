
#ifndef _CETUS_PROCESS_CYCLE_H_INCLUDED_
#define _CETUS_PROCESS_CYCLE_H_INCLUDED_

#include "cetus-process.h"
#include "cetus-channel.h"

#define CETUS_PROCESS_SINGLE     0
#define CETUS_PROCESS_MASTER     1
#define CETUS_PROCESS_SIGNALLER  2
#define CETUS_PROCESS_WORKER     3
#define CETUS_PROCESS_ADMIN      4
#define CETUS_PROCESS_HELPER     5

#define cetus_rename_file(o, n)    rename((const char *) o, (const char *) n)
#define CETUS_OLDPID_EXT     ".oldbin"


void cetus_master_process_cycle(cetus_cycle_t *cycle);
void send_admin_resp(chassis *cycle, network_mysqld_con *con);

extern unsigned int cetus_process;
extern unsigned int cetus_worker;
extern pid_t       cetus_pid;
extern pid_t       cetus_new_binary;
extern unsigned int cetus_exiting;

extern sig_atomic_t    cetus_reap;
extern sig_atomic_t    cetus_quit;
extern sig_atomic_t    cetus_terminate;
extern sig_atomic_t    cetus_noaccept;
extern sig_atomic_t    cetus_change_binary;


#endif /* _CETUS_PROCESS_CYCLE_H_INCLUDED_ */
