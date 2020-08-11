#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>

#include "queue.h"

#include "receiver_mock.h"

#define METRIC_BUFSIZ 32768

struct _connection {
	int sock;
	char buf[METRIC_BUFSIZ + 1];
	size_t buflen;
	size_t bufsize;
};

typedef struct _connection connection;

struct _receiver {
	char noread;
	char reset;
	char keep_running;
	queue *queue;
	size_t received;
	pthread_t tid;
	char *addr;
	unsigned short port;
	int lsnr_sock;
	size_t connsize;
	connection **conns;
};

ssize_t connection_read(connection *c)
{
	if (c->bufsize <= c->buflen) {
		errno = EMSGSIZE;
		return -1;
	}
	ssize_t ret = read(c->sock, c->buf + c->buflen, c->bufsize - c->buflen);
	if (ret > 0) {
		c->buflen += ret;
	}
	return ret;
}

receiver *
receiver_new(char *addr, unsigned short port, size_t qsize, char noread, char reset)
{
	size_t i;

	receiver *ret = malloc(sizeof(receiver));
	if (ret == NULL)
		return NULL;
	ret->queue = queue_new(qsize);
	if (ret->queue == NULL) {
		free(ret);
		return NULL;
	}
	ret->lsnr_sock = -1;
	ret->connsize = 1000;
	ret->conns = malloc(sizeof(connection*) * ret->connsize);
	if (ret->conns == NULL) {
		queue_free(ret->queue);
		free(ret);
		return NULL;
	}
	for (i = 0; i < ret->connsize; i++) {
		ret->conns[i] = NULL;
	}
	ret->addr = addr;
	ret->port = port;
	ret->noread = noread;
	ret->reset = reset;
	ret->tid = 0;
	ret->received = 0;
	ret->keep_running = 1;

	return ret;
}

unsigned short receiver_get_port(receiver *r)
{
	return r->port;
}

