/*
 * Copyright 2013-2018 Fabian Groffen
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


#include <stdarg.h>
#include <stddef.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <sys/uio.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include <ev.h>

#include "queue.h"
#include "relay.h"
#include "router.h"
#include "server.h"
#include "collector.h"
#include "dispatcher.h"

#ifdef HAVE_GZIP
#include <zlib.h>
#endif
#ifdef HAVE_LZ4
#include <lz4.h>
#include <lz4frame.h>
#endif
#ifdef HAVE_SNAPPY
#include <snappy-c.h>
#endif
#ifdef HAVE_SSL
#include <openssl/ssl.h>
#include <openssl/err.h>
#endif

enum conntype {
	LISTENER,
	CONNECTION
};

typedef struct _z_strm {
	ssize_t (*strmread)(struct _z_strm *, void *, size_t);  /* read func */
	/* read from buffer only func, on error set errno to ENOMEM, EMSGSIZE or EBADMSG */
	ssize_t (*strmreadbuf)(struct _z_strm *, void *, size_t, int, int);
	int (*strmclose)(struct _z_strm *);
	union {
#ifdef HAVE_GZIP
		struct gz {
			z_stream z;
			int inflatemode;
		} gz;
#endif
#ifdef HAVE_LZ4
		struct lz4 {
			LZ4F_decompressionContext_t lz;
			size_t iloc; /* location for unprocessed input */
		} lz4;
#endif
#ifdef HAVE_SSL
		SSL *ssl;
#endif
		int sock;
		/* udp variant (in order to receive info about sender) */
		struct udp_strm {
			int sock;
			struct sockaddr_in6 saddr;
			char *srcaddr;
			size_t srcaddrlen;
		} udp;
	} hdl;
#if defined(HAVE_GZIP) || defined(HAVE_LZ4) || defined(HAVE_SNAPPY)
	char *ibuf;
	size_t ipos;
	size_t isize;
#endif
	struct _z_strm *nextstrm;
} z_strm;

#define SOCKGROWSZ  32768
#define CONNGROWSZ  1024
#define MAX_LISTENERS 32  /* hopefully enough */
#define POLL_TIMEOUT  100
#define IDLE_DISCONNECT_TIME  (10 * 60.0)  /* 10 minutes */

/* connection takenby */
#define C_SETUP -2 /* being setup */
#define C_FREE  -1 /* free */
#define C_IN  0    /* not taken */
/* > 0	taken by worker with id */

typedef struct _ev_cmd {
	int what;
	void *arg;
	int *done;
} ev_cmd;

struct _dispatcher {
	struct ev_async notify_ev;
	struct ev_loop *evbase;
	queue *cmd_queue;
	pthread_t tid;
	enum conntype type;
	char id;
	size_t metrics;
	size_t blackholes;
	size_t discards;
	size_t ticks;
	size_t sleeps;
	size_t prevmetrics;
	size_t prevblackholes;
	size_t prevdiscards;
	size_t prevticks;
	size_t prevsleeps;
	char running;
	char keep_running;  /* full byte for atomic access */
	router *rtr;
	router *pending_rtr;
	char route_refresh_pending;  /* full byte for atomic access */
	char hold;  /* full byte for atomic access */
	char *allowed_chars;
	char tags_supported;
	int maxinplen;
	int maxmetriclen;
};

typedef struct _listensock {
	struct ev_io ev;
	int sock;
	listener *lsnr;
	short needclose;
} listensock;

struct _connection {
	struct ev_io ev;
	ev_timer ev_timeout;
	int sock;
	z_strm *strm;
	char takenby;
	char srcaddr[24];  /* string representation of source address */
	char buf[METRIC_BUFSIZ];
	int buflen;
	char needmore:1;
	char noexpire:1;
	char metric[METRIC_BUFSIZ];
	destination dests[CONN_DESTS_SIZE];
	size_t destlen;
	struct timeval lastwork;
	unsigned int maxsenddelay;
	char isaggr:1;
	char isudp:1;
};

#define EV_CMD_SHUTDOWN     0
#define EV_CMD_RELOAD       1
#define EV_CMD_LADD         2
#define EV_CMD_LREMOVE      3
#define EV_CMD_LTRANSPLANT  4
#define EV_CMD_READ         5

typedef struct _transplantlistener {
	listener *olsnr;
	listener *nlsnr;
	router *r;
} transplantlistener;

static dispatcher **workers = NULL;
static unsigned char workercnt = 0;
static listener **listeners = NULL;
static connection **connections = NULL;
static size_t connectionslen = 0;
pthread_rwlock_t listenerslock = PTHREAD_RWLOCK_INITIALIZER;
pthread_rwlock_t connectionslock = PTHREAD_RWLOCK_INITIALIZER;
static size_t acceptedconnections = 0;
static size_t closedconnections = 0;
static size_t errorsconnections = 0;
static unsigned int sockbufsize = 0;

/* connection specific readers and closers */

/* ordinary socket */
static inline ssize_t
sockread(z_strm *strm, void *buf, size_t sze)
{
	return read(strm->hdl.sock, buf, sze);
}

static inline int
sockclose(z_strm *strm)
{
	int ret = close(strm->hdl.sock);
	free(strm);
	return ret;
}

/* udp socket */
static inline ssize_t
udpsockread(z_strm *strm, void *buf, size_t sze)
{
	ssize_t ret;
	struct udp_strm *s = &strm->hdl.udp;
	socklen_t slen = sizeof(s->saddr);
	ret = recvfrom(s->sock, buf, sze, 0, (struct sockaddr *)&s->saddr, &slen);
	if (ret <= 0)
		return ret;

	/* figure out who's calling */
	s->srcaddr[0] = '\0';
	switch (s->saddr.sin6_family) {
		case PF_INET:
			inet_ntop(s->saddr.sin6_family,
					&((struct sockaddr_in *)&s->saddr)->sin_addr,
					s->srcaddr, s->srcaddrlen);
			break;
		case PF_INET6:
			inet_ntop(s->saddr.sin6_family, &s->saddr.sin6_addr,
					s->srcaddr, s->srcaddrlen);
			break;
	}

	return ret;
}

static inline int
udpsockclose(z_strm *strm)
{
	int ret = close(strm->hdl.udp.sock);
	free(strm);
	return ret;
}

#ifdef HAVE_GZIP
/* gzip wrapped socket */
static inline ssize_t
gzipreadbuf(z_strm *strm, void *buf, size_t sze, int last_ret, int err);

static inline ssize_t
gzipread(z_strm *strm, void *buf, size_t sze)
{
	z_stream *zstrm = &(strm->hdl.gz.z);
	int ret;

	if (zstrm->avail_in + 1 >= strm->isize) {
		logerr("buffer overflow during read of gzip stream\n");
		errno = EBADMSG;
		return -1;
	}
	/* update ibuf */
	if ((Bytef *)strm->ibuf != zstrm->next_in) {
		memmove(strm->ibuf, zstrm->next_in, zstrm->avail_in);
		zstrm->next_in = (Bytef *)strm->ibuf;
		strm->ipos = zstrm->avail_in;
	}

	/* read any available data, if it fits */
	ret = strm->nextstrm->strmread(strm->nextstrm,
			strm->ibuf + strm->ipos,
			strm->isize - strm->ipos);
	if (ret > 0) {
		zstrm->avail_in += ret;
		strm->ipos += ret;
	} else if (ret < 0) {
		if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
			strm->hdl.gz.inflatemode = Z_FINISH;
	} else {
		/* ret == 0: EOF, which means we didn't read anything here, so
		 * calling inflate should be to flush whatever is in the zlib
		 * buffers, much like a read error.
		 * data in buffers may be exist, read with gzipreadbuf */
		strm->hdl.gz.inflatemode = Z_FINISH;
		return 0;
	}

	return gzipreadbuf(strm, buf, sze, ret, errno);
}

