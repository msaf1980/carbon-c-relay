/*
 * Copyright 2013-2017 Fabian Groffen
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

/* This is a clumpsy program to test the distribution of a certain input
 * set of metrics.  See also:
 * https://github.com/graphite-project/carbon/issues/485
 *
 * compile using something like this:
 * clang -o distributiontest -I. issues/distributiontest.c consistent-hash.c \
 * server.c queue.c md5.c dispatcher.c router.c aggregator.c -pthread -lm */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <errno.h>
#include <libgen.h>
#include <signal.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>


#include "queue.h"
#include "relay.h"
#include "consistent-hash.h"
#include "router.h"
#include "server.h"
#include "md5.h"
#include "conffile.h"

#include "receiver_mock.h"

int
relaylog(enum logdst dest, const char *fmt, ...)
{
	(void) dest;
	(void) fmt;
	return 0;
}

/*
int
relaylog(enum logdst dest, const char *fmt, ...)
{
	va_list ap;
	int ret;

	va_start(ap, fmt);

	ret = vfprintf(stdout, fmt, ap);
	fflush(stdout);

	return ret;
}
*/

router *router_new(void);
char *router_validate_address(router *rtr, char **retip, unsigned short *retport, void **retsaddr, void **rethint, char *ip, con_proto proto);
void server_set_port(server *s, unsigned short port);

#include "minunit.h"

#define DESTS_SIZE 32
#define BIND_ADDR "127.0.0.1"

char testdir[256];

const char *m = "bla.bla.bla\n";

unsigned char mode = 0;
char relay_hostname[256];
#ifdef HAVE_SSL
char *sslCA = NULL;
char sslCAisdir = 0;
#endif

struct _reader {
	int sock;
	queue *queue;
} reader;

typedef struct {
	char *name;
	size_t queuesize;
	size_t batchsize;
	router *rtr;
	receiver **receivers;
} router_param;

void router_param_set(router_param *p, const char *name, size_t queuesize, size_t batchsize)
{
	p->name = strdup(name);
	p->queuesize = queuesize;
	p->batchsize = batchsize;
	p->receivers = NULL;
	p->rtr = NULL;
}

/* cleanup after test */
void router_param_cleanup(router *rtr, router_param *param, char freename)
{
	receiver **r = param->receivers;
	if (r != NULL) {
		while (*r != NULL) {
			receiver_stop(*r);
			receiver_free(*r);
			r++;
		}
		free(param->receivers);
		param->receivers = NULL;
	}

	if (rtr != NULL) {
		listener *lsnr = router_get_listeners(rtr);
		while (lsnr != NULL) {
			if (lsnr->saddrs != NULL) {
				freeaddrinfo(lsnr->saddrs);
			}
			lsnr = lsnr->next;
		}
		router_free(rtr);
	}

	if (freename) {
		free(param->name);
	}
}

void run_server_send(router *rtr, router_param *param)
{
	size_t destlen = 0, len = 0, blackholed = 0;
	size_t send_metrics = 2 * param->queuesize - 4 * param->batchsize;
	int i, j;
	destination dests[DESTS_SIZE];
	char *metric;
	char *firstspace;
	cluster *cl;
	size_t queued = 0;
	size_t sended = 0;
	size_t dropped = 0;

	cl = router_cluster(rtr, "test");
	if (cl == NULL) {
		mu_fail_step("cluster test not found", param->name);
		return;
	}

	for (i = 0; i < send_metrics; i++) {
		metric = strdup(m);
		firstspace = metric + strlen(metric);
		if (router_route(rtr, dests, &len, DESTS_SIZE, "127.0.0.1", metric, firstspace, 1) == 0) {
			for (j = 0; j < len; j++) {
				server_send(dests[j].dest, dests[j].metric, 0);
			}
			destlen += len;
		} else {
			blackholed++;
		}
		free(metric);
	}
	mu_assert_step(blackholed == 0, "router_route blackholed", param->name);

	for (i = 0; i < cl->members.anyof->count; i++) {
		queued += server_get_queue_len(cl->members.anyof->servers[i]);
		sended += server_get_metrics(cl->members.anyof->servers[i]);
		dropped += server_get_dropped(cl->members.anyof->servers[i]);
	}
	mu_assert_int_eq_step(send_metrics, sended + queued, param->name);
	mu_assert_step(sended == 0, "server_send send to blackhole", param->name);
	mu_assert_step(dropped == 0, "server_send drop", param->name);
}

