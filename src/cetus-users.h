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

#ifndef _CETUS_USERS_H_
#define _CETUS_USERS_H_

#include "glib-ext.h"
#include "network-mysqld-packet.h"

typedef struct cetus_users_t {
    chassis_config_t *conf_manager;
    GHashTable *records;        /* <char *, pwd_pair_t *> */
} cetus_users_t;

enum cetus_pwd_type {
    CETUS_UNKNOWN_PWD,
    CETUS_CLIENT_PWD,
    CETUS_SERVER_PWD,
};

cetus_users_t *cetus_users_new();

void cetus_users_free(cetus_users_t *users);

gboolean cetus_users_parse_json(cetus_users_t *users, char *buffer);

gboolean cetus_users_read_json(cetus_users_t *users, chassis_config_t *, int refresh);

gboolean cetus_users_update_record(cetus_users_t *users, const char *user, const char *pass, enum cetus_pwd_type type);

gboolean cetus_users_delete_record(cetus_users_t *users, const char *user);

/* write current users to disk */
gboolean cetus_users_write_json(cetus_users_t *users);

gboolean cetus_users_authenticate_client(cetus_users_t *users, network_mysqld_auth_challenge *,
                                         network_mysqld_auth_response *);

void cetus_users_get_hashed_pwd(cetus_users_t *, const char *user, enum cetus_pwd_type, GString *sha1pwd);

void cetus_users_get_hashed_client_pwd(cetus_users_t *, const char *user, GString *sha1pwd);

void cetus_users_get_hashed_server_pwd(cetus_users_t *, const char *user, GString *sha1pwd);

void cetus_users_get_server_pwd(cetus_users_t *, const char *user, GString *pwd);

gboolean cetus_users_contains(cetus_users_t *, const char *user);

#endif /*_CETUS_USERS_H_*/
