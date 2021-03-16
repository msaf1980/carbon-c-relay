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


#ifndef SERVER_H
#define SERVER_H 1

#include <netdb.h>

#include "queue.h"
#include "relay.h"

#define SERVER_MAX_CONNECTIONS 32

#define SERVER_STALL_BITS  4  /* 0 up to 15 */

#define SERVER_MAX_SEND    10 /* max try write to socket for complete */

#define SERVER_KEEP_RUNNING 1
#define SERVER_TRY_SEND 0 /* send all items with timeout from queue before shutdown */
#define SERVER_TRY_SWAP -1 /* don't send, if possible move items with swap shared queue */

extern int queuefree_threshold_start;
extern int queuefree_threshold_end;
extern int shutdown_timeout;

typedef struct _server server;

server *server_new(
		const char *ip,
		unsigned short port,
		con_type type,
		con_trnsp transport,
		con_proto ctype,
		unsigned short nconns,
		struct addrinfo *saddr,
		struct addrinfo *hint,
		size_t queuesize,
		queue *queue,
		size_t batchsize,
		int maxstalls,
		unsigned short iotimeout,
		unsigned int sockbufsize,
		int threshold_start,
		int threshold_end);
char server_cmp(server *s, struct addrinfo *saddr, const char *ip, unsigned short port, con_proto proto);
char server_start(server *s);
void server_add_secondaries(server *d, server **sec, size_t cnt);
void server_set_failover(server *d);
void server_set_weigtht(server *self, unsigned short weigtht);
void server_set_instance(server *d, char *inst);
char server_send(server *s, const char *d, char force);
void server_shutdown(server *s, int swap);
void server_shutdown_wait(server *s);
void server_free(server *s);
void server_swap_queue(server *l, server *r);
const char *server_ip(server *s);
unsigned short server_port(server *s);
char *server_instance(server *s);
unsigned short server_get_connections(server *s);
con_proto server_ctype(server *s);
con_type server_type(server *s);
con_trnsp server_transport(server *s);
char server_failed(server *s);
size_t server_get_ticks(server *s, unsigned short n);
size_t server_get_waits(server *s, unsigned short n);
size_t server_get_metrics(server *s, unsigned short n);
size_t server_get_stalls(server *s);
size_t server_get_dropped(server *s);
size_t server_get_requeue(server *s);
size_t server_get_ticks_sub(server *s, unsigned short n);
size_t server_get_waits_sub(server *s, unsigned short n);
size_t server_get_metrics_sub(server *s, unsigned short n);
size_t server_get_stalls_sub(server *s);
size_t server_get_dropped_sub(server *s);
size_t server_get_requeue_sub(server *s);
size_t server_get_queue_len(server *s);
size_t server_get_queue_size(server *s);
char server_queue_is_shared(server *s);
queue *server_set_shared_queue(server *s, queue *q);

#endif
