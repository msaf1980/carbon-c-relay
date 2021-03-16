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


#ifndef CLUSTER_H
#define CLUSTER_H 1

#include "pthread.h"

#include "consistent-hash.h"

#define CLUSTER_MAX_THREADS 16

enum clusttype {
	BLACKHOLE,  /* /dev/null-like destination */
	GROUP,      /* pseudo type to create a matching tree */
	AGGRSTUB,   /* pseudo type to have stub matches for aggregation returns */
	STATSTUB,   /* pseudo type to have stub matches for collector returns */
	VALIDATION, /* pseudo type to perform additional data validation */
	FORWARD,
	FILELOG,    /* like forward, write metric to file */
	FILELOGIP,  /* like forward, write ip metric to file */
	CARBON_CH,  /* original carbon-relay.py consistent-hash */
	FNV1A_CH,   /* FNV1a-based consistent-hash */
	JUMP_CH,    /* jump consistent hash with fnv1a input */
	ANYOF,      /* FNV1a-based hash, but with backup by others */
	FAILOVER,   /* ordered attempt delivery list */
	LB,         /* delivery list */
	AGGREGATION,
	REWRITE
};

typedef struct _servers {
	server *server;
	int refcnt;
	struct _servers *next;
} servers;

typedef struct {
	unsigned char repl_factor;
	ch_ring *ring;
	servers *servers;
} chashring;

typedef struct {
	unsigned short count;
	server **servers;
	servers *list;
} serverlist;

typedef struct _cluster {
	char *name;
	enum clusttype type;
	char running;       /* full byte for atomic access */
	char keep_running;  /* full byte for atomic access */
	int server_connections;
	int threshold_start;
	int threshold_end;
	size_t threads;
	pthread_t tids[CLUSTER_MAX_THREADS];
	size_t ttl; /* ttl seconds */
	size_t connect_ts; /* connect timestamp (seconds) for ttl */
	char poll_hold;          /* full byte for atomic access */
	char isdynamic:1;
	queue *queue;
	union {
		chashring *ch;
		servers *forward;
		serverlist *anyof;
		aggregator *aggregation;
		struct _route *routes;
		char *replacement;
		struct _validate *validation;
	} members;
	struct _cluster *next;
} cluster;


typedef struct _validate {
	struct _route *rule;
	enum { VAL_LOG, VAL_DROP } action;
} validate;

typedef struct _destinations {
	cluster *cl;
	struct _destinations *next;
} destinations;

typedef struct _route {
	char *pattern;    /* original regex input, used for printing only */
	regex_t *rule;    /* regex per worker on metric, only if type == REGEX */
	size_t nmatch;    /* number of match groups */
	char *strmatch;   /* string to search for if type not REGEX or MATCHALL */
	destinations *dests; /* where matches should go */
	char *masq;       /* when set, what to feed to the hashfunc when routing */
	char stop:1;      /* whether to continue matching rules after this one */
	enum {
		MATCHALL,     /* the '*', don't do anything, just match everything */
		REGEX,        /* a regex match */
		CONTAINS,     /* find string occurrence */
		STARTS_WITH,  /* metric must start with string */
		ENDS_WITH,    /* metric must end with string */
		MATCHES       /* metric matches string exactly */
	} matchtype;      /* how to interpret the pattern */
	struct _route *next;
} route;

size_t cluster_ttl(int ttl_minutes);
cluster *cluster_new(char *name, allocator *a, enum clusttype ctype, route *m, size_t queuesize, size_t ttl_minutes, int threads);
int cluster_set_threshold(cluster *cl, int threshold_start, int threshold_end);
void cluster_free(cluster *cl);
char cluster_start(cluster *c);
void cluster_shutdown(cluster *c, int swap);
void cluster_shutdown_wait(cluster *c);
cluster *router_cluster(const router *rtr, const char *clname);
servers *cluster_servers(cluster *c);

#endif
