#include <unistd.h>
#include <errno.h>
#include <sys/uio.h>
#include <sys/socket.h>

#include "glib-ext.h"
#include "cetus-channel.h"
#include "network-socket.h"


int
cetus_write_channel(int s, cetus_channel_t *ch, size_t size)
{
    ssize_t             n;
    struct iovec        iov[1];
    struct msghdr       msg;

    g_debug("%s:call cetus_write_channel, fd:%d", G_STRLOC, s);

    union {
        struct cmsghdr  cm; 
        char            space[CMSG_SPACE(sizeof(int))];
    } cmsg;

    if (ch->basics.fd == -1) {
        msg.msg_control = NULL;
        msg.msg_controllen = 0;

    } else {
        msg.msg_control = (caddr_t) &cmsg;
        msg.msg_controllen = sizeof(cmsg);

        memset(&cmsg, 0, sizeof(cmsg));

        cmsg.cm.cmsg_len = CMSG_LEN(sizeof(int));
        cmsg.cm.cmsg_level = SOL_SOCKET;
        cmsg.cm.cmsg_type = SCM_RIGHTS;

        /* memcpy(CMSG_DATA(&cmsg.cm), &ch->basics.fd, sizeof(int)); */
    }

    msg.msg_flags = 0;

    iov[0].iov_base = (char *) ch;
    iov[0].iov_len = size;

    msg.msg_name = NULL;
    msg.msg_namelen = 0;
    msg.msg_iov = iov;
    msg.msg_iovlen = 1;

    iov[0].iov_base = (char *) ch;
    iov[0].iov_len = size;

    msg.msg_name = NULL;
    msg.msg_namelen = 0;
    msg.msg_iov = iov;
    msg.msg_iovlen = 1;

    n = sendmsg(s, &msg, 0);

    g_debug("%s: sendmsg fd:%d, n:%d, size:%d", G_STRLOC, s, (int) n, (int) size);
    if (n == -1) {
        int err = errno;
        if (err == EAGAIN) {
            return NETWORK_SOCKET_WAIT_FOR_EVENT;
        }

        g_critical("%s:sendmsg() failed, err:%s", G_STRLOC, strerror(err));
        return NETWORK_SOCKET_ERROR;
    }

    return NETWORK_SOCKET_SUCCESS;
}


int
cetus_read_channel(int s, cetus_channel_t *ch, size_t size)
{
    ssize_t             n;
    struct iovec        iov[1];
    struct msghdr       msg;

    union {
        struct cmsghdr  cm;
        char            space[CMSG_SPACE(sizeof(int))];
    } cmsg;

    iov[0].iov_base = (char *) ch;
    iov[0].iov_len = size;

    msg.msg_name = NULL;
    msg.msg_namelen = 0;
    msg.msg_iov = iov;
    msg.msg_iovlen = 1;
    msg.msg_flags = 0;

    msg.msg_control = (caddr_t) &cmsg;
    msg.msg_controllen = sizeof(cmsg);

    n = recvmsg(s, &msg, 0);

    g_debug("%s: recvmsg fd:%d, n:%d", G_STRLOC, s, (int) n);

    if (n == -1) {
        int err = errno;
        if (err == EAGAIN) {
            g_debug("%s:recvmsg() EAGAIN, errno:%d", G_STRLOC, err);
            return NETWORK_SOCKET_WAIT_FOR_EVENT;
        }

        g_critical("%s:recvmsg() failed, err:%s", G_STRLOC, strerror(err));
        return NETWORK_SOCKET_ERROR;
    }

    if (n == 0) {
        g_message("%s:recvmsg() returned zero", G_STRLOC);
        return NETWORK_SOCKET_ERROR;
    }

    if ((size_t) n < sizeof(cetus_channel_mininum_t)) {
        g_critical("%s:recvmsg() returned not enough data: %d", G_STRLOC, (int) n);
        return NETWORK_SOCKET_ERROR;
    }

    switch (ch->basics.command) {
        case CETUS_CMD_ADMIN:
        case CETUS_CMD_ADMIN_RESP:
        case CETUS_CMD_OPEN_CHANNEL: 
            if (cmsg.cm.cmsg_len < (socklen_t) CMSG_LEN(sizeof(int))) {
                g_critical("%s:recvmsg() returned too small ancillary data:%d", 
                        G_STRLOC, (int) n);
                return NETWORK_SOCKET_ERROR;
            }

            if (cmsg.cm.cmsg_level != SOL_SOCKET || cmsg.cm.cmsg_type != SCM_RIGHTS)
            {
                g_critical("%s:recvmsg() returned invalid ancillary data level %d or type %d", 
                        G_STRLOC, cmsg.cm.cmsg_level, cmsg.cm.cmsg_type);
                return NETWORK_SOCKET_ERROR;
            }

            memcpy(&ch->basics.fd, CMSG_DATA(&cmsg.cm), sizeof(int));
            break;
        default:
            break;
    }

    if (ch->basics.command == CETUS_CMD_ADMIN || ch->basics.command == CETUS_CMD_ADMIN_RESP) {
        close(ch->basics.fd);
        ch->basics.fd = 0;
    } else {
        if (msg.msg_flags & (MSG_TRUNC|MSG_CTRUNC)) {
            g_critical("%s:recvmsg() truncated data", G_STRLOC); 
        }
    }


    ch->basics.num = n;

    return NETWORK_SOCKET_SUCCESS;
}


void
cetus_close_channel(int *fd)
{
    if (close(fd[0]) == -1) {
        g_critical("%s:close() channel failed, err:%s", G_STRLOC, strerror(errno));
    }

    if (close(fd[1]) == -1) {
        g_critical("%s:close() channel failed, err:%s", G_STRLOC, strerror(errno));
    }
}

