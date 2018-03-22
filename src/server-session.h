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

#ifndef _SERVER_SESSION_
#define _SERVER_SESSION_

#include "network-socket.h"
#include "network-mysqld.h"

void server_session_free(server_session_t *ss);

void server_session_con_handler(int event_fd, short events, void *user_data);

void server_sess_wait_for_event(server_session_t *ev_struct, short ev_type, struct timeval *timeout);

#endif /* _SERVER_SESSION_ */
