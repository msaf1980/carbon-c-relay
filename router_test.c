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

#include "consistent-hash.h"
#include "router.h"
#include "server.h"
#include "relay.h"
#include "md5.h"

#define SRVCNT 8
#define REPLCNT 2

unsigned char mode = 0;

int
relaylog(enum logdst dest, const char *fmt, ...)
{
	(void) dest;
	(void) fmt;
	return 0;
}

router *router_new(void);
char *router_validate_address(router *rtr, char **retip, unsigned short *retport, void **retsaddr, void **rethint, char *ip, con_proto proto);

#include "minunit.h"

static router *r;

char relay_hostname[256];
#ifdef HAVE_SSL
char *sslCA = NULL;
char sslCAisdir = 0;
#endif

MU_TEST(router_validate_address_hostname) {
	char *retip;
	unsigned short retport;
	void *retsaddr, *rethint;
	char ip[256];

	strcpy(ip, "host");
	mu_assert_string_eq(router_validate_address(r, &retip, &retport, &retsaddr, &rethint, ip, CON_TCP), NULL);
	mu_check(retport == 2003);
	mu_assert_string_eq(retip, "host");

	strcpy(ip, "[host:1]");
	mu_assert_string_eq(router_validate_address(r, &retip, &retport, &retsaddr, &rethint, ip, CON_TCP), NULL);
	mu_check(retport == 2003);
	mu_assert_string_eq(retip, "host:1");
}

MU_TEST(router_validate_address_port) {
	char *retip;
	unsigned short retport;
	void *retsaddr, *rethint;
	char ip[256];

	strcpy(ip, ":2005");
	mu_assert_string_eq(router_validate_address(r, &retip, &retport, &retsaddr, &rethint, ip, CON_TCP), NULL);
	mu_check(retport == 2005);
	mu_check(retip == NULL);

	strcpy(ip, ":2005a");
	mu_check(router_validate_address(r, &retip, &retport, &retsaddr, &rethint, ip, CON_TCP) != NULL);
}

MU_TEST(router_validate_address_hostname_port) {
	char *retip;
	unsigned short retport;
	void *retsaddr, *rethint;
	char ip[256];

	strcpy(ip, "host:2005");
	mu_assert_string_eq(router_validate_address(r, &retip, &retport, &retsaddr, &rethint, ip, CON_TCP), NULL);
	mu_check(retport == 2005);
	mu_assert_string_eq(retip, "host");

	strcpy(ip, "host:2005a");
	mu_check(router_validate_address(r, &retip, &retport, &retsaddr, &rethint, ip, CON_TCP) != NULL);

	strcpy(ip, "[host:1]:2005");
	mu_assert_string_eq(router_validate_address(r, &retip, &retport, &retsaddr, &rethint, ip, CON_TCP), NULL);
	mu_check(retport == 2005);
	mu_assert_string_eq(retip, "host:1");

	strcpy(ip, "[host:1]:2005a");
	mu_check(router_validate_address(r, &retip, &retport, &retsaddr, &rethint, ip, CON_TCP) != NULL);
}

MU_TEST_SUITE(test_suite) {
	MU_RUN_TEST(router_validate_address_hostname);
	MU_RUN_TEST(router_validate_address_port);
	MU_RUN_TEST(router_validate_address_hostname_port);
}

int main(int argc, char *argv[]) {
	if ((r = router_new()) == NULL) {
		return 1;
	}
	MU_RUN_SUITE(test_suite);
	MU_REPORT();
	return MU_EXIT_CODE;
}