/* read data from buffer */
static inline ssize_t
gzipreadbuf(z_strm *strm, void *buf, size_t sze, int rval, int err)
{
	z_stream *zstrm = &(strm->hdl.gz.z);
	int iret;

	zstrm->next_out = (Bytef *)buf;
	zstrm->avail_out = (uInt)sze;
	zstrm->total_out = 0;

	iret = inflate(zstrm, strm->hdl.gz.inflatemode);
	switch (iret) {
		case Z_OK:  /* progress has been made */
			/* calculate the "returned" bytes */
			iret = sze - zstrm->avail_out;
			break;
		case Z_STREAM_END:  /* everything uncompressed, nothing pending */
			iret = sze - zstrm->avail_out;
			break;
		case Z_DATA_ERROR:  /* corrupt input */
			inflateSync(zstrm);
			/* return isn't much of interest, we will call inflate next
			 * time and sync again if it still fails */
			iret = -1;
			break;
		case Z_MEM_ERROR:  /* out of memory */
			logerr("out of memory during read of gzip stream\n");
			errno = ENOMEM;
			return -1;
			break;
		case Z_BUF_ERROR:  /* output buffer full or nothing to read */
			errno = EAGAIN;
			break;
		default:
			iret = -1;
	}
	if (iret < 1) {
		if (strm->ipos == strm->isize) {
			logerr("buffer overflow during read of gzip stream\n");
			errno = EBADMSG;
		} else if (rval < 0)
			errno = err ? err : EAGAIN;
	}

	return (ssize_t)iret;
}

static inline int
gzipclose(z_strm *strm)
{
	int ret = strm->nextstrm->strmclose(strm->nextstrm);
	inflateEnd(&(strm->hdl.gz.z));
	free(strm->ibuf);
	free(strm);
	return ret;
}
#endif

#ifdef HAVE_LZ4
/* lz4 wrapped socket */
static inline ssize_t
lzreadbuf(z_strm *strm, void *buf, size_t sze, int rval, int err);

static inline ssize_t
lzreadbuf(z_strm *strm, void *buf, size_t sze, int rval, int err);

static inline ssize_t
lzread(z_strm *strm, void *buf, size_t sze)
{
	int ret;
	/* update ibuf */
	if (strm->hdl.lz4.iloc > 0) {
		memmove(strm->ibuf, strm->ibuf + strm->hdl.lz4.iloc, strm->ipos - strm->hdl.lz4.iloc);
		strm->ipos -= strm->hdl.lz4.iloc;
		strm->hdl.lz4.iloc = 0;
	} else if (strm->ipos == strm->isize) {
		logerr("buffer overflow during read of lz4 stream\n");
		errno = EMSGSIZE;
		return -1;
	}

	/* read any available data, if it fits */
	ret = strm->nextstrm->strmread(strm->nextstrm,
			strm->ibuf + strm->ipos, strm->isize - strm->ipos);

	/* if EOF(0) or no-data(-1) then only get out now if the input
	 * buffer is empty because start of next frame may be waiting for us */

	if (ret > 0) {
		strm->ipos += ret;
	} else if (ret < 0) {
		if (strm->ipos == 0)
			return -1;
	} else {
		/* ret == 0, a.k.a. EOF */
		if (strm->ipos == 0)
			return 0;
	}
	return lzreadbuf(strm, buf, sze, ret, errno);
}

static inline ssize_t
lzreadbuf(z_strm *strm, void *buf, size_t sze, int rval, int err)
{
	/* attempt to decompress something from the (partial) frame that's
	 * arrived so far.  srcsize is updated to the number of bytes
	 * consumed. likewise for destsize and bytes written */
	size_t ret;
	size_t srcsize;
	size_t destsize;

	srcsize = strm->ipos - strm->hdl.lz4.iloc;
	if (srcsize == 0)	/* input buffer decompressed */
		return 0;

	destsize = sze;
	
	ret = LZ4F_decompress(strm->hdl.lz4.lz, buf, &destsize, strm->ibuf + strm->hdl.lz4.iloc, &srcsize, NULL);

	/* check for error before doing anything else */

	if (LZ4F_isError(ret)) {
		/* need reset */
		LZ4F_resetDecompressionContext(strm->hdl.lz4.lz);

		/* liblz4 doesn't allow access to the error constants so have to
		 * return a generic code */
		if (strm->hdl.lz4.iloc == 0 && strm->ipos == strm->isize) {
			logerr("Error %s reading LZ4 compressed data, input buffer overflow\n", LZ4F_getErrorName(ret));
			errno = EBADMSG;
		} else if (rval == 0 ||
				   (rval == -1 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)) {
			logerr("Error %s reading LZ4 compressed data, lost %lu bytes in input buffer\n",
				   LZ4F_getErrorName(ret), strm->ipos - strm->hdl.lz4.iloc);
			errno = EBADMSG;
		} else
			errno = err ? err : EAGAIN;

		return -1;
	}

	/* if we decompressed something, update our ibuf */

	if (destsize > 0) {
		strm->hdl.lz4.iloc += srcsize;
	} else if (destsize == 0) {
		tracef("No LZ4 data was produced\n");
		errno = err ? err : EAGAIN;
		return -1;
	}

#ifdef ENABLE_TRACE
	/* debug logging */
	if (ret == 0)
		tracef("LZ4 frame fully decoded\n");
#endif

	return (ssize_t)destsize;
}

static inline int
lzclose(z_strm *strm)
{
	int ret = strm->nextstrm->strmclose(strm->nextstrm);
	LZ4F_freeDecompressionContext(strm->hdl.lz4.lz);
	free(strm->ibuf);
	free(strm);
	return ret;
}
#endif

#ifdef HAVE_SNAPPY
/* snappy wrapped socket */
static inline ssize_t
snappyreadbuf(z_strm *strm, void *buf, size_t sze, int rval, int err);

static inline ssize_t
snappyreadbuf(z_strm *strm, void *buf, size_t sze, int rval, int err);

static inline ssize_t
snappyread(z_strm *strm, void *buf, size_t sze)
{
	char *ibuf = strm->ibuf;
	int ret;
	size_t buflen = sze;

	/* read any available data, if it fits */
	ret = strm->nextstrm->strmread(strm->nextstrm,
			ibuf + strm->ipos,
			strm->isize - strm->ipos);
	if (ret > 0) {
		strm->ipos += ret;
	} else if (ret < 0) {
		return -1;
	} else {
		/* ret == 0, a.k.a. EOF */
		return 0;
	}

	ret = snappy_uncompress(ibuf, strm->ipos, buf, &buflen);

	/* if we decompressed something, update our ibuf */
	if (ret == SNAPPY_OK) {
		strm->ipos = strm->ipos - buflen;
		memmove(ibuf, ibuf + buflen, strm->ipos);
	} else if (ret == SNAPPY_BUFFER_TOO_SMALL) {
		logerr("discarding snappy buffer: "
				"the uncompressed block is too large\n");
		strm->ipos = 0;
	}

	return (ssize_t)(ret == SNAPPY_OK ? buflen : -1);
}

static inline ssize_t
snappyreadbuf(z_strm *strm, void *buf, size_t sze, int rval, int err)
{
	return 0;
}

static inline int
snappyclose(z_strm *strm)
{
	int ret = strm->nextstrm->strmclose(strm->nextstrm);
	free(strm->ibuf);
	free(strm);
	return ret;
}
#endif

#ifdef HAVE_SSL
/* (Open|Libre)SSL wrapped socket */
static inline ssize_t
sslread(z_strm *strm, void *buf, size_t sze)
{
	return (ssize_t)SSL_read(strm->hdl.ssl, buf, (int)sze);
}

static inline int
sslclose(z_strm *strm)
{
	int sock = SSL_get_fd(strm->hdl.ssl);
	SSL_free(strm->hdl.ssl);
	free(strm);
	return close(sock);
}
#endif

