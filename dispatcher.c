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
#include <sys/resource.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "queue.h"
#include "relay.h"
#include "router.h"
#include "server.h"
#include "collector.h"
#include "dispatcher_internal.h"
#include "eventpipe.h"

#define SOCKGROWSZ  32768
#define CONNGROWSZ  1024
#define MAX_LISTENERS 32  /* hopefully enough */
#define POLL_TIMEOUT  100
#define IDLE_DISCONNECT_TIME (10 * 60)  /* 10 minutes */

/* connection takenby */
#define C_SETUP -2 /* being setup */
#define C_FREE  -1 /* free */
#define C_IN  0    /* not taken */
/* > 0	taken by worker with id */

struct _dispatcher {
	struct event_base *evbase;
	struct event *notify_ev; /* not used now, libevent initialized with ptheads locks */
	eventpipe notify_fd;
	pthread_t tid;
	enum conntype type;
	char id;
	size_t connections; /* for balance dispatchers */
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
	struct timeval stop;
};

struct _connection {
	struct event *ev;
	int sock;
	dispatcher *d;
	z_strm *strm;
	char takenby;
	char srcaddr[24];  /* string representation of source address */
	char *buf;
	size_t bufsize;
	size_t buflen;
	char metric[METRIC_BUFSIZ];
	destination dests[CONN_DESTS_SIZE];
	size_t destlen;
	struct timeval lastwork;
	unsigned int maxsenddelay;
	char needmore:1;
	char noexpire:1;
	char isaggr:1;
	char isudp:1;
};

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
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
static inline char connection_ended(ssize_t ret, int err) {
	if (ret > 0) {
		return 0;
	} else if (ret == 0) {
		return 1;
	} else if (err != EINTR && err != EAGAIN && err != EWOULDBLOCK) {
		return 1;
	} else {
		return 0;
	}
}
#pragma GCC diagnostic pop

/* ordinary socket */

static inline const char *
sockerror(z_strm *strm, int rval)
{
	(void)strm;
	(void)rval;
	return strerror(errno);
}

z_strm *
socknew(size_t isize, size_t *osize)
{
	z_strm *strm = malloc(sizeof(z_strm));
	if (strm == NULL) {
		return NULL;
	}
	strm->strmerror = &sockerror;
	*osize = isize;
	return strm;
}

static inline ssize_t
sockread(z_strm *strm, void *buf, size_t sze)
{
	/*debug */
	/*
	ssize_t n = read(strm->hdl.sock, buf, sze);
	((char *) buf)[n] = '\0';
	logout("sockread:(%d) '%s'\n", strm->hdl.sock, (char *) buf);
	return n;
	*/
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
gzipreadbuf(z_strm *strm, void *buf, size_t sze);

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
	strm->closing = connection_ended(ret, errno);
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

	return gzipreadbuf(strm, buf, sze);
}

/* read data from buffer */
static inline ssize_t
gzipreadbuf(z_strm *strm, void *buf, size_t sze)
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
		} else if (strm->closing == 0) {
			errno = EAGAIN;
		}
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

static inline const char *
gziperror(z_strm *strm, int rval)
{
	return strm->nextstrm->strmerror(strm->nextstrm, rval);
}

z_strm *
gzipnew(size_t isize, z_strm *basestrm, size_t *osize)
{
	z_strm *zstrm = malloc(sizeof(z_strm));
	if (zstrm == NULL) {
		return NULL;
	}
	zstrm->ibuf = malloc(isize);
	if (zstrm->ibuf == NULL) {
		free(zstrm);
		return NULL;
	}
	zstrm->isize = isize;
	*osize = isize;

	zstrm->hdl.gz.z.zalloc = Z_NULL;
	zstrm->hdl.gz.z.zfree = Z_NULL;
	zstrm->hdl.gz.z.opaque = Z_NULL;
	zstrm->ipos = 0;
	if (inflateInit2(&(zstrm->hdl.gz.z), 15 + 16) != Z_OK)
	{
		free(zstrm);
		return NULL;
	}
	zstrm->hdl.gz.inflatemode = Z_SYNC_FLUSH;

	zstrm->hdl.gz.z.next_in = (Bytef *)zstrm->ibuf;
	zstrm->hdl.gz.z.avail_in = 0;

	zstrm->nextstrm = basestrm;

	zstrm->strmread = &gzipread;
	zstrm->strmreadbuf = &gzipreadbuf;
	zstrm->strmclose = &gzipclose;
	zstrm->strmerror = &gziperror;

	return zstrm;
}

#endif

#ifdef HAVE_LZ4

#include "lz4settings.h"

