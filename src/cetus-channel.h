
#ifndef _CETUS_CHANNEL_H_INCLUDED_
#define _CETUS_CHANNEL_H_INCLUDED_

#include <fcntl.h>
#include <stddef.h>

#define CETUS_CMD_OPEN_CHANNEL   1
#define CETUS_CMD_CLOSE_CHANNEL  2
#define CETUS_CMD_QUIT           3
#define CETUS_CMD_TERMINATE      4
#define CETUS_CMD_ADMIN          5
#define CETUS_CMD_ADMIN_RESP     6

#define MAX_ADMIN_SQL_LEN 512
typedef struct {
    unsigned int command;
    int slot;
    int fd;
    int num;
    pid_t   pid;
    struct event *event;
} cetus_channel_mininum_t;


typedef struct {
    cetus_channel_mininum_t basics;
    int len;
    int admin_sql_resp_len;
    char admin_sql[MAX_ADMIN_SQL_LEN];
    unsigned char admin_sql_resp[0];
} cetus_channel_t;


int cetus_write_channel(int s, cetus_channel_t *ch, size_t size);
int cetus_read_channel(int s, cetus_channel_t *ch, size_t size);
void cetus_close_channel(int *fd);


#endif /* _CETUS_CHANNEL_H_INCLUDED_ */