static inline int waitfor_done(int *done)
{
	int i;
	if (done == NULL)
		return 0;
	usleep(10);
	for (i = 0; i < 100; i++)
	{
		if (__sync_bool_compare_and_swap(done, 1, 1))
			return 0;
		usleep(100);
	}
	return -1;
}

static void dispatch_closeconnection(connection *conn, dispatcher *self, ssize_t len);
static int dispatch_connection(connection *conn, dispatcher *self, struct timeval start);

static inline void set_done(int *done)
{
	if (done)
		__sync_bool_compare_and_swap(done, 0, 1);
}

static int dispatch_notify(dispatcher *d, ev_cmd *cmd, int *done) {
	cmd->done = done;
	if (done)
		*done = 0;
	if (queue_putback(d->cmd_queue, (char *) cmd) == 0) {
		return -1;
	}
	if (d->running) {
		ev_async_send(d->evbase, &d->notify_ev);
		return waitfor_done(done);
	} else {
		return 0;
	}
}

/*
 * Write shutdown command to command socket
 **/
static int
dispatch_notify_shutdown(dispatcher *d, int *done)
{
	ev_cmd *cmd = malloc(sizeof(ev_cmd));
	if (cmd == NULL)
		return -1;
	cmd->what = EV_CMD_SHUTDOWN;
	return dispatch_notify(d, cmd, done);
}

static int
dispatch_notify_reload(dispatcher *d, int *done)
{
	ev_cmd *cmd = malloc(sizeof(ev_cmd));
	if (cmd == NULL)
		return -1;
	cmd->what = EV_CMD_RELOAD;
	return dispatch_notify(d, cmd, done);
}

/*
 * Write accept command to command socket
 **/
/*
static int
dispatcher_notify_accept(dispatcher *d, int fd, listener *lsnr)
{
	ev_cmd cmd;
	cmd.what = EV_CMD_ACCEPT;
	cmd.fd = fd;
	cmd.arg = lsnr;
	if (write(d->notify_fd[0], &cmd, sizeof(cmd)) == sizeof(cmd))
		return -1;
	else
		return 0;
}
*/

/*
 * Write close listen socket command to command socket
 **/
/*
static int
dispatch_notify_close(dispatcher *d, listensock *lsock)
{
	ev_cmd cmd;
	cmd.what = EV_CMD_READ;
	cmd.fd = lsock->sock;
	cmd.arg = lsock;
	if (write(d->notify_fd[0], &cmd, sizeof(cmd)) == sizeof(cmd))
		return -1;
	else
		return 0;
}
*/

/*
 * Write accept command to command socket
 **/
static int
dispatch_notify_read(dispatcher *d, connection *conn, int *done)
{
	ev_cmd *cmd = malloc(sizeof(ev_cmd));
	if (cmd == NULL)
		return -1;
	cmd->what = EV_CMD_READ;
	cmd->arg = conn;
	return dispatch_notify(d, cmd, done);
}

static int __dispatch_addlistener(struct ev_loop *loop, listener *lsnr);

static int
dispatch_notify_addlistener(dispatcher *d, listener *lsnr, int *done)
{
	ev_cmd *cmd = malloc(sizeof(ev_cmd));
	if (cmd == NULL)
		return -1;
	cmd->what = EV_CMD_LADD;
	cmd->arg = lsnr;
	return dispatch_notify(d, cmd, done);
}

static int __dispatch_removelistener(struct ev_loop *loop, listener *lsnr);

static int
dispatch_notify_removelistener(dispatcher *d, listener *lsnr, int *done)
{
	ev_cmd *cmd = malloc(sizeof(ev_cmd));
	if (cmd == NULL)
		return -1;
	cmd->what = EV_CMD_LREMOVE;
	cmd->arg = lsnr;
	return dispatch_notify(d, cmd, done);
}

static int __dispatch_transplantlistener(struct ev_loop *loop, listener *olsnr, listener *nlsnr, router *r);

static int
dispatch_notify_transplantlistener(dispatcher *d, transplantlistener *tr, int *done)
{
	ev_cmd *cmd = malloc(sizeof(ev_cmd));
	if (cmd == NULL)
		return -1;
	cmd->what = EV_CMD_LTRANSPLANT;
	cmd->arg = tr;
	return dispatch_notify(d, cmd, done);
}

void dispatch_read_cb(struct ev_loop *loop, ev_io *io, int revents)
{
	connection *conn = (connection *) io;
	dispatcher *d  = (dispatcher *) ev_userdata(loop);
	struct timeval start, stop;

	gettimeofday(&start, NULL);
	if (!__sync_bool_compare_and_swap(&(conn->takenby), C_IN, d->id))
			return;
	if (__sync_bool_compare_and_swap(&(d->hold), 1, 1) && !conn->isaggr)
	{
		__sync_bool_compare_and_swap(
				&(conn->takenby), d->id, C_IN);
		return;
	}
	ev_timer_stop (loop, &conn->ev_timeout);
	if (dispatch_connection(conn, d, start) == 0) {
		logout("disconnect socket %d\n", conn->sock);
		ev_timer_stop(loop, &conn->ev_timeout);
		ev_io_stop(loop, io);
	}
	if (!conn->noexpire) {
		ev_timer_set (&conn->ev_timeout, IDLE_DISCONNECT_TIME, 0.);
		ev_timer_start (loop, &conn->ev_timeout);
	}

	gettimeofday(&stop, NULL);
	__sync_add_and_fetch(&(d->ticks), timediff(start, stop));
}

void dispatch_timeout_cb(struct ev_loop *loop, ev_timer *timer, int revents)
{
	connection *conn = (connection *) ((char *) timer - offsetof(connection, ev_timeout));
	dispatcher *d  = (dispatcher *) ev_userdata(loop);

	struct timeval start, stop;

	gettimeofday(&start, NULL);
	if (!__sync_bool_compare_and_swap(&(conn->takenby), d->id, d->id))
			return;
	ev_io_stop(loop, &conn->ev);
	ev_timer_stop(loop, &conn->ev_timeout);
	dispatch_closeconnection(conn, d, 0);

	gettimeofday(&stop, NULL);
	__sync_add_and_fetch(&(d->ticks), timediff(start, stop));
}

void dispatch_accept_cb(struct ev_loop *loop, ev_io *io, int revents)
{
	ev_io_sock *lsock = (ev_io_sock *) io;
    int client;
	struct sockaddr addr;
	socklen_t addrlen = sizeof(addr);

	if ((client = accept(lsock->sock, &addr, &addrlen)) < 0)
	{
		logerr("dispatch: failed to "
				"accept() new connection on socket %d: %s\n",
				lsock->sock, strerror(errno));
		dispatch_check_rlimit_and_warn();
		__sync_add_and_fetch(&errorsconnections, 1);
		return;
	}

	(void) fcntl(client, F_SETFL, O_NONBLOCK);
	dispatch_addconnection(client, lsock->lsnr, dispatch_worker_with_low_connections());
}

/**
 * Read command from socket callback
 */