/* lz4 wrapped socket */
static inline ssize_t
lzreadbuf(z_strm *strm, void *buf, size_t sze);

static inline ssize_t
lzread(z_strm *strm, void *buf, size_t sze)
{
	int ret;

	/* update ibuf */
	if (strm->hdl.lz4.iloc > 0) {
		memmove(strm->ibuf, strm->ibuf + strm->hdl.lz4.iloc, strm->ipos - strm->hdl.lz4.iloc);
		strm->ipos -= strm->hdl.lz4.iloc;
		strm->hdl.lz4.iloc = 0;
	} else if (strm->ipos >= strm->isize) {
		logerr("buffer overflow during read of lz4 stream\n");
		errno = ENOBUFS;
		return -1;
	}

	/* read any available data, if it fits */
	ret = strm->nextstrm->strmread(strm->nextstrm,
			strm->ibuf + strm->ipos, strm->isize - strm->ipos);
	strm->closing = connection_ended(ret, errno);
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

	return lzreadbuf(strm, buf, sze);
}

static inline ssize_t
lzreadbuf(z_strm *strm, void *buf, size_t sze)
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
			errno = ENOBUFS;
		} else if (strm->closing) {
			logerr("Error %s reading LZ4 compressed data, lost %lu bytes in input buffer\n",
				   LZ4F_getErrorName(ret), strm->ipos - strm->hdl.lz4.iloc);
			errno = EBADMSG;
		} else {
			errno = EAGAIN;
		}
		return -1;
	}

	/* if we decompressed something, update our ibuf */

	if (destsize > 0) {
		strm->hdl.lz4.iloc += srcsize;
	} else if (destsize == 0) {
		tracef("No LZ4 data was produced\n");
		if (strm->closing) {
			return 0;
		} else {
			errno = EAGAIN;
			return -1;
		}
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

static inline const char *
lzerror(z_strm *strm, int rval)
{
	return strm->nextstrm->strmerror(strm->nextstrm, rval);
}

z_strm *
lznew(size_t isize, z_strm *basestrm, size_t *osize)
{
	z_strm *lzstrm = malloc(sizeof(z_strm));
	if (lzstrm == NULL) {
		return NULL;
	}
	if (LZ4F_isError(LZ4F_createDecompressionContext(&lzstrm->hdl.lz4.lz, LZ4F_VERSION))) {
		errno = EINVAL;
		free(lzstrm);
		return NULL;
	}
	lzstrm->isize = isize > 65536 ? isize : 65536;
	*osize = LZ4F_compressBound(lzstrm->isize, &lz4f_prefs);
	lzstrm->ibuf = malloc(lzstrm->isize);
	if (lzstrm->ibuf == NULL) {
		free(lzstrm);
		return NULL;
	}	

	lzstrm->ipos = 0;
	lzstrm->hdl.lz4.iloc = 0;

	lzstrm->strmread = &lzread;
	lzstrm->strmreadbuf = &lzreadbuf;
	lzstrm->strmclose = &lzclose;
	lzstrm->strmerror = &lzerror;

	lzstrm->nextstrm = basestrm;

	return lzstrm;
}
#endif

#ifdef HAVE_SNAPPY
/* snappy wrapped socket */
static inline ssize_t
snappyreadbuf(z_strm *strm, void *buf, size_t sze);

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
	strm->closing = connection_ended(ret, errno);
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
snappyreadbuf(z_strm *strm, void *buf, size_t sze)
{
	/* not implemented */
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

static inline const char *
snappyerror(z_strm *strm, int rval)
{
	return strm->nextstrm->strmerror(strm->nextstrm, rval);
}

z_strm *
snappynew(size_t isize, z_strm *basestrm, size_t *osize)
{
	z_strm *sstrm = malloc(sizeof(z_strm));
	if (sstrm == NULL) {
		return NULL;
	}
	sstrm->ibuf = malloc(isize);
	if (sstrm->ibuf == NULL) {
		free(sstrm);
		return NULL;
	}
	sstrm->isize = isize;
	*osize = isize;		

	sstrm->ipos = 0;

	sstrm->strmread = &snappyread;
	sstrm->strmreadbuf = &snappyreadbuf;
	sstrm->strmclose = &snappyclose;
	sstrm->strmerror = &snappyerror;

	sstrm->nextstrm = basestrm;

	return sstrm;
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

static  __thread char _sslerror_buf[256];
static inline const char *
sslerror(z_strm *strm, int rval)
{
	int err = SSL_get_error(strm->hdl.ssl, rval);
	switch (err) {
		case SSL_ERROR_NONE:
			snprintf(_sslerror_buf, sizeof(_sslerror_buf),
					"%d: SSL_ERROR_NONE", err);
			break;
		case SSL_ERROR_ZERO_RETURN:
			snprintf(_sslerror_buf, sizeof(_sslerror_buf),
					"%d: TLS/SSL connection has been closed", err);
			break;
		case SSL_ERROR_WANT_READ:
		case SSL_ERROR_WANT_WRITE:
		case SSL_ERROR_WANT_CONNECT:
		case SSL_ERROR_WANT_ACCEPT:
			snprintf(_sslerror_buf, sizeof(_sslerror_buf),
					"%d: the read or write operation did not complete", err);
			break;
		case SSL_ERROR_WANT_X509_LOOKUP:
			snprintf(_sslerror_buf, sizeof(_sslerror_buf),
					"%d: call callback via SSL_CTX_set_client_cert_cb()", err);
			break;
#ifdef SSL_ERROR_WANT_ASYNC
		case SSL_ERROR_WANT_ASYNC:
			snprintf(_sslerror_buf, sizeof(_sslerror_buf),
					"%d: asynchronous engine is still processing data", err);
			break;
#endif
#ifdef SSL_ERROR_WANT_ASYNC_JOB
		case SSL_ERROR_WANT_ASYNC_JOB:
			snprintf(_sslerror_buf, sizeof(_sslerror_buf),
					"%d: no async jobs available in the pool", err);
			break;
#endif
		case SSL_ERROR_SYSCALL:
			snprintf(_sslerror_buf, sizeof(_sslerror_buf),
					"%d: I/O error: %s", err, strerror(errno));
			break;
		case SSL_ERROR_SSL:
			ERR_error_string_n(ERR_get_error(),
					_sslerror_buf, sizeof(_sslerror_buf));
			break;
	}
	return _sslerror_buf;
}
#endif

z_strm *
connection_strm_new(int sock, char *srcaddr, con_proto ctype, con_trnsp transport
			#ifdef HAVE_SSL
			, SSL_CTX *ctx
			#else
			, void *empthy
			#endif
			, char **obuf, size_t *osize
)
{
	int compress_type =	transport & 0xFFFF;
	z_strm *strm = socknew(METRIC_BUFSIZ, osize);
	if (strm == NULL) {
		logerr("cannot add new connection: "
				"out of memory allocating stream\n");		
		return NULL;
	}

	/* set socket or SSL connection */
	strm->nextstrm = NULL;
	strm->strmreadbuf = NULL;
	if ((transport & ~0xFFFF) != W_SSL) {
		if (ctype != CON_UDP) {
			strm->hdl.sock = sock;
			strm->strmread = &sockread;
			strm->strmclose = &sockclose;
		} else {
			strm->hdl.udp.sock = sock;
			strm->hdl.udp.srcaddr = srcaddr;
			strm->hdl.udp.srcaddrlen =
				sizeof(srcaddr);
			strm->strmread = &udpsockread;
			strm->strmclose = &udpsockclose;
		}
#ifdef HAVE_SSL
	} else {
		if ((strm->hdl.ssl = SSL_new(ctx)) == NULL) {
			free(strm);
			return NULL;
		};
		SSL_set_fd(strm->hdl.ssl, sock);
		SSL_set_accept_state(strm->hdl.ssl);

		strm->strmread = &sslread;
		strm->strmclose = &sslclose;
		strm->strmerror = &sslerror;
#endif
	}

	/* setup decompressor */
	if (compress_type == 0) {
		*obuf = NULL;
	}
#ifdef HAVE_GZIP
	else if (compress_type == W_GZIP) {
		z_strm *zstrm = gzipnew(METRIC_BUFSIZ, strm, osize);
		if (zstrm == NULL) {
			logerr("cannot add new connection: "
					"out of memory allocating gzip stream\n");
			free(strm);
			return NULL;
		}
		strm = zstrm;
	}
#endif
#ifdef HAVE_LZ4
	else if (compress_type == W_LZ4) {
		z_strm *lzstrm = lznew(METRIC_BUFSIZ, strm, osize);
		if (lzstrm == NULL) {
			if (errno == EINVAL) {
				logerr("Failed to create LZ4 decompression context\n");
			} else {
				logerr("cannot add new connection: "
					"out of memory allocating lz4 stream\n");
			}
			free(strm);
			return NULL;
		}
		strm = lzstrm;
	}
#endif
#ifdef HAVE_SNAPPY
	else if (compress_type == W_SNAPPY) {
		z_strm *sstrm = snappynew(METRIC_BUFSIZ, strm, osize);
		if (sstrm == NULL) {
			logerr("cannot add new connection: "
					"out of memory allocating snappy stream\n");
			free(strm);
			return NULL;
		}
		strm = sstrm;
	}
#endif
	if ((*obuf = malloc(*osize)) == NULL) {
		strm->strmclose(strm);
		logerr("cannot add new connection: "
					"out of memory allocating out buffer\n");
		return NULL;
	}
	return strm;
}

static void dispatch_releaseconnection(int sock);
static int dispatch_connection(connection *conn, dispatcher *self, struct timeval start);

/*
 * Shutdown command
 **/
static int
dispatch_shutdown(dispatcher *d)
{
	/* libevent initialized with pthreads locks */
	if (event_base_loopexit(d->evbase, NULL) == -1) {
			logerr("event_base_loopexit failed!\n");
		abort();
	}
	return 0;
}

static int __dispatch_addlistener(dispatcher *d, listener *lsnr);
static int __dispatch_removelistener(dispatcher *d, listener *lsnr);
static int __dispatch_transplantlistener(dispatcher *d, listener *olsnr, listener *nlsnr, router *r);

void dispatch_read_cb(int fd, short flags, void *arg)
{
	connection *conn = (connection *) arg;
	dispatcher *d  = conn->d;
	struct timeval start;

	gettimeofday(&start, NULL);
	__sync_add_and_fetch(&(d->sleeps), timediff(d->stop, start));
	if (flags & EV_TIMEOUT) {
		__sync_lock_test_and_set(&(conn->takenby), conn->d->id);
		tracef("dispatcher: timeout socket %d\n", conn->sock);
		dispatch_closeconnection(conn->d, conn, 0);
	} else {
		if (!__sync_bool_compare_and_swap(&(conn->takenby), C_IN, conn->d->id))
				return;
		if (__sync_bool_compare_and_swap(&(d->hold), 1, 1) && !conn->isaggr)
		{
			__sync_bool_compare_and_swap(
					&(conn->takenby), d->id, C_IN);
			return;
		}
		dispatch_connection(conn, conn->d, start);
	}

	gettimeofday(&d->stop, NULL);
	__sync_add_and_fetch(&(d->ticks), timediff(start, d->stop));
}

void dispatch_accept_cb(int fd, short flags, void *arg)
{
	ev_io_sock *lsock = (ev_io_sock *) arg;
	dispatcher *d = (dispatcher *) lsock->d;
    int client;
	struct sockaddr addr;
	socklen_t addrlen = sizeof(addr);
	struct timeval start;

	gettimeofday(&start, NULL);
	__sync_add_and_fetch(&(d->sleeps), timediff(d->stop, start));
	if ((client = accept(lsock->sock, &addr, &addrlen)) < 0)
	{
		logerr("dispatch: failed to "
				"accept() new connection on socket %d: %s\n",
				lsock->sock, strerror(errno));
		dispatch_check_rlimit_and_warn();
		__sync_add_and_fetch(&errorsconnections, 1);
		return;
	}
	if (!d->running) {
		close(client);
	} else {
		(void) fcntl(client, F_SETFL, O_NONBLOCK);
		dispatch_addconnection(client, lsock->lsnr, dispatch_worker_with_low_connections(), 0, 0);
	}
	gettimeofday(&d->stop, NULL);
	__sync_add_and_fetch(&(d->ticks), timediff(start, d->stop));
}

/**
 * Fake read command from socket callback
 */
static void
dispatch_cmd_cb(int fd, short flags, void *arg)
{
    int cmd;
	struct timeval tv;
	dispatcher *d = (dispatcher *) arg;
	if (read(fd, &cmd, sizeof(cmd)) != sizeof(cmd)) {
		logerr("dispatcher: incomplete cmd\n");
	}
	tv.tv_sec = 10;
	tv.tv_usec = 0;
	if (event_base_loopexit(d->evbase, &tv) == -1) {
		logerr("event_base_loopexit failed!\n");
		abort();
	}
}

static int
dispatch_reload(dispatcher *d)
{
	if (__sync_bool_compare_and_swap(&(d->route_refresh_pending), 1, 1)) {
		int c;
		ev_io_sock *socks;
		d->rtr = d->pending_rtr;
		d->pending_rtr = NULL;
		/* change listener on accept_cb arg */
		for (c = 0; c < MAX_LISTENERS; c++) {
			if (listeners[c] != NULL) {
				for (socks = listeners[c]->socks; socks->sock != -1; socks++) {
					event_free(socks->ev);
					socks->ev = event_new(d->evbase, socks->sock, EV_READ | EV_PERSIST,
										  dispatch_accept_cb, socks);
					event_add(socks->ev, NULL);
				}
			}
		}
		__sync_bool_compare_and_swap(&(d->route_refresh_pending), 1, 0);
		__sync_and_and_fetch(&(d->hold), 0);
	}
	return 0;
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
	return workers;
}

dispatcher *
dispatch_listener_worker(void)
{
	return workers[0];
}

dispatcher *
dispatch_worker_with_low_connections(void)
{
	dispatcher *d = workers[1];
	size_t min = __sync_fetch_and_add(&(d->connections), 0);
	size_t i;
	for (i = 2; i < 1 + workercnt; i++) {
		size_t c = __sync_fetch_and_add(&(workers[i]->connections), 0);
		if (c < min) {
			min = c;
			d = workers[i];
		}
	}
	/* increase connection counter, decrease on connection close or error */
	__sync_fetch_and_add(&(d->connections), 1);
	return d;
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
dispatch_connection_to_worker(dispatcher *d, connection *conn)
{
	if (conn->noexpire) {
		conn->ev = event_new(d->evbase, conn->sock, EV_READ | EV_PERSIST, dispatch_read_cb, conn);
	} else {
		conn->ev = event_new(d->evbase, conn->sock, EV_TIMEOUT | EV_READ | EV_PERSIST, dispatch_read_cb, conn);
	}
	if (conn->ev == NULL)
		return -1;
	conn->d = d;
	if (!conn->noexpire) {
		struct timeval tv;
		tv.tv_sec = IDLE_DISCONNECT_TIME;
		tv.tv_usec = 0;
		event_add(conn->ev, &tv);
	} else {
		event_add(conn->ev, NULL);
	}
	tracef("dispatcher %d: event read add for socket %d\n", d->id, conn->sock);
	return 0;
}

connection *
dispatch_get_connection(int c)
{
	return connections[c];
}

int
dispatch_addlistener(listener *lsnr)
{
	int res = __dispatch_addlistener(dispatch_listener_worker(), lsnr);
	if (res != 0) {
		logerr("dispatch: failed to notify addlistener\n");
		return 1;
	}
	return 0;
}

/**
 * Adds an (initial) listener socket to the chain of connections.
 * Listener sockets are those which need to be accept()-ed on.
 */
static int
__dispatch_addlistener(dispatcher *d, listener *lsnr)
{
	int c;
	connection *conn;
	ev_io_sock *socks;

	if (lsnr->ctype == CON_UDP) {
		/* Adds a pseudo-listener for datagram (UDP) sockets, which is
		 * pseudo, for in fact it adds a new connection, but makes sure
		 * that connection won't be closed after being idle, and won't
		 * count that connection as an incoming connection either. */
		for (socks = lsnr->socks; socks->sock != -1; socks++) {
			conn = dispatch_addconnection(socks->sock, lsnr, dispatch_worker_with_low_connections(), 0, 1);

			if (conn == NULL)
				return 1;

			socks->d = conn->d;
			socks->ev = conn->ev;
			socks->lsnr = lsnr;

			__sync_add_and_fetch(&acceptedconnections, -1);
		}

		return 0;
	}

	pthread_rwlock_wrlock(&listenerslock);
	for (c = 0; c < MAX_LISTENERS; c++) {
		if (listeners[c] == NULL) {
			listeners[c] = lsnr;
			for (socks = lsnr->socks; socks->sock != -1; socks++) {
				(void) fcntl(socks->sock, F_SETFL, O_NONBLOCK);
				socks->d = d;
				socks->ev = event_new(d->evbase, socks->sock, EV_READ | EV_PERSIST,
									  dispatch_accept_cb, socks);
				if (socks->ev == NULL) {
					logerr("cannot add new listener: "
							"event allocation error\n");
					pthread_rwlock_unlock(&listenerslock);
					return 1;
				}
				event_priority_set(socks->ev, 2);
				event_add(socks->ev, NULL);
				tracef("dispatcher %d: event accept add for socket %d\n", d->id, socks->sock);
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
	int res = __dispatch_removelistener(dispatch_listener_worker(), lsnr);
	if (res != 0) {
		logerr("dispatch: failed to notify removelistener\n");
		return 1;
	}
	return 0;
}

/**
 * Remove listener from the listeners list.  Each removal will incur a
 * global lock.  Frequent usage of this function is not anticipated.
 */
static int
__dispatch_removelistener(dispatcher *d, listener *lsnr)
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
		//ev_io_stop(loop, &socks->ev);
		if (lsnr->ctype == CON_UDP) {
			dispatch_closeconnection(d, connections[socks->sock], 0);
		} else {
			event_free(socks->ev);
			close(socks->sock);
		}
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
	int res = __dispatch_transplantlistener(dispatch_listener_worker(), olsnr, nlsnr, r);
	if (res != 0) {
		logerr("dispatch: failed to notify transplantlistener (%d)\n", errno);
		return 1;
	}
	return 0;

}

/**
 * Copy over all state related things from olsnr to nlsnr and ensure
 * olsnr can be discarded (that is, thrown away without calling
 * dispatch_removelistener).
 */
static int
__dispatch_transplantlistener(dispatcher *d, listener *olsnr, listener *nlsnr, router *r)
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

connection *connection_new() {
	connection *con = malloc(sizeof(connection));
	if (con) {
		con->takenby = C_SETUP;
	}
	return con;
}

/* only for tests */
void connection_buf_cat(connection *con, char *data) {
	size_t len = strlen(data);
	strcpy(con->buf + con->buflen, data);
	con->buflen += len;
}

/* only for tests */
const char *connection_buf(connection *con) {
	return con->buf;
}

/* only for tests */
const char *connection_metric(connection *con) {
	return con->metric;
}

/**
 * Adds a connection socket to the chain of connections.
 * Connection sockets are those which need to be read from.
 * Returns the connection id, or -1 if a failure occurred.
 */
connection *
dispatch_addconnection(int sock, listener *lsnr, dispatcher *d, char is_aggr, char no_expire)
{
	int c;
	connection *conn;
	con_proto ctype;
    con_trnsp transport;
	struct sockaddr_in6 saddr;
	socklen_t saddr_len = sizeof(saddr);
#ifdef HAVE_SSL
	SSL_CTX *ctx;
#else
	void *ctx;
#endif

	pthread_rwlock_rdlock(&connectionslock);
	if (sock < connectionslen) {
		if (connections[sock] == NULL) {
			connections[sock] = connection_new();
		} else {
			logerr("dispatcher %d: addconnection for existing socket %d\n", d->id, sock);
			/* abort(); */
			pthread_rwlock_unlock(&connectionslock);
			return NULL;
		}
	} else {
		connection **newlst;
		pthread_rwlock_unlock(&connectionslock);
		pthread_rwlock_wrlock(&connectionslock);
		if (connectionslen > sock) {
			/* another dispatcher just extended the list */
			pthread_rwlock_unlock(&connectionslock);
			return dispatch_addconnection(sock, lsnr, d, is_aggr, no_expire);
		}
		newlst = realloc(connections,
				sizeof(connection *) * (sock + CONNGROWSZ));
		if (newlst == NULL) {
			pthread_rwlock_unlock(&connectionslock);
			__sync_add_and_fetch(&d->connections, -1);
			close(sock);
			logerr("cannot add new connection: "
					"out of memory allocating more slots (max = %zu)\n",
					connectionslen);
			return NULL;
		}

		for (c = connectionslen; c < sock + CONNGROWSZ; c++) {
			newlst[c] = NULL;
		}
		connections = newlst;
		connectionslen = sock + CONNGROWSZ;
		connections[sock] = connection_new();
		if (connections[sock] == NULL) {
			pthread_rwlock_unlock(&connectionslock);
			__sync_add_and_fetch(&d->connections, -1);
			logerr("cannot allocate new connection: "
					"out of memory allocating more slots (max = %zu)\n",
					connectionslen);
			return NULL;
		}
	}
	conn = connections[sock];
	pthread_rwlock_unlock(&connectionslock);

	/* figure out who's calling */
	if (getpeername(sock, (struct sockaddr *)&saddr, &saddr_len) == 0) {
		snprintf(conn->srcaddr, sizeof(conn->srcaddr),
				"(unknown)");
		switch (saddr.sin6_family) {
			case PF_INET:
				inet_ntop(saddr.sin6_family,
						&((struct sockaddr_in *)&saddr)->sin_addr,
						conn->srcaddr, sizeof(conn->srcaddr));
				break;
			case PF_INET6:
				inet_ntop(saddr.sin6_family, &saddr.sin6_addr,
						conn->srcaddr, sizeof(conn->srcaddr));
				break;
		}
	}

	(void) fcntl(sock, F_SETFL, O_NONBLOCK);
	if (sockbufsize > 0) {
		if (setsockopt(sock, SOL_SOCKET, SO_RCVBUF,
				&sockbufsize, sizeof(sockbufsize)) != 0)
			;
	}
	conn->sock = sock;
	if (lsnr == NULL) {
		ctype = CON_TCP;
		transport = W_PLAIN;
		ctx = NULL;
	} else {
		ctype = lsnr->ctype;
		transport = lsnr->transport;
		#ifdef HAVE_SSL
			ctx = lsnr->ctx;
		#else
			ctx = NULL;
		#endif
	}

	if ((conn->strm = connection_strm_new(sock, conn->srcaddr, ctype, transport, ctx, &conn->buf, &conn->bufsize)) == NULL) {
		dispatch_releaseconnection(sock);
		__sync_sub_and_fetch(&d->connections, 1);
		return NULL;
	}

	conn->buflen = 0;
	conn->needmore = 0;
	if (no_expire == 1)
		conn->noexpire = 1;
	else
		conn->noexpire = 0;
	if (is_aggr == 1)
		conn->isaggr = 1;
	else
		conn->isaggr = 0;
	if (lsnr == NULL || lsnr->ctype != CON_UDP) {
		conn->isudp = 0;
	} else {
		conn->isudp = 1;
	}
	conn->destlen = 0;
	gettimeofday(&conn->lastwork, NULL);
	/* after this dispatchers will pick this connection up */
	__sync_bool_compare_and_swap(&(conn->takenby), C_SETUP, C_IN);
	conn->ev = NULL;
	if (dispatch_connection_to_worker(d, conn) == -1) {
		dispatch_closeconnection(d, conn, 0);
		__sync_add_and_fetch(&errorsconnections, 1);
		__sync_sub_and_fetch(&d->connections, 1);
		logerr("dispatch %d: failed to "
				"pass new connection on socket %d\n",
				d->id, sock);
		return NULL;
	}
	__sync_add_and_fetch(&acceptedconnections, 1);

	return conn;
}

/**
 * Adds a connection which we know is from an aggregator, so direct
 * pipe.  This is different from normal connections that we don't want
 * to count them, never expire them, and want to recognise them when
 * we're doing reloads.
 */
connection *
dispatch_addconnection_aggr(int sock)
{
	connection *conn = dispatch_addconnection(sock, NULL, dispatch_worker_with_low_connections(), 1, 1);

	if (conn == NULL)
		return NULL;

	__sync_add_and_fetch(&acceptedconnections, -1);

	return conn;
}

/* optimized rand for perfomance */
int rand2()
{
    static __thread unsigned int seed = 4541;

    seed = (8253729 * seed + 2396403);

    return seed  % RAND_MAX;
}

inline static char
dispatch_process_dests(connection *conn, dispatcher *self, struct timeval now)
{
	int i;
	char force;

	if (conn->destlen > 0) {
		if (conn->maxsenddelay == 0)
			conn->maxsenddelay = ((rand2() % 750) + 250) * 1000;
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
void
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
				tracef("dispatcher %d, connfd %d, discard metric %s\n",
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
						conn->metric, firstspace, self->id));
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
				"conn->buflen: %lu, sizeof(conn->buf): %lu, "
				"memmove(%d, %lu, %zu)\n",
				self->id,
				conn->buf, lastnl, lastnl - conn->buf,
				conn->buflen, conn->bufsize,
				0, (size_t) (lastnl + 1 - conn->buf), conn->buflen + 1);
		tracef("dispatcher %d, pre conn->buf: %s\n", self->id, conn->buf);
		/* copy last NULL-byte for debug tracing */
		memmove(conn->buf, lastnl + 1, conn->buflen + 1);
		tracef("dispatcher %d, post conn->buf: %s\n", self->id, conn->buf);
	}
}

static inline void dispatch_releaseconnection(int sock)
{
	connection *conn;
	pthread_rwlock_rdlock(&connectionslock);
	conn = connections[sock];
	connections[sock] = NULL;
	pthread_rwlock_unlock(&connectionslock);
	free(conn);
	close(sock);
}

void
dispatch_closeconnection(dispatcher *d, connection *conn, ssize_t len)
{
	tracef("dispatcher: %d, connfd: %d, len: %zd [%s], disconnecting\n",
		d->id, conn->sock, len,
		len < 0 ? strerror(errno) : "");
	__sync_add_and_fetch(&d->connections, -1);
	__sync_add_and_fetch(&closedconnections, 1);
	pthread_rwlock_rdlock(&connectionslock);
	connections[conn->sock] = NULL;
	pthread_rwlock_unlock(&connectionslock);
	if (conn->ev != NULL) {
		event_free(conn->ev);
	}
	conn->strm->strmclose(conn->strm);
	free(conn->buf);
	free(conn);
	__sync_add_and_fetch(&closedconnections, 1);
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
						(conn->bufsize - 1) - conn->buflen)) > 0
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

		dispatch_received_metrics(conn, self);
		if (conn->strm->strmreadbuf != NULL) {
			while((ilen = conn->strm->strmreadbuf(conn->strm,
							conn->buf + conn->buflen,
							(conn->bufsize - 1) - conn->buflen)) > 0) {
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
			dispatch_closeconnection(self, conn, len);
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
	d->running = 1;
	if (event_base_dispatch(d->evbase) < 0) {
		logerr("dispatcher %d: event_base_dispatch() failed\n", d->id);
	}
	d->running = 0;
	return NULL;
}

/**
 * Starts a new dispatcher for the given type and with the given id.
 * Returns its handle.
 */
dispatcher *
dispatch_new(
		unsigned char id,
		enum conntype type
	)
{
	dispatcher *ret = malloc(sizeof(dispatcher));

	if (ret == NULL)
		return NULL;

	if ((ret->evbase = event_base_new()) == NULL) {
		free(ret);
		return NULL;
	}

	if (eventpipe_init(&ret->notify_fd) == -1) {
		event_base_free(ret->evbase);
		free(ret);
		return NULL;
	}

	ret->notify_ev = event_new(ret->evbase, eventpipe_rd(&ret->notify_fd),
		          EV_READ | EV_PERSIST, dispatch_cmd_cb, (void*) ret);
	if (ret->notify_ev == NULL) {
		eventpipe_close(&ret->notify_fd);
		event_base_free(ret->evbase);
		free(ret);
		return NULL;
	}
	event_priority_set(ret->notify_ev, 0); /* priority event */
	event_add(ret->notify_ev, NULL);

	ret->running = 0;

	ret->type = type;

	ret->id = id;
	ret->keep_running = 1;
	ret->route_refresh_pending = 0;
	ret->hold = 0;

	ret->connections = 0;

	ret->metrics = 0;
	ret->blackholes = 0;
	ret->discards = 0;
	ret->ticks = 0;
	ret->sleeps = 0;
	ret->prevmetrics = 0;
	ret->prevblackholes = 0;
	ret->prevticks = 0;
	ret->prevsleeps = 0;

	gettimeofday(&ret->stop, NULL);

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
		for (id = 1; id < 1 + count; id++) {
			if ((workers[id] = dispatch_new(id, CONNECTION)) == NULL) {
				logerr("failed to add connection dispatcher\n");
				return -1;
			}
		}
		workers[id] = NULL; /* sentinel */
		workercnt = count;
	}
	return workercnt;
}

void
dispatch_init(
		dispatcher *d,
		router *r,
		char *allowed_chars,
		int maxinplen,
		int maxmetriclen
	)
{
	d->rtr = r;
	d->allowed_chars = allowed_chars;
	d->tags_supported = 0;
	d->maxinplen = maxinplen;
	d->maxmetriclen = maxmetriclen;

	/* switch tag support on when the user didn't allow ';' as valid
	 * character in metrics */
	if (allowed_chars != NULL && strchr(allowed_chars, ';') == NULL)
		d->tags_supported = 1;
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
	
	dispatch_init(d, r, allowed_chars, maxinplen, maxmetriclen);

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
	for (id = 0; id < 1 + workercnt; id++) {
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
	dispatch_shutdown(d);
}

void
dispatchs_stop(void)
{
	int id;
	for (id = 0; id < 1 + workercnt; id++)
		dispatch_stop(workers[id]);
}

/**
 * Shuts down dispatcher d.  Returns when the dispatcher has terminated.
 */
void
dispatch_wait_shutdown(dispatcher *d)
{
	pthread_join(d->tid, NULL);
}

void
dispatch_wait_shutdown_byid(unsigned char id)
{
	pthread_join(workers[id]->tid, NULL);
}

/**
 * Free up resources taken by dispatcher d.  The caller should make sure
 * the dispatcher has been shut down at this point.
 */
void
dispatch_free(dispatcher *d)
{
	event_free(d->notify_ev);
	event_base_free(d->evbase);
	eventpipe_close(&d->notify_fd);
	free(d);
}

void
dispatchs_free()
{
	int i;
	for (i = 0; i < connectionslen; i++) {
		if (connections[i] != NULL)
			dispatch_closeconnection(dispatch_listener_worker(), connections[i], 0);
	}
	for (i = 0; i < 1 + workercnt; i++)
		dispatch_free(workers[i]);
	for (i = 0; i < MAX_LISTENERS; i++) {
		if (listeners[i] != NULL)
			free(listeners[i]);
	}
	free(listeners);
	free(workers);
	free(connections);
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
	dispatch_reload(d);
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
