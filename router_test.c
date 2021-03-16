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

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <libgen.h>

#include "consistent-hash.h"
#include "md5.h"
#include "relay.h"
#include "router.h"
#include "server.h"
#include "cluster.h"

#define SRVCNT 8
#define REPLCNT 2

unsigned char mode = 0;

char testdir[200];

int relaylog(enum logdst dest, const char *fmt, ...) {
    (void) dest;
    (void) fmt;
    return 0;
}

router *router_new(void);
char *router_validate_address(router *rtr, char **retip,
                              unsigned short *retport,
                              struct addrinfo **retsaddr,
                              struct addrinfo **rethint, char *ip,
                              con_proto proto);

#define CTEST_MAIN
#define CTEST_SEGFAULT

#include "ctest.h"

char relay_hostname[256];
#ifdef HAVE_SSL
char *sslCA = NULL;
char sslCAisdir = 0;
#endif
static int batchsize = 2500;
static int queuesize = 25000;
static int maxstalls = 4;
static unsigned short iotimeout = 600;
static unsigned short listenport = 2003;
static int sockbufsize = 0;

CTEST(cluster_test, cluster_ttl) {
    ASSERT_EQUAL(1 * 60, cluster_ttl(1));
    ASSERT_EQUAL(3 * 60, cluster_ttl(3));
    ASSERT_EQUAL(10 * 60, cluster_ttl(10));
    ASSERT_EQUAL(11 * 60, cluster_ttl(11));
    ASSERT_EQUAL(60 * 60, cluster_ttl(60));
    ASSERT_EQUAL(61 * 60, cluster_ttl(61));
    ASSERT_EQUAL(70 * 60, cluster_ttl(70));
    ASSERT_EQUAL(71 * 60, cluster_ttl(71));
}

queue *server_queue(server *s);

CTEST_DATA(router_test) {
    router *r;
    struct addrinfo *retsaddr;
    struct addrinfo *rethint;
};

CTEST_SETUP(router_test) {
    shutdown_timeout = 10;
    data->r = router_new();
    data->retsaddr = NULL;
    data->rethint = NULL;
}

CTEST_TEARDOWN(router_test) {
    router_free(data->r);
    free(data->rethint);
    freeaddrinfo(data->retsaddr);
}

CTEST2(router_test, router_validate_address_hostname) {
    char *retip;
    unsigned short retport;
    char ip[256];

    strcpy(ip, "host");
    ASSERT_NULL(router_validate_address(data->r, &retip, &retport, &weight,
                                        &data->retsaddr, &data->rethint, ip,
                                        CON_TCP));
    ASSERT_EQUAL(retport, 2003);
    ASSERT_STR(retip, "host");
    free(data->rethint);
    data->rethint = NULL;
    freeaddrinfo(data->retsaddr);
    data->retsaddr = NULL;

    strcpy(ip, "[host:1]");
    ASSERT_NULL(router_validate_address(data->r, &retip, &retport, &weight,
                                        &data->retsaddr, &data->rethint, ip,
                                        CON_TCP));
    ASSERT_EQUAL(retport, 2003);
    ASSERT_STR(retip, "host:1");
    freeaddrinfo(data->rethint);
    data->rethint = NULL;
    freeaddrinfo(data->retsaddr);
    data->retsaddr = NULL;
}

CTEST2(router_test, router_validate_address_port) {
    char *retip;
    unsigned short retport;
    char ip[256];

    strcpy(ip, ":2005");
    ASSERT_NULL(router_validate_address(data->r, &retip, &retport, &weight,
                                        &data->retsaddr, &data->rethint, ip,
                                        CON_TCP));
    ASSERT_EQUAL(retport, 2005);
    ASSERT_NULL(retip);
    freeaddrinfo(data->rethint);
    data->rethint = NULL;
    freeaddrinfo(data->retsaddr);
    data->retsaddr = NULL;

    strcpy(ip, ":2005a");
    ASSERT_STR(router_validate_address(data->r, &retip, &retport, &weight,
                                       &data->retsaddr, &data->rethint, ip,
                                       CON_TCP),
               "invalid port number '2005a'");
    freeaddrinfo(data->rethint);
    data->rethint = NULL;
    freeaddrinfo(data->retsaddr);
    data->retsaddr = NULL;
}