static void
dispatch_cmd_cb(struct ev_loop *loop, ev_async *io, int revents)
{
	dispatcher *d  = (dispatcher *) ev_userdata(loop);
	ev_cmd *cmd = (ev_cmd *) queue_dequeue(d->cmd_queue);
	if (cmd) {
		/* logout("dispatch %d: set %d\n", d->id, cmd->what); */
		switch (cmd->what) {
			case EV_CMD_SHUTDOWN:
				/* logout("shutdown\n"); */
				ev_break(loop, EVBREAK_ONE);
				break;
			case EV_CMD_RELOAD:
				{
					if (__sync_bool_compare_and_swap(&(d->route_refresh_pending), 1, 1)) {
						d->rtr = d->pending_rtr;
						d->pending_rtr = NULL;
						__sync_bool_compare_and_swap(&(d->route_refresh_pending), 1, 0);
						__sync_and_and_fetch(&(d->hold), 0);
					}
				}
				break;
			case EV_CMD_LADD:
				{
					listener *lsnr = (listener *) cmd->arg;
					__dispatch_addlistener(loop, lsnr);
				}
				break;
			case EV_CMD_LREMOVE:
				{
					listener *lsnr = (listener *) cmd->arg;
					__dispatch_removelistener(loop, lsnr);
				}
				break;
			case EV_CMD_LTRANSPLANT:
				{
					transplantlistener *tr = (transplantlistener *) cmd->arg;
					__dispatch_transplantlistener(loop, tr->olsnr, tr->nlsnr, tr->r);
				}
				break;
			case EV_CMD_READ:
				{
					connection *conn = (connection *) cmd->arg;
					ev_io_init(&conn->ev, dispatch_read_cb, conn->sock, EV_READ);
					ev_io_start(loop, &conn->ev);
					if (!conn->noexpire) {
						ev_timer_init (&conn->ev_timeout, dispatch_timeout_cb, IDLE_DISCONNECT_TIME, 0.);
						ev_timer_start (loop, &conn->ev_timeout);
					}
				}
				break;
			default:
				logerr("dispatcher %d: unknown cmd %d\n", d->id, cmd->what);
		}
		set_done(cmd->done);
	}
}

/**
 * Helper function to try and be helpful to the user.  If errno
 * indicates no new fds could be made, checks what the current max open
 * files limit is, and if it's close to what we have in use now, write
 * an informative message to stderr.
 */
void
dispatch_check_rlimit_and_warn(void)
{
	if (errno == EISCONN || errno == EMFILE) {
		struct rlimit ofiles;
		/* rlimit can be changed for the running process (at least on
		 * Linux 2.6+) so refetch this value every time, should only
		 * occur on errors anyway */
		if (getrlimit(RLIMIT_NOFILE, &ofiles) < 0)
			ofiles.rlim_max = 0;
		if (ofiles.rlim_max != RLIM_INFINITY && ofiles.rlim_max > 0)
			logerr("process configured maximum connections = %d, "
					"consider raising max open files/max descriptor limit\n",
					(int)ofiles.rlim_max);
	}
}

dispatcher **
dispatch_workers(void)
{
	return &workers[1];
}

dispatcher *
dispatch_listener_worker(void)
{
	return workers[0];
}

dispatcher *
dispatch_worker_with_low_connections(void)
{
	return workers[0];
}

/*
 * Get workers count
 */
char
dispatch_workercnt(void)
{
	return workercnt;
}

int
dispatch_connection_to_worker(dispatcher *d, connection *conn,  int *done)
{
	return dispatch_notify_read(d, conn, done);
}

connection *
dispatch_get_connection(int c)
{
	return connections[c];
}

int
dispatch_addlistener(listener *lsnr)
{
	int done = 0;
	int res = dispatch_notify_addlistener(workers[0], lsnr, &done);
	if (res != 0) {
		logerr("dispatch: failed to notify addlistener\n");
		return 1;
	}
	return waitfor_done(&done);
}

/**
 * Adds an (initial) listener socket to the chain of connections.
 * Listener sockets are those which need to be accept()-ed on.
 */
static int
__dispatch_addlistener(struct ev_loop *loop, listener *lsnr)
{
	int c;
	ev_io_sock *socks;
	dispatcher *d = dispatch_listener_worker();

	if (lsnr->ctype == CON_UDP) {
		/* Adds a pseudo-listener for datagram (UDP) sockets, which is
		 * pseudo, for in fact it adds a new connection, but makes sure
		 * that connection won't be closed after being idle, and won't
		 * count that connection as an incoming connection either. */
		for (socks = lsnr->socks; socks->sock != -1; socks++) {
			c = dispatch_addconnection(socks->sock, lsnr, d);

			if (c == -1)
				return 1;

			connections[c]->noexpire = 1;
			if (lsnr->ctype == CON_UDP) {
				connections[c]->isudp = 1;
			}
			acceptedconnections--;
		}

		return 0;
	}

	pthread_rwlock_wrlock(&listenerslock);
	for (c = 0; c < MAX_LISTENERS; c++) {
		if (listeners[c] == NULL) {
			listeners[c] = lsnr;
			for (socks = lsnr->socks; socks->sock != -1; socks++) {
				(void) fcntl(socks->sock, F_SETFL, O_NONBLOCK);
				socks->lsnr = lsnr;
				ev_io_init(&socks->ev, dispatch_accept_cb, socks->sock, EV_READ);
				ev_io_start(loop, &socks->ev);
  			}
			break;
		}
	}
	if (c == MAX_LISTENERS) {
		logerr("cannot add new listener: "
				"no more free listener slots (max = %d)\n",
				MAX_LISTENERS);
		pthread_rwlock_unlock(&listenerslock);
		return 1;
	}
	pthread_rwlock_unlock(&listenerslock);

	return 0;
}

int
dispatch_removelistener(listener *lsnr)
{
	int done = 0;
	int res = dispatch_notify_removelistener(workers[0], lsnr, &done);
	if (res != 0) {
		logerr("dispatch: failed to notify removelistener\n");
		return 1;
	}
	return waitfor_done(&done);
}

/**
 * Remove listener from the listeners list.  Each removal will incur a
 * global lock.  Frequent usage of this function is not anticipated.
 */
static int
__dispatch_removelistener(struct ev_loop *loop, listener *lsnr)
{
	int c;
	ev_io_sock *socks;

	if (lsnr->ctype != CON_UDP) {
		pthread_rwlock_wrlock(&listenerslock);
		/* find connection */
		for (c = 0; c < MAX_LISTENERS; c++)
			if (listeners[c] != NULL && listeners[c] == lsnr)
				break;
		if (c == MAX_LISTENERS) {
			/* not found?!? */
			/* logerr("dispatch: cannot find listener to remove!\n"); */
			pthread_rwlock_unlock(&listenerslock);
			return 0;
		}
		listeners[c] = NULL;
		pthread_rwlock_unlock(&listenerslock);
	}
	/* cleanup */
#ifdef HAVE_SSL
	if ((lsnr->transport & ~0xFFFF) == W_SSL)
		SSL_CTX_free(lsnr->ctx);
#endif
	for (socks = lsnr->socks; socks->sock != -1; socks++) {
		ev_io_stop(loop, &socks->ev);
		close(socks->sock);
		logout("listener: close socket %d\n", socks->sock);
		socks->sock = -1;
	}
	if (lsnr->saddrs) {
		freeaddrinfo(lsnr->saddrs);
		lsnr->saddrs = NULL;
	}
	return 0;
}

int
dispatch_transplantlistener(listener *olsnr, listener *nlsnr, router *r)
{
	int done = 0;
	transplantlistener tr;
	tr.olsnr = olsnr;
	tr.nlsnr = nlsnr;
	tr.r = r;
	int res = dispatch_notify_transplantlistener(workers[0], &tr, &done);
	if (res != 0) {
		logerr("dispatch: failed to notify transplantlistener (%d)\n", errno);
		return 1;
	}
	return waitfor_done(&done);

}

/**
 * Copy over all state related things from olsnr to nlsnr and ensure
 * olsnr can be discarded (that is, thrown away without calling
 * dispatch_removelistener).
 */
static int
__dispatch_transplantlistener(struct ev_loop *loop, listener *olsnr, listener *nlsnr, router *r)
{
	int c;

	pthread_rwlock_wrlock(&listenerslock);
	for (c = 0; c < MAX_LISTENERS; c++) {
		if (listeners[c] == olsnr) {
			router_transplant_listener_socks(r, olsnr, nlsnr);
#ifdef HAVE_SSL
			if ((nlsnr->transport & ~0xFFFF) == W_SSL)
				nlsnr->ctx = olsnr->ctx;
#endif
			if (olsnr->saddrs) {
				freeaddrinfo(olsnr->saddrs);
				olsnr->saddrs = NULL;
			}
			listeners[c] = nlsnr;
			break;  /* found and done */
		}
	}
	pthread_rwlock_unlock(&listenerslock);
	return 0;
}

