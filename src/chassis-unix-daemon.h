#ifndef __CHASSIS_UNIX_DAEMON_H__
#define __CHASSIS_UNIX_DAEMON_H__

int chassis_unix_proc_keepalive(int *child_exit_status);
void chassis_unix_daemonize(void);

#endif
