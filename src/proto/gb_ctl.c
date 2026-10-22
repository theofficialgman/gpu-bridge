/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "gb_ctl.h"

#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

int gb_ctl_send(int sock, const struct gb_ctl_msg *m, int fd)
{
	struct iovec iov = { (void *)m, sizeof(*m) };
	union {
		char buf[CMSG_SPACE(sizeof(int))];
		struct cmsghdr align;
	} u;
	struct msghdr mh = { .msg_iov = &iov, .msg_iovlen = 1 };

	if (fd >= 0) {
		memset(&u, 0, sizeof(u));
		mh.msg_control = u.buf;
		mh.msg_controllen = sizeof(u.buf);
		struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
		c->cmsg_level = SOL_SOCKET;
		c->cmsg_type = SCM_RIGHTS;
		c->cmsg_len = CMSG_LEN(sizeof(int));
		memcpy(CMSG_DATA(c), &fd, sizeof(int));
	}
	for (;;) {
		ssize_t n = sendmsg(sock, &mh, MSG_NOSIGNAL);
		if (n == (ssize_t)sizeof(*m))
			return 0;
		if (n < 0 && errno == EINTR)
			continue;
		return n < 0 ? -errno : -EIO;
	}
}

int gb_ctl_recv(int sock, struct gb_ctl_msg *m, int *fd_out)
{
	struct iovec iov = { m, sizeof(*m) };
	union {
		char buf[CMSG_SPACE(sizeof(int))];
		struct cmsghdr align;
	} u;
	struct msghdr mh = { .msg_iov = &iov, .msg_iovlen = 1,
			     .msg_control = u.buf,
			     .msg_controllen = sizeof(u.buf) };
	ssize_t n;

	if (fd_out)
		*fd_out = -1;
	do {
		n = recvmsg(sock, &mh, MSG_CMSG_CLOEXEC);
	} while (n < 0 && errno == EINTR);
	if (n < 0)
		return -errno;
	if (n == 0)
		return -ECONNRESET;

	/* Always take (and, if unwanted, close) any attached fd. */
	for (struct cmsghdr *c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c)) {
		if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
			int fd;
			memcpy(&fd, CMSG_DATA(c), sizeof(int));
			if (fd_out && *fd_out < 0)
				*fd_out = fd;
			else
				close(fd);
		}
	}
	if (n != (ssize_t)sizeof(*m) || m->magic != GB_CTL_MAGIC) {
		if (fd_out && *fd_out >= 0) {
			close(*fd_out);
			*fd_out = -1;
		}
		return -EPROTO;
	}
	return 0;
}
