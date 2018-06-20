#include <unistd.h>
#include <errno.h>
#include <sys/uio.h>
#include <sys/socket.h>
#include "cetus-channel.h"
#include "network-socket.h"


int
cetus_write_channel(int s, cetus_channel_t *ch, size_t size)
{
    int                 err;
    ssize_t             n;
    struct iovec        iov[1];
    struct msghdr       msg;

    g_debug("%s: call cetus_write_channel for pid:%d, to fd:%d)", G_STRLOC, getpid(), s);

    iov[0].iov_base = (char *) ch;
    iov[0].iov_len = size;

    msg.msg_name = NULL;
    msg.msg_namelen = 0;
    msg.msg_iov = iov;
    msg.msg_iovlen = 1;

    n = sendmsg(s, &msg, 0);

    if (n == -1) {
        err = errno;
        if (err == EAGAIN) {
            return NETWORK_SOCKET_WAIT_FOR_EVENT;
        }

        g_critical("%s:sendmsg() failed, errno:%d", G_STRLOC, errno);
        return NETWORK_SOCKET_ERROR;
    }

    return NETWORK_SOCKET_SUCCESS;
}


int
cetus_read_channel(int s, cetus_channel_t *ch, size_t size)
{
    int                 fd;
    int                 err;
    ssize_t             n;
    struct iovec        iov[1];
    struct msghdr       msg;

    iov[0].iov_base = (char *) ch;
    iov[0].iov_len = size;

    msg.msg_name = NULL;
    msg.msg_namelen = 0;
    msg.msg_iov = iov;
    msg.msg_iovlen = 1;

    n = recvmsg(s, &msg, 0);

    if (n == -1) {
        err = errno;
        if (err == EAGAIN) {
            return NETWORK_SOCKET_WAIT_FOR_EVENT;
        }

        g_critical("%s:recvmsg() failed, errno:%d", G_STRLOC, errno);
        return NETWORK_SOCKET_ERROR;
    }

    if (n == 0) {
        g_message("%s:recvmsg() returned zero", G_STRLOC);
        return NETWORK_SOCKET_ERROR;
    }

    if ((size_t) n < sizeof(cetus_channel_t)) {
        g_critical("%s:recvmsg() returned not enough data: %d", G_STRLOC, (int) n);
        return NETWORK_SOCKET_ERROR;
    }


    if (ch->command == CETUS_CMD_OPEN_CHANNEL) {
        ch->fd = fd;
    }

    ch->num = n;

    return NETWORK_SOCKET_SUCCESS;
}


void
cetus_close_channel(int *fd)
{
    if (close(fd[0]) == -1) {
        g_critical("%s:close() channel failed, errno:%d", G_STRLOC, errno);
    }

    if (close(fd[1]) == -1) {
        g_critical("%s:close() channel failed, errno:%d", G_STRLOC, errno);
    }
}

