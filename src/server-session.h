#ifndef _SERVER_SESSION_
#define _SERVER_SESSION_

#include "network-socket.h"
#include "network-mysqld.h"

void server_session_free(server_session_t *pmd);

void server_session_con_handler(int event_fd, short events, void *user_data);

void server_sess_wait_for_event(server_session_t *ev_struct, short ev_type,
                               struct timeval *timeout);

#endif /* _SERVER_SESSION_ */
