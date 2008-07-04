/*
 *  Copyright (C) 2004 Trog <trog@clamav.net>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 *  MA 02110-1301, USA.
 */

#if HAVE_CONFIG_H
#include "clamav-config.h"
#endif

#include <stdio.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>

#include "shared/output.h"

#include "thrmgr.h"
#include "others.h"

#define FALSE (0)
#define TRUE (1)

/* BSD and HP-UX need a bigger stacksize than the system default */
#if defined (C_BSD) || defined (C_HPUX)
#define C_BIGSTACK 1
#endif

static work_queue_t *work_queue_new(void)
{
	work_queue_t *work_q;
	
	work_q = (work_queue_t *) malloc(sizeof(work_queue_t));
	if (!work_q) {
		return NULL;
	}
	
	work_q->head = work_q->tail = NULL;
	work_q->item_count = 0;
	return work_q;
}

static int work_queue_add(work_queue_t *work_q, void *data)
{
	work_item_t *work_item;
	
	if (!work_q) {
		return FALSE;
	}
	work_item = (work_item_t *) malloc(sizeof(work_item_t));
	if (!work_item) {
		return FALSE;
	}
	
	work_item->next = NULL;
	work_item->data = data;
	gettimeofday(&(work_item->time_queued), NULL);
	
	if (work_q->head == NULL) {
		work_q->head = work_q->tail = work_item;
		work_q->item_count = 1;
	} else {
		work_q->tail->next = work_item;
		work_q->tail = work_item;
		work_q->item_count++;
	}
	return TRUE;
}

static void *work_queue_pop(work_queue_t *work_q)
{
	work_item_t *work_item;
	void *data;
	
	if (!work_q || !work_q->head) {
		return NULL;
	}
	work_item = work_q->head;
	data = work_item->data;
	work_q->head = work_item->next;
	if (work_q->head == NULL) {
		work_q->tail = NULL;
	}
	free(work_item);
	return data;
}

void thrmgr_destroy(threadpool_t *threadpool)
{
	if (!threadpool || (threadpool->state != POOL_VALID)) {
		return;
	}
  	if (pthread_mutex_lock(&threadpool->pool_mutex) != 0) {
   		logg("!Mutex lock failed\n");
    		exit(-1);
	}
	threadpool->state = POOL_EXIT;
	
	/* wait for threads to exit */
	if (threadpool->thr_alive > 0) {
		if (pthread_cond_broadcast(&(threadpool->pool_cond)) != 0) {
			pthread_mutex_unlock(&threadpool->pool_mutex);
			return;
		}
	}
	while (threadpool->thr_alive > 0) {
		if (pthread_cond_wait (&threadpool->pool_cond, &threadpool->pool_mutex) != 0) {
			pthread_mutex_unlock(&threadpool->pool_mutex);
			return;
		}
	}
  	if (pthread_mutex_unlock(&threadpool->pool_mutex) != 0) {
    		logg("!Mutex unlock failed\n");
    		exit(-1);
  	}
	
	pthread_mutex_destroy(&(threadpool->pool_mutex));
	pthread_cond_destroy(&(threadpool)->idle_cond);
	pthread_cond_destroy(&(threadpool->pool_cond));
	pthread_attr_destroy(&(threadpool->pool_attr));
	free(threadpool->queue);
	free(threadpool);
	return;
}