CTEST2(router_test, router_validate_address_hostname_port) {
    char *retip;
    unsigned short retport;
    char ip[256];

    strcpy(ip, "host:2004");
    ASSERT_NULL(router_validate_address(data->r, &retip, &retport, &weight,
                                        &data->retsaddr, &data->rethint, ip,
                                        CON_TCP));
    ASSERT_EQUAL(retport, 2004);
    ASSERT_STR(retip, "host");
    free(data->rethint);
    data->rethint = NULL;
    freeaddrinfo(data->retsaddr);
    data->retsaddr = NULL;

    strcpy(ip, "host:2005a");
    ASSERT_STR(router_validate_address(data->r, &retip, &retport, &weight,
                                       &data->retsaddr, &data->rethint, ip,
                                       CON_TCP),
               "invalid port number '2005a'");
    freeaddrinfo(data->rethint);
    data->rethint = NULL;
    freeaddrinfo(data->retsaddr);
    data->retsaddr = NULL;

    strcpy(ip, "[host:1]:2002");
    ASSERT_NULL(router_validate_address(data->r, &retip, &retport, &weight,
                                        &data->retsaddr, &data->rethint, ip,
                                        CON_TCP));
    ASSERT_EQUAL(retport, 2002);
    ASSERT_STR(retip, "host:1");
    freeaddrinfo(data->rethint);
    data->rethint = NULL;
    freeaddrinfo(data->retsaddr);
    data->retsaddr = NULL;

    strcpy(ip, "[host:1]:2006c");
    ASSERT_STR(router_validate_address(data->r, &retip, &retport, &weight,
                                       &data->retsaddr, &data->rethint, ip,
                                       CON_TCP),
               "invalid port number '2006c'");
    freeaddrinfo(data->rethint);
    data->rethint = NULL;
    freeaddrinfo(data->retsaddr);
    data->retsaddr = NULL;
}

CTEST2(router_test, router_validate_address_hostname_port_weight) {
    char *retip;
    unsigned short retport, weight;
    char ip[256];

    strcpy(ip, "host:2004:0");
    ASSERT_NULL(router_validate_address(data->r, &retip, &retport, &weight,
                                        &data->retsaddr, &data->rethint, ip,
                                        CON_TCP));
    ASSERT_EQUAL(retport, 2004);
    ASSERT_STR(retip, "host");
    ASSERT_EQUAL(weight, 0);
    free(data->rethint);
    data->rethint = NULL;
    freeaddrinfo(data->retsaddr);
    data->retsaddr = NULL;

    strcpy(ip, "host:2004:b");
    ASSERT_NULL(router_validate_address(data->r, &retip, &retport, &weight,
                                        &data->retsaddr, &data->rethint, ip,
                                        CON_TCP));
    ASSERT_EQUAL(retport, 2004);
    ASSERT_STR(retip, "host");
    ASSERT_EQUAL(weight, 0);
    free(data->rethint);
    data->rethint = NULL;
    freeaddrinfo(data->retsaddr);
    data->retsaddr = NULL;

    strcpy(ip, "host:2005:-1");
    ASSERT_STR(router_validate_address(data->r, &retip, &retport, &weight,
                                       &data->retsaddr, &data->rethint, ip,
                                       CON_TCP),
               "invalid weight '-1'");
    freeaddrinfo(data->rethint);
    data->rethint = NULL;
    freeaddrinfo(data->retsaddr);
    data->retsaddr = NULL;

    strcpy(ip, "host:2005:a");
    ASSERT_STR(router_validate_address(data->r, &retip, &retport, &weight,
                                       &data->retsaddr, &data->rethint, ip,
                                       CON_TCP),
               "invalid weight 'a'");
    freeaddrinfo(data->rethint);
    data->rethint = NULL;
    freeaddrinfo(data->retsaddr);
    data->retsaddr = NULL;
}

