#include "workqueue.h"

int
pthread_condition_init(pthread_condition_t *cond) {
	if (pthread_mutex_init(&cond->mutex, NULL) != 0) {
		return -1;
	}
	if (pthread_cond_init(&cond->cond, NULL) != 0) {
		return -1;
	}
	return 0;
}

int
workqueue_init(workqueue *wq, size_t size) {
	if (pthread_condition_init(&wq->cond) != 0)
		return -1;
	if ((wq->q = queue_new(size)) == NULL)
		return -1;
	return 0;
}

void
workqueue_destroy(workqueue *wq)
{
	queue_destroy(wq->q);
}

void
workqueue_enqueue(workqueue *wq, const char *p)
{
	queue_enqueue(wq->q, p);
	pthread_cond_signal(&wq->cond.cond);
}

char
workqueue_putback(workqueue *wq, const char *p)
{
	if (queue_putback(wq->q, p) == 0)
		return 0;
	pthread_cond_signal(&wq->cond.cond);
	return 1;
}

const char *workqueue_dequeue(workqueue *wq) {
	if (pthread_mutex_lock(&wq->cond.mutex) != 0)
        return NULL;
	if (pthread_cond_wait(&wq->cond.cond, &wq->cond.mutex) != 0)
		return NULL;
	return queue_dequeue(wq->q);
}

inline size_t
workqueue_len(workqueue *wq)
{
	return queue_len(wq->q);
}

inline size_t
workqueue_free(workqueue *wq)
{
	return queue_free(wq->q);
}