threadpool_t *thrmgr_new(int max_threads, int idle_timeout, void (*handler)(void *))
{
	threadpool_t *threadpool;
#if defined(C_BIGSTACK)
	size_t stacksize;
#endif
	
	if (max_threads <= 0) {
		return NULL;
	}
	
	threadpool = (threadpool_t *) malloc(sizeof(threadpool_t));
	if (!threadpool) {
		return NULL;
	}

	threadpool->queue = work_queue_new();
	if (!threadpool->queue) {
		free(threadpool);
		return NULL;
	}	
	threadpool->thr_max = max_threads;
	threadpool->thr_alive = 0;
	threadpool->thr_idle = 0;
	threadpool->idle_timeout = idle_timeout;
	threadpool->handler = handler;
	
	pthread_mutex_init(&(threadpool->pool_mutex), NULL);
	if (pthread_cond_init(&(threadpool->pool_cond), NULL) != 0) {
		pthread_mutex_destroy(&(threadpool->pool_mutex));
		free(threadpool->queue);
		free(threadpool);
		return NULL;
	}

	if (pthread_cond_init(&(threadpool->idle_cond),NULL) != 0)  {
		pthread_cond_destroy(&(threadpool->pool_cond));
		pthread_mutex_destroy(&(threadpool->pool_mutex));
		free(threadpool->queue);
		free(threadpool);
		return NULL;
	}

	if (pthread_attr_init(&(threadpool->pool_attr)) != 0) {
		pthread_cond_destroy(&(threadpool->idle_cond));
		pthread_cond_destroy(&(threadpool->pool_cond));
		pthread_mutex_destroy(&(threadpool->pool_mutex));
		free(threadpool->queue);
		free(threadpool);
		return NULL;
	}
	
	if (pthread_attr_setdetachstate(&(threadpool->pool_attr), PTHREAD_CREATE_DETACHED) != 0) {
		pthread_attr_destroy(&(threadpool->pool_attr));
		pthread_cond_destroy(&(threadpool->idle_cond));
		pthread_cond_destroy(&(threadpool->pool_cond));
		pthread_mutex_destroy(&(threadpool->pool_mutex));
		free(threadpool->queue);
		free(threadpool);
		return NULL;
	}

#if defined(C_BIGSTACK)
	pthread_attr_getstacksize(&(threadpool->pool_attr), &stacksize);
	stacksize = stacksize + 64 * 1024;
	if (stacksize < 1048576) stacksize = 1048576; /* at least 1MB please */
	logg("Set stacksize to %u\n", stacksize);
	pthread_attr_setstacksize(&(threadpool->pool_attr), stacksize);
#endif
	threadpool->state = POOL_VALID;

	return threadpool;
}

#ifdef OPTIMIZE_MEMORY_FOOTPRINT
/**
 * thrmgr_worker_stop_wait : set state to POOL_STOP, wake all thread worker, wait for them
 * to exit before continuing.
 */
void thrmgr_worker_stop_wait(threadpool_t * const threadpool)
{
	struct timespec timeout;
	int ret_cond;
	int loop = 2;
	
	if (!threadpool || (threadpool->state != POOL_VALID)) {
		return;
	}
  	if (pthread_mutex_lock(&threadpool->pool_mutex) != 0) {
   		logg("!Mutex lock failed\n");
    		exit(-1);
	}
	threadpool->state = POOL_STOP;
	
	/* wait for threads to exit */
	if (threadpool->thr_alive > 0) {
#ifdef CL_DEBUG
		logg("*%u active threads: waking them and entering wait loop\n", threadpool->thr_alive);
#endif
		if (pthread_cond_broadcast(&(threadpool->pool_cond)) != 0) {
			pthread_mutex_unlock(&threadpool->pool_mutex);
			logg("!Fatal: failed in cond broadcast 'pool_cond'\n");
			return;
		}
	}
	/* now, wait for the threads to exit, make 'loop' number of tries,  */
	while (threadpool->thr_alive > 0 && loop--) {		
#ifdef CL_DEBUG
		logg("*%u active threads. Waiting.\n", threadpool->thr_alive);
#endif
		timeout.tv_sec = time(NULL) + (threadpool->idle_timeout/2) + 10L;
		timeout.tv_nsec = 0;
		ret_cond = pthread_cond_timedwait (&threadpool->pool_cond, &threadpool->pool_mutex, &timeout);
		if (ret_cond == ETIMEDOUT) {
#ifdef CL_DEBUG
			logg("*%u active threads. Continue to wait.\n", threadpool->thr_alive);
#endif
		} else if (ret_cond == 0) {
#ifdef CL_DEBUG
			logg("*Received signal. %u active threads.\n", threadpool->thr_alive);
#endif
		}
	}
  	if (pthread_mutex_unlock(&threadpool->pool_mutex) != 0) {
    		logg("!Mutex unlock failed\n");
    		exit(-1);
  	}
}
#endif
#ifdef OPTIMIZE_MEMORY_FOOTPRINT
void thrmgr_setstate(threadpool_t * const threadpool, pool_state_t state )
{
  	if (pthread_mutex_lock(&threadpool->pool_mutex) != 0) {
   		logg("!Mutex lock failed\n");
    		exit(-1);
	}
	threadpool->state = state;
  	if (pthread_mutex_unlock(&threadpool->pool_mutex) != 0) {
    		logg("!Mutex unlock failed\n");
    		exit(-1);
  	}
}
#endif