#define CONNGROWSZ  1024

/**
 * Adds a connection socket to the chain of connections.
 * Connection sockets are those which need to be read from.
 * Returns the connection id, or -1 if a failure occurred.
 */
int
dispatch_addconnection(int sock, listener *lsnr, dispatcher *d)
{
	size_t c;
	struct sockaddr_in6 saddr;
	socklen_t saddr_len = sizeof(saddr);
#if defined(HAVE_GZIP) || defined(HAVE_LZ4) || defined(HAVE_SNAPPY)
	int compress_type;
	char *ibuf;
#endif

	pthread_rwlock_rdlock(&connectionslock);
	for (c = 0; c < connectionslen; c++) {
		if (connections[c] == NULL) {
			connections[c] = malloc(sizeof(connection));
			connections[c]->takenby = C_SETUP;
			break;
		} else if (__sync_bool_compare_and_swap(&(connections[c]->takenby), C_FREE, C_SETUP))
			break;
	}
	pthread_rwlock_unlock(&connectionslock);

	if (c == connectionslen) {
		connection **newlst;

		pthread_rwlock_wrlock(&connectionslock);
		if (connectionslen > c) {
			/* another dispatcher just extended the list */
			pthread_rwlock_unlock(&connectionslock);
			return dispatch_addconnection(sock, lsnr, d);
		}
		newlst = realloc(connections,
				sizeof(connection *) * (connectionslen + CONNGROWSZ));
		if (newlst == NULL) {
			logerr("cannot add new connection: "
					"out of memory allocating more slots (max = %zu)\n",
					connectionslen);

			pthread_rwlock_unlock(&connectionslock);
			return -1;
		}

		for (c = connectionslen; c < connectionslen + CONNGROWSZ; c++) {
			newlst[c] = NULL;
		}
		connections = newlst;
		c = connectionslen;  /* for the setup code below */
		connections[c] = malloc(sizeof(connection));
		connections[c]->takenby = C_SETUP;
		connectionslen += CONNGROWSZ;

		pthread_rwlock_unlock(&connectionslock);
	}

	/* figure out who's calling */
	if (getpeername(sock, (struct sockaddr *)&saddr, &saddr_len) == 0) {
		snprintf(connections[c]->srcaddr, sizeof(connections[c]->srcaddr),
				"(unknown)");
		switch (saddr.sin6_family) {
			case PF_INET:
				inet_ntop(saddr.sin6_family,
						&((struct sockaddr_in *)&saddr)->sin_addr,
						connections[c]->srcaddr, sizeof(connections[c]->srcaddr));
				break;
			case PF_INET6:
				inet_ntop(saddr.sin6_family, &saddr.sin6_addr,
						connections[c]->srcaddr, sizeof(connections[c]->srcaddr));
				break;
		}
	}

	(void) fcntl(sock, F_SETFL, O_NONBLOCK);
	if (sockbufsize > 0) {
		if (setsockopt(sock, SOL_SOCKET, SO_RCVBUF,
				&sockbufsize, sizeof(sockbufsize)) != 0)
			;
	}
	connections[c]->sock = sock;
	connections[c]->strm = malloc(sizeof(z_strm));
	if (connections[c]->strm == NULL) {
		logerr("cannot add new connection: "
				"out of memory allocating stream\n");
		__sync_bool_compare_and_swap(&(connections[c]->takenby), C_SETUP, C_FREE);
		return -1;
	}

	/* set socket or SSL connection */
	connections[c]->strm->nextstrm = NULL;
	connections[c]->strm->strmreadbuf = NULL;
	if (lsnr == NULL || (lsnr->transport & ~0xFFFF) != W_SSL) {
		if (lsnr == NULL || lsnr->ctype != CON_UDP) {
			connections[c]->strm->hdl.sock = sock;
			connections[c]->strm->strmread = &sockread;
			connections[c]->strm->strmclose = &sockclose;
		} else {
			connections[c]->strm->hdl.udp.sock = sock;
			connections[c]->strm->hdl.udp.srcaddr =
				connections[c]->srcaddr;
			connections[c]->strm->hdl.udp.srcaddrlen =
				sizeof(connections[c]->srcaddr);
			connections[c]->strm->strmread = &udpsockread;
			connections[c]->strm->strmclose = &udpsockclose;
		}
#ifdef HAVE_SSL
	} else {
		if ((connections[c]->strm->hdl.ssl = SSL_new(lsnr->ctx)) == NULL) {
			logerr("cannot add new connection: %s\n",
					ERR_reason_error_string(ERR_get_error()));
			free(connections[c]->strm);
			__sync_bool_compare_and_swap(&(connections[c]->takenby), C_SETUP, C_FREE);
			return -1;
		};
		SSL_set_fd(connections[c]->strm->hdl.ssl, sock);
		SSL_set_accept_state(connections[c]->strm->hdl.ssl);

		connections[c]->strm->strmread = &sslread;
		connections[c]->strm->strmclose = &sslclose;
#endif
	}

#if defined(HAVE_GZIP) || defined(HAVE_LZ4) || defined(HAVE_SNAPPY)
	if (lsnr == NULL)
		compress_type = 0;
	else
		compress_type = lsnr->transport & 0xFFFF;

	/* allocate input buffer */
	if (compress_type == W_GZIP || compress_type == W_LZ4 || compress_type == W_SNAPPY) {
		ibuf = malloc(METRIC_BUFSIZ);
		if (ibuf == NULL) {
			logerr("cannot add new connection: "
					"out of memory allocating stream ibuf\n");
			free(connections[c]->strm);
			__sync_bool_compare_and_swap(&(connections[c]->takenby), C_SETUP, C_FREE);
			return -1;
		}
	} else
		ibuf = NULL;
#endif


	/* setup decompressor */
	if (lsnr == NULL) {
		/* do nothing, catch case only */
	}
#ifdef HAVE_GZIP
	else if (compress_type == W_GZIP) {
		z_strm *zstrm = malloc(sizeof(z_strm));
		if (zstrm == NULL) {
			logerr("cannot add new connection: "
					"out of memory allocating gzip stream\n");
			free(ibuf);
			free(connections[c]->strm);
			__sync_bool_compare_and_swap(&(connections[c]->takenby), C_SETUP, C_FREE);
			return -1;
		}
		zstrm->hdl.gz.z.zalloc = Z_NULL;
		zstrm->hdl.gz.z.zfree = Z_NULL;
		zstrm->hdl.gz.z.opaque = Z_NULL;
		zstrm->ipos = 0;
		inflateInit2(&(zstrm->hdl.gz.z), 15 + 16);
		zstrm->hdl.gz.inflatemode = Z_SYNC_FLUSH;

		zstrm->ibuf = ibuf;
		zstrm->isize = METRIC_BUFSIZ;
		zstrm->hdl.gz.z.next_in = (Bytef *)zstrm->ibuf;
		zstrm->hdl.gz.z.avail_in = 0;

		zstrm->strmread = &gzipread;
		zstrm->strmreadbuf = &gzipreadbuf;
		zstrm->strmclose = &gzipclose;
		zstrm->nextstrm = connections[c]->strm;
		connections[c]->strm = zstrm;
	}
#endif
#ifdef HAVE_LZ4
	else if (compress_type == W_LZ4) {
		z_strm *lzstrm = malloc(sizeof(z_strm));
		if (lzstrm == NULL) {
			logerr("cannot add new connection: "
					"out of memory allocating lz4 stream\n");
			free(ibuf);
			free(connections[c]->strm);
			__sync_bool_compare_and_swap(&(connections[c]->takenby), C_SETUP, C_FREE);
			return -1;
		}
		if (LZ4F_isError(LZ4F_createDecompressionContext(&lzstrm->hdl.lz4.lz, LZ4F_VERSION))) {
			logerr("Failed to create LZ4 decompression context\n");
			free(ibuf);
			free(connections[c]->strm);
			return -1;
		}
		lzstrm->ibuf = ibuf;
		lzstrm->isize = METRIC_BUFSIZ;
		lzstrm->ipos = 0;
		lzstrm->hdl.lz4.iloc = 0;

		lzstrm->strmread = &lzread;
		lzstrm->strmreadbuf = &lzreadbuf;
		lzstrm->strmclose = &lzclose;
		lzstrm->nextstrm = connections[c]->strm;
		connections[c]->strm = lzstrm;
	}
#endif
#ifdef HAVE_SNAPPY
	else if (compress_type == W_SNAPPY) {
		z_strm *lzstrm = malloc(sizeof(z_strm));
		if (lzstrm == NULL) {
			logerr("cannot add new connection: "
					"out of memory allocating snappy stream\n");
			__sync_bool_compare_and_swap(&(connections[c]->takenby), C_SETUP, C_FREE);
			free(ibuf);
			free(connections[c]->strm);
			__sync_bool_compare_and_swap(&(connections[c]->takenby), C_SETUP, C_FREE);
			return -1;
		}

		lzstrm->ibuf = ibuf;
		lzstrm->isize = METRIC_BUFSIZ;
		lzstrm->ipos = 0;

		lzstrm->strmread = &snappyread;
		lzstrm->strmreadbuf = &snappyreadbuf;
		lzstrm->strmclose = &snappyclose;
		lzstrm->nextstrm = connections[c]->strm;
		connections[c]->strm = lzstrm;
	}
#endif

	connections[c]->buflen = 0;
	connections[c]->needmore = 0;
	connections[c]->noexpire = noexpire;
	connections[c]->isaggr = 0;
	connections[c]->isudp = 0;
	connections[c]->destlen = 0;
	gettimeofday(&connections[c]->lastwork, NULL);
	/* after this dispatchers will pick this connection up */
	__sync_bool_compare_and_swap(&(connections[c]->takenby), C_SETUP, C_IN);
	if (dispatch_connection_to_worker(d, connections[c], NULL) == -1) {
		connections[c]->strm->strmclose(connections[c]->strm);
		__sync_bool_compare_and_swap(&(connections[c]->takenby), C_IN, C_SETUP);
		__sync_add_and_fetch(&errorsconnections, 1);
		logerr("dispatch %d: failed to "
				"pass new connection on socket %d\n",
				d->id, sock);
		return -1;
	}
	__sync_add_and_fetch(&acceptedconnections, 1);

	return c;
}

