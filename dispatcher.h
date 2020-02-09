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


#ifndef DISPATCHER_H
#define DISPATCHER_H 1

#include <stdlib.h>

#include "router.h"

typedef struct _dispatcher dispatcher;
typedef struct _connection connection;

char dispatch_global_alloc(void);
void dispatch_check_rlimit_and_warn(void);
char dispatch_workers_alloc(char count);
dispatcher **dispatch_workers(void);
dispatcher *dispatch_listener_worker(void);
dispatcher *dispatch_worker_with_low_connections(void);
char dispatch_workercnt(void);
connection *dispatch_get_connection(int c);
int dispatch_addlistener(listener *lsnr);
int dispatch_removelistener(listener *lsnr);
int dispatch_transplantlistener(listener *olsnr, listener *nlsnr, router *r);
connection *dispatch_addconnection(int sock, listener *lsnr, dispatcher *d);
connection *dispatch_addconnection_aggr(int sock);
void dispatch_set_bufsize(unsigned int sockbufsize);
char dispatch_init_listeners(void);
int dispatch_start_listener(unsigned char id, router *r,
		char *allowed_chars, int maxinplen, int maxmetriclen);
int dispatch_start_connection(unsigned char id, router *r,
		char *allowed_chars, int maxinplen, int maxmetriclen);
unsigned char dispatch_start_connections(router *r, char *allowed_chars,
		int maxinplen, int maxmetriclen);
void dispatch_stop(dispatcher *d);
void dispatchs_stop(void);
void dispatch_wait_shutdown(dispatcher *d);
void dispatch_wait_shutdown_byid(unsigned char id);
void dispatch_free(dispatcher *d);
void dispatchs_free();
size_t dispatch_get_ticks(dispatcher *self);
size_t dispatch_get_metrics(dispatcher *self);
size_t dispatch_get_blackholes(dispatcher *self);
size_t dispatch_get_discards(dispatcher *self);
size_t dispatch_get_sleeps(dispatcher *self);
size_t dispatch_get_ticks_sub(dispatcher *self);
size_t dispatch_get_metrics_sub(dispatcher *self);
size_t dispatch_get_blackholes_sub(dispatcher *self);
size_t dispatch_get_discards_sub(dispatcher *self);
size_t dispatch_get_sleeps_sub(dispatcher *self);
size_t dispatch_get_accepted_connections(void);
size_t dispatch_get_closed_connections(void);
void dispatch_hold(dispatcher *d);
void dispatchs_hold();
void dispatch_schedulereload(dispatcher *d, router *r);
void dispatchs_schedulereload(router *r);
char dispatch_reloadcomplete(dispatcher *d);
void dispatch_wait_reloadcomplete();


#endif
