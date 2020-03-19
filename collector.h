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


#ifndef COLLECTOR_H
#define COLLECTOR_H 1

#include <time.h>

#include "dispatcher.h"
#include "router.h"
#include "aggregator.h"
#include "server.h"
#include "relay.h"


#if __linux
typedef struct timespec clocktime_t;
#define clocktime(X) do { clock_gettime(CLOCK_MONOTONIC, X); } while (0)
#define timediff(X, Y) \
	(Y.tv_sec > X.tv_sec ? (Y.tv_sec - X.tv_sec) * 1000 * 1000 + ((Y.tv_nsec - X.tv_nsec) / 1000) : (Y.tv_nsec - X.tv_nsec) / 1000)
#else
typedef struct timeval clocktime_t;
#define clocktime(X) do { gettimeofday(X, NULL); } while (0)
#define timediff(X, Y) \
	(Y.tv_sec > X.tv_sec ? (Y.tv_sec - X.tv_sec) * 1000 * 1000 + ((Y.tv_usec - X.tv_usec)) : Y.tv_usec - X.tv_usec)
#endif

void collector_start(dispatcher **d, router *rtr, server *submission, char cum);
void collector_stop(void);
void collector_schedulereload(router *rtr);
char collector_reloadcomplete(void);

#endif