/**
 * Adds a connection which we know is from an aggregator, so direct
 * pipe.  This is different from normal connections that we don't want
 * to count them, never expire them, and want to recognise them when
 * we're doing reloads.
 */
int
dispatch_addconnection_aggr(int sock)
{
	int c = dispatch_addconnection(sock, NULL, dispatch_listener_worker());

	if (c == -1)
		return 1;

	connections[c]->noexpire = 1;
	connections[c]->isaggr = 1;
	acceptedconnections--;

	return 0;
}

inline static char
dispatch_process_dests(connection *conn, dispatcher *self, struct timeval now)
{
	int i;
	char force;

	if (conn->destlen > 0) {
		if (conn->maxsenddelay == 0)
			conn->maxsenddelay = ((rand() % 750) + 250) * 1000;
		/* force when aggr (don't stall it) or after timeout */
		force = conn->isaggr ? 1 :
			timediff(conn->lastwork, now) > conn->maxsenddelay;
		for (i = 0; i < conn->destlen; i++) {
			tracef("dispatcher %d, connfd %d, metric %s, queueing to %s:%d\n",
					self->id, conn->sock, conn->dests[i].metric,
					server_ip(conn->dests[i].dest),
					server_port(conn->dests[i].dest));
			if (server_send(conn->dests[i].dest, conn->dests[i].metric, force) == 0)
				break;
		}
		if (i < conn->destlen) {
			conn->destlen -= i;
			memmove(&conn->dests[0], &conn->dests[i],
					(sizeof(destination) * conn->destlen));
			return 0;
		} else {
			/* finally "complete" this metric */
			conn->destlen = 0;
			conn->lastwork = now;
		}
	}

	return 1;
}

/* Extract received metrics from buffer */
static void
dispatch_received_metrics(connection *conn, dispatcher *self)
{
	/* Metrics look like this: metric_path value timestamp\n
	 * due to various messups we need to sanitise the
	 * metrics_path here, to ensure we can calculate the metric
	 * name off the filesystem path (and actually retrieve it in
	 * the web interface).
	 * Since tag support, metrics can look like this:
	 *   metric_path[;tag=value ...] value timestamp\n
	 * where the tag=value part can be repeated.  It should not be
	 * sanitised, however. */
	char *p, *q, *firstspace, *lastnl;
	char search_tags;

	q = conn->metric;
	firstspace = NULL;
	lastnl = NULL;
	search_tags = self->tags_supported;
	for (p = conn->buf; p - conn->buf < conn->buflen; p++) {
		if (*p == '\n' || *p == '\r') {
			/* end of metric */
			lastnl = p;

			/* just a newline on it's own? some random garbage?
			 * do we exceed the set limits? drop */
			if (q == conn->metric || firstspace == NULL ||
					q - conn->metric > self->maxinplen - 1 ||
					firstspace - conn->metric > self->maxmetriclen)
			{
				tracef("dispatcher %d, connfd %d, discard metric %s",
						self->id, conn->sock, conn->metric);
				__sync_add_and_fetch(&(self->discards), 1);
				q = conn->metric;
				firstspace = NULL;
				continue;
			}

			__sync_add_and_fetch(&(self->metrics), 1);
			/* add newline and terminate the string, we can do this
			 * because we substract one from buf and we always store
			 * a full metric in buf before we copy it to metric
			 * (which is of the same size) */
			*q++ = '\n';
			*q = '\0';

			/* perform routing of this metric */
			tracef("dispatcher %d, connfd %d, metric %s",
					self->id, conn->sock, conn->metric);
			__sync_add_and_fetch(&(self->blackholes),
					router_route(self->rtr,
						conn->dests, &conn->destlen, CONN_DESTS_SIZE,
						conn->srcaddr,
						conn->metric, firstspace, self->id - 1));
			tracef("dispatcher %d, connfd %d, destinations %zd\n",
					self->id, conn->sock, conn->destlen);

			/* restart building new one from the start */
			q = conn->metric;
			firstspace = NULL;
			search_tags = self->tags_supported;

			gettimeofday(&conn->lastwork, NULL);
			conn->maxsenddelay = 0;
			/* send the metric to where it is supposed to go */
			if (dispatch_process_dests(conn, self, conn->lastwork) == 0)
				break;
		} else if (*p == ' ' || *p == '\t' || *p == '.') {
			/* separator */
			if (q == conn->metric) {
				/* make sure we skip this on next iteration to
				 * avoid an infinite loop, issues #8 and #51 */
				lastnl = p;
				continue;
			}
			if (*p == '\t')
				*p = ' ';
			if (*p == ' ' && firstspace == NULL) {
				if (*(q - 1) == '.')
					q--;  /* strip trailing separator */
				firstspace = q;
				*q++ = ' ';
			} else {
				/* metric_path separator or space,
				 * - duplicate elimination
				 * - don't start with separator/space */
				if (*(q - 1) != *p && (q - 1) != firstspace)
					*q++ = *p;
			}
		} else if (search_tags && *p == ';') {
			/* copy up to next space */
			search_tags = 0;
			firstspace = q;
			*q++ = *p;
		} else if (
				*p != '\0' && (
					firstspace != NULL ||
					(*p >= 'a' && *p <= 'z') ||
					(*p >= 'A' && *p <= 'Z') ||
					(*p >= '0' && *p <= '9') ||
					strchr(self->allowed_chars, *p)
				)
			)
		{
			/* copy char */
			*q++ = *p;
		} else {
			/* something barf, replace by underscore */
			*q++ = '_';
		}
	}
	conn->needmore = q != conn->metric;
	if (lastnl != NULL) {
		/* move remaining stuff to the front */
		conn->buflen -= lastnl + 1 - conn->buf;
		tracef("dispatcher %d, conn->buf: %p, lastnl: %p, diff: %zd, "
				"conn->buflen: %d, sizeof(conn->buf): %lu, "
				"memmove(%d, %lu, %d)\n",
				self->id,
				conn->buf, lastnl, lastnl - conn->buf,
				conn->buflen, sizeof(conn->buf),
				0, lastnl + 1 - conn->buf, conn->buflen + 1);
		tracef("dispatcher %d, pre conn->buf: %s\n", self->id, conn->buf);
		/* copy last NULL-byte for debug tracing */
		memmove(conn->buf, lastnl + 1, conn->buflen + 1);
		tracef("dispatcher %d, post conn->buf: %s\n", self->id, conn->buf);
	}
}