MU_TEST_STEP(test_server_send, router_param*)
{
	router *rtr;
	char config[280];

	queuefree_threshold_start = 1;
	queuefree_threshold_end = 3;

	strcpy(relay_hostname, "relay");

	snprintf(config, sizeof(config), "%s/%s.conf", testdir, param->name);
	rtr = router_readconfig(NULL, config, 1, param->queuesize,
					param->batchsize, 1, 600, 0, 2003);
	if (rtr == NULL) {
		mu_fail_step("router_readconfig failed", param->name);
		return;
	}

	run_server_send(rtr, param);

	router_param_cleanup(rtr, param, 0);
}

void run_server_shutdown_timeout(router *rtr, router_param *param)
{
	size_t destlen = 0, len = 0, blackholed = 0;
	size_t send_metrics = param->queuesize - 3 * param->batchsize;
	int i, j;
	destination dests[DESTS_SIZE];
	char *metric;
	char *firstspace;
	cluster *cl;
	time_t start, stop;
	unsigned long long elapsed;
	size_t sended = 0;
	size_t queued = 0;
	size_t dropped = 0;

	cl = router_cluster(rtr, "test");
	if (cl == NULL) {
		mu_fail_step("cluster test not found", param->name);
		return;
	}
	cl = router_cluster(rtr, "test");
	if (cl == NULL) {
		mu_fail_step("cluster test not found", param->name);
		return;
	}

	param->receivers = malloc(sizeof(receiver*) * (cl->members.anyof->count + 1));
	for (i = 0; i < cl->members.anyof->count; i++) {
		param->receivers[i] = receiver_new(BIND_ADDR, 0, send_metrics, 1, 0);
		receiver_start(param->receivers[i], 1);
		server_set_port(cl->members.anyof->servers[i], receiver_get_port(param->receivers[i]));
	}
	param->receivers[i] = NULL;

	for (i = 0; i < cl->members.anyof->count; i++) {
		server_start(cl->members.anyof->servers[i]);
	}
	for (i = 0; i < send_metrics; i++) {
		metric = strdup(m);
		firstspace = metric + strlen(metric);
		if (router_route(rtr, dests, &len, DESTS_SIZE, "127.0.0.1", metric, firstspace, 1) == 0) {
			for (j = 0; j < len; j++) {
				server_send(dests[j].dest, dests[j].metric, 0);
			}
			destlen += len;
		} else {
			blackholed++;
		}
		free(metric);
	}
	start = time(NULL);
	router_shutdown(rtr);
	stop = time(NULL);
	elapsed = stop - start;
	mu_assert_step(elapsed < shutdown_timeout + 20, "router_shutdown timeout", param->name);
	mu_assert_step(blackholed == 0, "router_route blackholed", param->name);

	mu_assert_int_eq_step(0, dropped, param->name);

	for (i = 0; i < cl->members.anyof->count; i++) {
		queued += server_get_queue_len(cl->members.anyof->servers[i]);
		sended += server_get_metrics(cl->members.anyof->servers[i]);
		dropped += server_get_dropped(cl->members.anyof->servers[i]);
	}
	mu_assert_int_eq_step(send_metrics, sended + queued, param->name);
	mu_assert_step(sended == 0, "server_send send to blackhole", param->name);
	mu_assert_step(dropped == 0, "server_send drop", param->name);
}

MU_TEST_STEP(test_server_shutdown_timeout, router_param*)
{
	router *rtr;
	char config[280];

	queuefree_threshold_start = 1;
	queuefree_threshold_end = 3;
	send_timeout = 1;
	shutdown_timeout = 10;

	strcpy(relay_hostname, "relay");

	snprintf(config, sizeof(config), "%s/%s.conf", testdir, param->name);
	rtr = router_readconfig(NULL, config, 1, param->queuesize,
					param->batchsize, 1, 600, 0, 2003);
	if (rtr == NULL) {
		mu_fail_step("router_readconfig failed", param->name);
		return;
	}

	run_server_shutdown_timeout(rtr, param);

	router_param_cleanup(rtr, param, 0);
}