static void *thrmgr_worker_cleanup(void *arg)
{
	threadpool_t *threadpool = (threadpool_t *) arg;
	
	if (pthread_mutex_lock(&(threadpool->pool_mutex)) != 0) {
		/* Fatal error */
		logg("!Fatal: mutex lock failed\n");
		exit(-2);
	}
	(threadpool->thr_alive) && threadpool->thr_alive--;
	/* logg("*Thread clean up, %u active threads.", threadpool->thr_alive); */
	if (threadpool->thr_alive == 0) {
		/* signal that all threads are finished */
		pthread_cond_broadcast(&threadpool->pool_cond);
	}
	if (pthread_mutex_unlock(&(threadpool->pool_mutex)) != 0) {
		/* Fatal error */
		logg("!Fatal: mutex unlock failed\n");
		exit(-2);
	}
}

static void *thrmgr_worker(void *arg)
{
	threadpool_t *threadpool = (threadpool_t *) arg;
	void *job_data;
	int retval, must_exit = FALSE;
	struct timespec timeout;
	
	/* Register cleanup procedure for worker in current thread */
	pthread_cleanup_push(thrmgr_worker_cleanup, arg);
	
	/* loop looking for work */
	for (;;) {
		if (pthread_mutex_lock(&(threadpool->pool_mutex)) != 0) {
			logg("!Fatal: mutex lock failed\n");
			exit(-2);
		}
		timeout.tv_sec = time(NULL) + threadpool->idle_timeout;
		timeout.tv_nsec = 0;
		threadpool->thr_idle++;
		while ( must_exit == FALSE 
				&& ((job_data = work_queue_pop(threadpool->queue)) == NULL)
				&& (threadpool->state != POOL_EXIT)) {
			/* Sleep, awaiting wakeup */
			pthread_cond_signal(&threadpool->idle_cond);
			retval = pthread_cond_timedwait(&(threadpool->pool_cond),
				&(threadpool->pool_mutex), &timeout);
			if (retval == ETIMEDOUT)
				must_exit = TRUE;
		}
		threadpool->thr_idle--;
		if (threadpool->state == POOL_EXIT) {
			must_exit = TRUE;
		}

		if (pthread_mutex_unlock(&(threadpool->pool_mutex)) != 0) {
			logg("!Fatal: mutex unlock failed\n");
			exit(-2);
		}
		if (must_exit) break;
		if (job_data) threadpool->handler(job_data);
		if (threadpool->state == POOL_STOP) break;
	}

#ifdef HAVE_PTHREAD_YIELD
	pthread_yield(); /* do not remove on premptive kernel e.g linux 2.6 */
#elif HAVE_SCHED_YIELD
	sched_yield();
#endif
	pthread_cleanup_pop(1);
	return NULL;
}

int thrmgr_dispatch(threadpool_t *threadpool, void *user_data)
{
	pthread_t thr_id;

	if (!threadpool) {
		return FALSE;
	}

	/* Lock the threadpool */
	if (pthread_mutex_lock(&(threadpool->pool_mutex)) != 0) {
		logg("!Mutex lock failed\n");
		return FALSE;
	}

	if (threadpool->state != POOL_VALID) {
		if (pthread_mutex_unlock(&(threadpool->pool_mutex)) != 0) {
			logg("!Mutex unlock failed\n");
		}
		return FALSE;
	}
	if (!work_queue_add(threadpool->queue, user_data)) {
		if (pthread_mutex_unlock(&(threadpool->pool_mutex)) != 0) {
			logg("!Mutex unlock failed\n");
			return FALSE;
		}
		return FALSE;
	}

	if ((threadpool->thr_idle == 0) &&
			(threadpool->thr_alive < threadpool->thr_max)) {
		/* Start a new thread */
		if (pthread_create(&thr_id, &(threadpool->pool_attr),
				thrmgr_worker, threadpool) != 0) {
			logg("!pthread_create failed\n");
		} else {
			threadpool->thr_alive++;
		}
	}
	pthread_cond_signal(&(threadpool->pool_cond));

	if (pthread_mutex_unlock(&(threadpool->pool_mutex)) != 0) {
		logg("!Mutex unlock failed\n");
		return FALSE;
	}
	return TRUE;
}