static void
dispatch_closeconnection(connection *conn, dispatcher *self, ssize_t len)
{
	tracef("dispatcher: %d, connfd: %d, len: %zd [%s], disconnecting\n",
		self->id, conn->sock, len,
		len < 0 ? strerror(errno) : "");
	__sync_add_and_fetch(&closedconnections, 1);
	conn->strm->strmclose(conn->strm);

		/* flag this connection as no longer in use, unless there is
		 * pending metrics to send */
	__sync_bool_compare_and_swap(&(conn->takenby), self->id, C_FREE);
}

/**
 * Look at conn and see if works needs to be done.  If so, do it.  This
 * function operates on an (exclusive) lock on the connection it serves.
 * Schematically, what this function does is like this:
 *
 *   read (partial) data  <----
 *         |                   |
 *         v                   |
 *   split and clean metrics   |
 *         |                   |
 *         v                   |
 *   route metrics             | feedback loop
 *         |                   | (stall client)
 *         v                   |
 *   send 1st attempt          |
 *         \                   |
 *          v*                 | * this is optional, but if a server's
 *   retry send (<1s)  --------    queue is full, the client is stalled
 *      block reads
 *  return 1 - success,
 *         0 - connection close (due remote shutdown or fatal error)
 *        -1 - can retry
 */
static int
dispatch_connection(connection *conn, dispatcher *self, struct timeval start)
{
	ssize_t len;
	int err;

	/* TODO: timeout */
	/* first try to resume any work being blocked */
	if (dispatch_process_dests(conn, self, start) == 0) {
		__sync_bool_compare_and_swap(&(conn->takenby), self->id, C_IN);
		return 0;
	}

	len = -2;
	/* try to read more data, if that succeeds, or we still have data
	 * left in the buffer, try to process the buffer */
	if (
			(!conn->needmore && conn->buflen > 0) ||
			(len = conn->strm->strmread(conn->strm,
						conn->buf + conn->buflen,
						(sizeof(conn->buf) - 1) - conn->buflen)) > 0
	   )
	{
		ssize_t ilen;
		if (len > 0) {
			conn->buflen += len;
			tracef("dispatcher %d, connfd %d, read %zd bytes from socket\n",
					self->id, conn->sock, len);
#ifdef ENABLE_TRACE
			conn->buf[conn->buflen] = '\0';
#endif
		}

		err = errno;
		dispatch_received_metrics(conn, self);
		if (conn->strm->strmreadbuf != NULL) {
			while((ilen = conn->strm->strmreadbuf(conn->strm,
							conn->buf + conn->buflen,
							(sizeof(conn->buf) - 1) - conn->buflen, len, err)) > 0) {
				conn->buflen += ilen;
				tracef("dispatcher %d, connfd %d, read %zd bytes from socket\n",
						self->id, conn->sock, ilen);
#ifdef ENABLE_TRACE
				conn->buf[conn->buflen] = '\0';
#endif
				dispatch_received_metrics(conn, self);
			}

			if (ilen < 0 && errno == ENOMEM) /* input buffer overflow */
				len = -1;
		}
	}
	if (len == -1 && (errno == EINTR ||
				errno == EAGAIN ||
				errno == EWOULDBLOCK))
	{
		/* nothing available/no work done */
		struct timeval stop;
		gettimeofday(&stop, NULL);
		__sync_bool_compare_and_swap(&(conn->takenby), self->id, C_IN);
		return -1;
	}
	if (len == -1 || len == 0 || conn->isudp) {  /* error + EOF */
		/* we also disconnect the client in this case if our reading
		 * buffer is full, but we still need more (read returns 0 if the
		 * size argument is 0) -> this is good, because we can't do much
		 * with such client */

		if (conn->isaggr || conn->isudp) {
			/* reset buffer only (UDP/aggregations) and move on */
			conn->needmore = 1;
			conn->buflen = 0;
			__sync_bool_compare_and_swap(&(conn->takenby), self->id, C_IN);

			return -1;
		} else if (conn->destlen == 0) {
			dispatch_closeconnection(conn, self, len);
			return 0;
		}
	}

	/* "release" this connection again */
	__sync_bool_compare_and_swap(&(conn->takenby), self->id, C_IN);

	return 1;
}

/**
 * pthread compatible routine that handles connections and processes
 * whatever comes in on those.
 */
static void *
dispatch_runner(void *arg)
{
	dispatcher *d = (dispatcher *)arg;
	size_t len = queue_len(d->cmd_queue);
	size_t i;
	for (i = 0; i < len; i++) {
		dispatch_cmd_cb(d->evbase, &d->notify_ev, 0);
	}
	d->running = 1;
	ev_run (d->evbase, 0);
	d->running = 0;
	ev_async_stop(d->evbase, &d->notify_ev);
	ev_loop_destroy(d->evbase);
	return NULL;
}

/**
 * Starts a new dispatcher for the given type and with the given id.
 * Returns its handle.
 */
static dispatcher *
dispatch_new(
		unsigned char id,
		enum conntype type
	)
{
	dispatcher *ret = malloc(sizeof(dispatcher));

	if (ret == NULL)
		return NULL;
	ret->evbase =  ev_loop_new(EVFLAG_AUTO);
	if (ret->evbase == NULL) {
		free(ret);
		return NULL;
	}
	ret->type = type;

	ret->running = 0;
	ret->id = id + 1;  /* ensure > 0 */
	ret->cmd_queue = queue_new(100);
	ret->keep_running = 1;
	ret->route_refresh_pending = 0;
	ret->hold = 0;

	ret->metrics = 0;
	ret->blackholes = 0;
	ret->discards = 0;
	ret->ticks = 0;
	ret->sleeps = 0;
	ret->prevmetrics = 0;
	ret->prevblackholes = 0;
	ret->prevticks = 0;
	ret->prevsleeps = 0;

	return ret;
}

/**
 * Allocate workers. On errors return -1
 */
char
dispatch_workers_alloc(char count)
{
	workers = malloc(sizeof(dispatcher *) *
			(1/*lsnr*/ + count + 1/*sentinel*/));
	if (workers == NULL)
		workercnt = -1;
	else {
		unsigned char id;
		workercnt = 0;
		/* ensure the listener id is at the end for regex_t array hack */
		if ((workers[0] = dispatch_new(0, LISTENER)) == NULL) {
			logerr("failed to add listener dispatcher\n");
			return -1;
		}
		for (id = 1; id <= count; id++) {
			if ((workers[id] = dispatch_new(id, CONNECTION)) == NULL) {
				logerr("failed to add connection dispatcher\n");
				return -1;
			}
		}
		workers[id + 1] = NULL; /* sentinel */
		workercnt = id;
	}
	return workercnt;
}