void run_server_queuereader(router *rtr, router_param *param)
{
	size_t destlen = 0, len = 0, blackholed = 0;
	size_t send_metrics = param->queuesize - 3 * param->batchsize;
	int i, j;
	destination dests[DESTS_SIZE];
	char *metric;
	char *firstspace;
	cluster *cl;
	time_t start, stop;
	unsigned long long elapsed;
	size_t sended = 0;
	size_t queued = 0;
	size_t dropped = 0;

	cl = router_cluster(rtr, "test");
	if (cl == NULL) {
		mu_fail_step("cluster test not found", param->name);
		return;
	}
	cl = router_cluster(rtr, "test");
	if (cl == NULL) {
		mu_fail_step("cluster test not found", param->name);
		return;
	}

	param->receivers = malloc(sizeof(receiver*) * (cl->members.anyof->count + 1));
	for (i = 0; i < cl->members.anyof->count; i++) {
		char noread = (i == 0);
		param->receivers[i] = receiver_new(BIND_ADDR, 0, send_metrics, noread, 0);
		receiver_start(param->receivers[i], 0);
		server_set_port(cl->members.anyof->servers[i], receiver_get_port(param->receivers[i]));
	}
	param->receivers[i] = NULL;

	for (i = 0; i < cl->members.anyof->count; i++) {
		server_start(cl->members.anyof->servers[i]);
	}
	for (i = 0; i < send_metrics; i++) {
		metric = strdup(m);
		firstspace = metric + strlen(metric);
		if (router_route(rtr, dests, &len, DESTS_SIZE, "127.0.0.1", metric, firstspace, 1) == 0) {
			for (j = 0; j < len; j++) {
				server_send(dests[j].dest, dests[j].metric, 0);
			}
			destlen += len;
		} else {
			blackholed++;
		}
		free(metric);
	}
	sleep(1);
	for (i = 0; i < cl->members.anyof->count; i++) {
		queued += server_get_queue_len(cl->members.anyof->servers[i]);
		sended += server_get_metrics(cl->members.anyof->servers[i]);
		dropped += server_get_dropped(cl->members.anyof->servers[i]);
	}
	mu_assert_int_eq_step(send_metrics, sended + queued, param->name);
	mu_assert_step(queued == 0, "server_send queued", param->name);
	mu_assert_step(dropped == 0, "server_send drop", param->name);

	start = time(NULL);
	router_shutdown(rtr);
	stop = time(NULL);
	elapsed = stop - start;
	mu_assert_step(elapsed < shutdown_timeout + 20, "router_shutdown timeout", param->name);
	mu_assert_step(blackholed == 0, "router_route blackholed", param->name);

}

MU_TEST_STEP(test_server_queuereader, router_param*)
{
	router *rtr;
	char config[280];

	queuefree_threshold_start = 1;
	queuefree_threshold_end = 3;
	send_timeout = 1;
	shutdown_timeout = 10;

	strcpy(relay_hostname, "relay");

	snprintf(config, sizeof(config), "%s/%s.conf", testdir, param->name);
	rtr = router_readconfig(NULL, config, 1, param->queuesize,
					param->batchsize, 1, 600, 0, 2003);
	if (rtr == NULL) {
		mu_fail_step("router_readconfig failed", param->name);
		return;
	}

	run_server_queuereader(rtr, param);

	router_param_cleanup(rtr, param, 0);
}

MU_TEST_SUITE(server_send_suite) {
	volatile size_t i;
	size_t n = 2;
	router_param steps[2];
	router_param_set(&steps[0], "failover2", 100, 10);
	router_param_set(&steps[1], "any_of2",   100, 10);

	for (i = 0; i < n; i++) {
		MU_RUN_TEST_STEP(test_server_send, &steps[i]);
		MU_RUN_TEST_STEP(test_server_shutdown_timeout, &steps[i]);
		MU_RUN_TEST_STEP(test_server_queuereader, &steps[i]);
	}

	for (i = 0; i < sizeof(steps) / sizeof(router_param); i++) {
		router_param_cleanup(NULL, &steps[i], 1);
	}
}

int main(int argc, char *argv[]) {
	char *dir = dirname(argv[0]);
	snprintf(testdir, sizeof(testdir), "%s/test", dir);
	signal(SIGPIPE, SIG_IGN);
	MU_RUN_SUITE(server_send_suite);
	MU_REPORT();
	return MU_EXIT_CODE;
}