size_t receiver_connection(receiver *r, connection *c)
{
	ssize_t ret = 0;
	if (__sync_add_and_fetch(&(r->reset), 0) == 0) {
		ret = connection_read(c);
	}
	if (ret == 0 ||
		(ret == -1 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
		fprintf(stderr, "connection closed: %d with error %s\n", c->sock, strerror(errno));
		close(c->sock);
		r->conns[c->sock] = NULL;
		free(c);
		return 0;
	}
	return ret;
}

size_t receiver_process(receiver *r, connection *c)
{
	static const char delim = '\n';
	size_t processed = 0;
	char *last = c->buf;
	c->buf[c->buflen] = '\0';
	while (1) {
		char *p = strchr (last, delim);
		if (p == NULL) /*incomplete metric */
			break;
		queue_putback(r->queue, strndup(last, p - last));
		processed += (p - last);
		last = p + 1;
	}
	if (processed > 0) {
		memmove(c->buf, c->buf + processed, c->buflen - processed);
		c->buflen -= processed;

	}
	return processed;
}

static void *
receiver_dispatch(void *d)
{
	receiver *r = (receiver *) d;

	/*
	struct sockaddr addr;
	socklen_t addrlen = sizeof(addr);
	*/
	struct pollfd ufds[MAX_CLIENTS];
	size_t nfds = 1;
	ssize_t i;

	ufds[0].fd = r->lsnr_sock;
	ufds[0].events = POLLIN;
	(void) fcntl(r->lsnr_sock, F_SETFL, O_NONBLOCK);

	while(__sync_add_and_fetch(&(r->keep_running), 0)) {
		if (poll(ufds, nfds, 1000) > 0) {
			char closed = 0;
			size_t len = nfds;
			i = len;
			for (i = 1; i < len; i++) {
				if (ufds[i].revents & POLLIN) {
					/*read */
					int sock = ufds[i].fd;
					connection *c;
					size_t ret;
					if (__sync_add_and_fetch(&(r->noread), 0)) {
						continue;
					}
					c = r->conns[sock];
					ret = receiver_connection(r, c);
					if (ret == 0) {
						ufds[i].fd = -1;
						closed = 1;
					} else if (ret > 0) {
						receiver_process(r, c);
					}
				} else if (ufds[i].revents & POLLHUP || ufds[i].revents & POLLNVAL || ufds[i].revents & POLLERR) {
					/* closing connection */
					int error = 0;
					socklen_t errlen = sizeof(error);
					getsockopt(ufds[i].fd, SOL_SOCKET, SO_ERROR, (void *)&error, &errlen);
					if (ufds[i].revents & POLLNVAL) {
						fprintf(stderr, "connection closed: %d with poll event %x (error %s)\n", ufds[i].fd, ufds[i].revents, strerror(error));
					}
					connection *c = r->conns[ufds[i].fd];
					close(ufds[i].fd);
					r->conns[ufds[i].fd] = NULL;
					free(c);
					ufds[i].fd = -1;
					closed = 1;
				}
				ufds[i].revents = 0;
			}

			/* compact pollfd[] */
			if (closed) {
				volatile size_t j;
				size_t pos = 0;
				struct pollfd ufds_old[MAX_CLIENTS];
				memcpy(ufds_old, ufds, sizeof(ufds));
				for (j = 0; j < nfds; j++) {
					if (ufds_old[j].fd >= 0) {
						ufds[pos].fd = ufds_old[j].fd;
						ufds[pos].events = POLLIN;
						pos++;
					}
				}
				nfds = pos;
			}

			/* incomming connection */
			if (ufds[0].revents & POLLIN) {
				int client = accept(ufds[0].fd, NULL, NULL);
				if (client < 0) {
					break;
				}
				if (__sync_add_and_fetch(&(r->reset), 0) == 0) {
					close(client);
				}
				if (client >= r->connsize) {
					size_t j;
					size_t newconnsize = r->connsize + 1024;
					connection **newconns = realloc(r->conns, sizeof(connection*) * newconnsize);
					if (newconns == NULL) {
						fprintf(stderr, "connect alloc error on %s:%u\n", r->addr, r->port);
						close(client);
						continue;
					}
					r->conns = newconns;
					for (j = r->connsize; j < newconnsize; j++) {
						r->conns[j] = NULL;
					}
					r->connsize = newconnsize;
				}
				r->conns[client] = malloc(sizeof(connection));
				if (r->conns[client] == NULL) {
					fprintf(stderr, "connect alloc error on %s:%u\n", r->addr, r->port);
					close(client);
				} else {
					(void) fcntl(client, F_SETFL, O_NONBLOCK);
					r->conns[client]->buflen = 0;
					r->conns[client]->bufsize = sizeof(r->conns[client]->buf);
					r->conns[client]->sock = client;
					ufds[nfds].fd = client;
					ufds[nfds].events = POLLIN;
					nfds++;
					fprintf(stderr, "connection add: %d\n", ufds[i].fd);
				}
			}
		}
	}
	for (i = 0; i < nfds; i++) {
		if (ufds[i].fd >= 0) {
			close(ufds[i].fd);
		}
	}
	return NULL;
}

int receiver_start(receiver *r, char bind_only)
{
	struct sockaddr_in addr;
	socklen_t addr_len = sizeof(addr);
	bzero(&addr, addr_len);
	addr.sin_family = AF_INET;
	addr.sin_port = htons(r->port);
	if (r->addr == NULL) {
		addr.sin_addr.s_addr = htonl(INADDR_ANY);
	} else {
		inet_aton(r->addr, &addr.sin_addr);
	}
	r->lsnr_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (bind(r->lsnr_sock, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
		close(r->lsnr_sock);
		return -1;
	}
	if (r->port == 0) {
		getsockname(r->lsnr_sock, (struct sockaddr*)&addr, &addr_len);
		r->port = ntohs(addr.sin_port);
	}
	if (bind_only) {
		return 0;
	}
	if (listen(r->lsnr_sock, 20) == -1) {
		close(r->lsnr_sock);
		return -1;
	}
	/* printf("listen on %s:%u\n", r->addr, r->port); */

	return pthread_create(&r->tid, NULL, &receiver_dispatch, r);
}

void receiver_stop(receiver *r)
{
	__sync_bool_compare_and_swap(&(r->keep_running), 1, 0);
}

void receiver_free(receiver *r)
{
	size_t i;
	if (r->tid > 0) {
		pthread_join(r->tid, NULL);
	}
	for (i = 0; i < r->connsize; i++) {
		free(r->conns[i]);
	}
	free(r->conns);
	queue_destroy(r->queue);
	free(r);
}
