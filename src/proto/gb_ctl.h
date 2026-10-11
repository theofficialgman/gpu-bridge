/*
 * gb_ctl.h - control socket protocol (AF_UNIX SOCK_SEQPACKET socketpair).
 *
 * The client creates the socketpair and spawns the server with one end, so
 * there is no filesystem socket and the server's lifetime is tied to the
 * client's: when the client dies the peer sees EOF and exits.
 *
 * Fixed-size messages, optional single fd attached via SCM_RIGHTS.
 */
#ifndef GB_CTL_H
#define GB_CTL_H

#include <stdint.h>

#define GB_CTL_MAGIC 0x4C544347u /* "GCTL" */

enum gb_ctl_type {
	GB_CTL_HELLO = 1,       /* c->s: a[0]=proto ver, a[1]=ptr bits, a[2]=ops hash */
	GB_CTL_HELLO_REPLY,     /* s->c: status, a[0]=proto ver, a[1]=ptr bits (server) */
	GB_CTL_NEW_CHANNEL,     /* c->s + fd: a[0]=region bytes */
	GB_CTL_NEW_CHANNEL_REPLY, /* s->c: status, a[0]=channel id */
	GB_CTL_BYE,             /* c->s */
	GB_CTL_FD_PUT,          /* c->s + fd: a[0]=token (an fd the next requests name) */
	GB_CTL_FD_GIVE,         /* s->c + fd: a[0]=token (an fd the server returns) */
};

struct gb_ctl_msg {
	uint32_t magic;
	uint32_t type;
	int32_t status; /* replies: 0 or -errno */
	uint32_t seq;
	uint64_t a[4];
};

_Static_assert(sizeof(struct gb_ctl_msg) == 48, "gb_ctl_msg layout");

/* Send one message, with `fd` attached if >= 0. Returns 0 or -errno. */
int gb_ctl_send(int sock, const struct gb_ctl_msg *m, int fd);
/* Receive one message. If *fd_out is non-NULL it receives an attached fd
 * (or -1). Returns 0, -errno, or -ECONNRESET on orderly peer close. */
int gb_ctl_recv(int sock, struct gb_ctl_msg *m, int *fd_out);

#endif /* GB_CTL_H */