static int
dispatch_start(
		unsigned char id,
		router *r,
		char *allowed_chars,
		int maxinplen,
		int maxmetriclen
	)
{
	dispatcher *d = workers[id];
	if (d->type == CONNECTION && r == NULL) {
		return -1;
	}
	d->rtr = r;
	d->allowed_chars = allowed_chars;
	d->tags_supported = 0;
	d->maxinplen = maxinplen;
	d->maxmetriclen = maxmetriclen;

	/* switch tag support on when the user didn't allow ';' as valid
	 * character in metrics */
	if (allowed_chars != NULL && strchr(allowed_chars, ';') == NULL)
		d->tags_supported = 1;

	ev_async_init(&d->notify_ev, dispatch_cmd_cb);
	ev_async_start(d->evbase, &d->notify_ev);

	ev_set_userdata(d->evbase, d);

	if (pthread_create(&d->tid, NULL, dispatch_runner, d) == 0) {
		return 0;
	} else {
		return -1;
	}
}

void
dispatch_set_bufsize(unsigned int nsockbufsize)
{
	sockbufsize = nsockbufsize;
}

/**
 * Initialise the listeners array.  This is a one-time allocation that
 * currently never is extended.  This code does no locking, as it
 * assumes to be run before any access to listeners occur.
 */
char
dispatch_init_listeners()
{
	int i;
	/* once all-or-nothing allocation */
	if ((listeners = malloc(sizeof(listener *) * MAX_LISTENERS)) == NULL)
		return 1;
	for (i = 0; i < MAX_LISTENERS; i++)
		listeners[i] = NULL;

	return 0;
}

/**
 * Starts a new dispatchers specialised in handling incoming data
 * on existing and incoming connections
 */
unsigned char
dispatch_start_connections(router *r, char *allowed_chars,
		int maxinplen, int maxmetriclen)
{
	unsigned char id = 0;
	for (id = 0; id < workercnt; id++) {
		if (dispatch_start(id, r, allowed_chars, maxinplen, maxmetriclen) == -1) {
			logerr("failed to start worker %d\n", id);
			return -1;
		}
	}
	return id;
}

/**
 * Signals this dispatcher to stop whatever it's doing.
 */
void
dispatch_stop(dispatcher *d)
{
	__sync_bool_compare_and_swap(&(d->keep_running), 1, 0);
	dispatch_notify_shutdown(d, NULL);
}

void
dispatchs_stop(void)
{
	int id;
	for (id = 0; id < workercnt; id++)
		dispatch_stop(workers[id]);
}

/**
 * Shuts down dispatcher d.  Returns when the dispatcher has terminated.
 */
void
dispatch_shutdown(dispatcher *d)
{
	dispatch_stop(d);
	pthread_join(d->tid, NULL);
}

void
dispatch_shutdown_id(unsigned char id)
{
	dispatch_stop(workers[id]);
	pthread_join(workers[id]->tid, NULL);
}

/**
 * Free up resources taken by dispatcher d.  The caller should make sure
 * the dispatcher has been shut down at this point.
 */
void
dispatch_free(dispatcher *d)
{
	free(d);
}

void
dispatchs_free()
{
	int id;
	for (id = 1; id < 1 + workercnt; id++)
		dispatch_free(workers[id]);
	free(workers);
}

/**
 * Requests this dispatcher to stop processing connections.  As soon as
 * schedulereload finishes reloading the routes, this dispatcher will
 * un-hold and continue processing connections.
 * Returns when the dispatcher is no longer doing work.
 */

inline void
dispatch_hold(dispatcher *d)
{
	__sync_bool_compare_and_swap(&(d->hold), 0, 1);
}

void
dispatchs_hold(void)
{
	int id;
	for (id = 1; id < 1 + workercnt; id++)
		dispatch_hold(workers[id]);
}

/**
 * Schedules routes r to be put in place for the current routes.  The
 * replacement is performed at the next cycle of the dispatcher.
 */
inline void
dispatch_schedulereload(dispatcher *d, router *r)
{
	d->pending_rtr = r;
	__sync_bool_compare_and_swap(&(d->route_refresh_pending), 0, 1);
	dispatch_notify_reload(d, NULL);
}

void
dispatchs_schedulereload(router *r)
{
	int id;
	for (id = 1; id < 1 + workercnt; id++)
		dispatch_schedulereload(workers[id], r);
}

/**
 * Returns true if the routes scheduled to be reloaded by a call to
 * dispatch_schedulereload() have been activated.
 */
inline char
dispatch_reloadcomplete(dispatcher *d)
{
	return __sync_bool_compare_and_swap(&(d->route_refresh_pending), 0, 0);
}

void dispatch_wait_reloadcomplete()
{
	int id;
	for (id = 1; id < 1 + workercnt; id++) {
		while (!dispatch_reloadcomplete(workers[id]))
			usleep((100 + (rand() % 200)) * 1000);  /* 100ms - 300ms */
	}
}

/**
 * Returns the wall-clock time in milliseconds consumed by this dispatcher.
 */
inline size_t
dispatch_get_ticks(dispatcher *self)
{
	return __sync_add_and_fetch(&(self->ticks), 0);
}

/**
 * Returns the wall-clock time consumed since last call to this
 * function.
 */
inline size_t
dispatch_get_ticks_sub(dispatcher *self)
{
	size_t d = dispatch_get_ticks(self) - self->prevticks;
	self->prevticks += d;
	return d;
}

/**
 * Returns the wall-clock time in milliseconds consumed while sleeping
 * by this dispatcher.
 */
inline size_t
dispatch_get_sleeps(dispatcher *self)
{
	return __sync_add_and_fetch(&(self->sleeps), 0);
}

/**
 * Returns the wall-clock time consumed while sleeping since last call
 * to this function.
 */
inline size_t
dispatch_get_sleeps_sub(dispatcher *self)
{
	size_t d = dispatch_get_sleeps(self) - self->prevsleeps;
	self->prevsleeps += d;
	return d;
}

/**
 * Returns the number of metrics dispatched since start.
 */
inline size_t
dispatch_get_metrics(dispatcher *self)
{
	return __sync_add_and_fetch(&(self->metrics), 0);
}

/**
 * Returns the number of metrics dispatched since last call to this
 * function.
 */
inline size_t
dispatch_get_metrics_sub(dispatcher *self)
{
	size_t d = dispatch_get_metrics(self) - self->prevmetrics;
	self->prevmetrics += d;
	return d;
}

/**
 * Returns the number of metrics that were explicitly or implicitly
 * blackholed since start.
 */
inline size_t
dispatch_get_blackholes(dispatcher *self)
{
	return __sync_add_and_fetch(&(self->blackholes), 0);
}

/**
 * Returns the number of metrics that were blackholed since last call to
 * this function.
 */
inline size_t
dispatch_get_blackholes_sub(dispatcher *self)
{
	size_t d = dispatch_get_blackholes(self) - self->prevblackholes;
	self->prevblackholes += d;
	return d;
}

/**
 * Returns the number of metrics that were discarded since start.
 */
inline size_t
dispatch_get_discards(dispatcher *self)
{
	return __sync_add_and_fetch(&(self->discards), 0);
}

/**
 * Returns the number of metrics that were discarded since last call to
 * this function.
 */
inline size_t
dispatch_get_discards_sub(dispatcher *self)
{
	size_t d = dispatch_get_discards(self) - self->prevdiscards;
	self->prevdiscards += d;
	return d;
}

/**
 * Returns the number of accepted connections thusfar.
 */
inline size_t
dispatch_get_accepted_connections(void)
{
	return __sync_add_and_fetch(&(acceptedconnections), 0);
}

/**
 * Returns the number of closed connections thusfar.
 */
inline size_t
dispatch_get_closed_connections(void)
{
	return __sync_add_and_fetch(&(closedconnections), 0);
}