CTEST(router_reload_test, swap_queue) {
    router *rtr, *newrtr;
    servers *s;
    queue *oq_3, *q_3;
    cluster *c;
    char config_1[256], config_2[256], config_3[256];

    snprintf(config_1, sizeof(config_1), "%s/reload-1.conf", testdir);
    snprintf(config_2, sizeof(config_2), "%s/reload-2.conf", testdir);
    snprintf(config_3, sizeof(config_3), "%s/reload-3.conf", testdir);

    /* load */
    rtr = router_readconfig(NULL, config_1, 2,
					queuesize, batchsize, maxstalls,
					iotimeout, sockbufsize, listenport);
    ASSERT_NOT_NULL_D(rtr, "failed to read configuration 1");
    router_start(rtr);
    ASSERT_NULL_D((uintmax_t) router_cluster(rtr, "one")->queue, "cluster one (forward) shared queue must be NULL (configuration 1)");
    ASSERT_NULL_D((uintmax_t) router_cluster(rtr, "two")->queue, "cluster two (any_of) shared queue must be NULL (configuration 1)");
    c = router_cluster(rtr, "three");
    oq_3 = c->queue;
    for (s = cluster_servers(c); s != NULL; s = s->next) {
        /* check for shared queue */
        ASSERT_EQUAL_D(1, server_queue_is_shared(s->server), "cluster three (failover) shared queue not set (configuration 1)");        
        ASSERT_EQUAL_U_D((uintmax_t) oq_3, (uintmax_t) server_queue(s->server), "cluster three (failover) shared queue not set (configuration 1)");
    }
    ASSERT_NULL_D((uintmax_t) router_cluster(rtr, "four")->queue, "cluster four (carbon_ch) shared queue must be NULL (configuration 1)");

    /* 1-st reload */
    newrtr = router_readconfig(NULL, config_2, 2,
					queuesize, batchsize, maxstalls,
					iotimeout, sockbufsize, listenport);
    ASSERT_NOT_NULL_D(newrtr, "failed to read configuration 2");
    router_swap(newrtr, rtr);
    router_free(rtr);
    rtr = newrtr;
    ASSERT_NULL_D((uintmax_t) router_cluster(rtr, "one")->queue, "cluster one (forward) shared queue must be NULL (configuration 2)");
    ASSERT_NULL_D((uintmax_t) router_cluster(rtr, "two")->queue, "cluster two (any_of) shared queue must be NULL (configuration 2)");
    c = router_cluster(rtr, "three");
    q_3 = c->queue;
    ASSERT_EQUAL_U_D((uintmax_t) oq_3, (uintmax_t) q_3, "cluster three (failover) shared queue not swapped (configuration 2)");
    for (s = cluster_servers(c); s != NULL; s = s->next) {
        /* check for shared queue */
        ASSERT_EQUAL_D(1, server_queue_is_shared(s->server), "cluster three (failover) shared queue not set (configuration 2)");        
        ASSERT_EQUAL_U_D((uintmax_t) q_3, (uintmax_t) server_queue(s->server), "cluster three (failover) shared queue not set (configuration 2)");
    }
    ASSERT_NULL_D((uintmax_t) router_cluster(rtr, "four")->queue, "cluster four (carbon_ch) shared queue must be NULL (configuration 2)");

    /* 2-st reload */
    newrtr = router_readconfig(NULL, config_3, 2,
					queuesize, batchsize, maxstalls,
					iotimeout, sockbufsize, listenport);
    ASSERT_NOT_NULL_D(newrtr, "failed to read configuration 3");
    router_swap(newrtr, rtr);
    router_free(rtr);
    rtr = newrtr;
    ASSERT_NULL_D((uintmax_t) router_cluster(rtr, "one")->queue, "cluster one (forward) shared queue must be NULL (configuration 3)");
    ASSERT_NULL_D((uintmax_t) router_cluster(rtr, "two")->queue, "cluster two (any_of) shared queue must be NULL (configuration 3)");
    c = router_cluster(rtr, "three");
    q_3 = c->queue;
    ASSERT_EQUAL_U_D((uintmax_t) oq_3, (uintmax_t) q_3, "cluster three (failover) shared queue not swapped (configuration 3)");
    for (s = cluster_servers(c); s != NULL; s = s->next) {
        /* check for shared queue */
        ASSERT_EQUAL_D(1, server_queue_is_shared(s->server), "cluster three (failover) shared queue not set (configuration 3)");        
        ASSERT_EQUAL_U_D((uintmax_t) q_3, (uintmax_t) server_queue(s->server), "cluster three (failover) shared queue not set (configuration 3)");
    }
    ASSERT_EQUAL_U_D((uintmax_t) oq_3, (uintmax_t) q_3, "cluster three (failover) shared queue not swapped (configuration 3)");
    ASSERT_NULL_D((uintmax_t) router_cluster(rtr, "four")->queue, "cluster four (carbon_ch) shared queue must be NULL (configuration 3)");

    /* cleanup */
    router_shutdown(rtr, 0, NULL);
    router_free(rtr);
}

int main(int argc, const char *argv[]) {
    const char *dir = dirname((char *) argv[0]);

    snprintf(testdir, sizeof(testdir), "%s/test", dir);
    strcpy(relay_hostname, "relay");

    return ctest_main(argc, argv);
}
