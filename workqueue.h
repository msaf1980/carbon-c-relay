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


#ifndef WORKQUEUE_H
#define WORKQUEUE_H 1

#include <pthread.h>

#include "queue.h"

typedef struct _pthread_condition_t {
        int              condition_predicate;
        pthread_mutex_t  mutex;
        pthread_cond_t   cond;
} pthread_condition_t;

int pthread_condition_init(pthread_condition_t *cond);

typedef struct _workqueue {
	queue *q;
	pthread_condition_t cond;
} workqueue;

int workqueue_init(workqueue *wq, size_t size);
void workqueue_destroy(workqueue *wq);
void workqueue_enqueue(workqueue *wq, const char *p);
char workqueue_putback(workqueue *wq, const char *p);
const char *workqueue_dequeue(workqueue *wq);
size_t workqueue_len(workqueue *wq);
size_t workqueue_free(workqueue *wq);

#endif
