








#ifndef XT_THREADPOOL_H
#define XT_THREADPOOL_H

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif



#if defined(_WIN32)
#include <windows.h>
typedef HANDLE xt_thread_t;
typedef CRITICAL_SECTION xt_mutex_t;
typedef CONDITION_VARIABLE xt_cond_t;
#else
#include <pthread.h>
typedef pthread_t xt_thread_t;
typedef pthread_mutex_t xt_mutex_t;
typedef pthread_cond_t xt_cond_t;
#endif


void xt_mutex_init(xt_mutex_t* m);
void xt_mutex_destroy(xt_mutex_t* m);
void xt_mutex_lock(xt_mutex_t* m);
void xt_mutex_unlock(xt_mutex_t* m);


void xt_cond_init(xt_cond_t* c);
void xt_cond_destroy(xt_cond_t* c);
void xt_cond_wait(xt_cond_t* c, xt_mutex_t* m);
void xt_cond_signal(xt_cond_t* c);
void xt_cond_broadcast(xt_cond_t* c);


typedef void* (*xt_thread_func)(void* arg);
int xt_thread_create(xt_thread_t* t, xt_thread_func f, void* arg);
int xt_thread_join(xt_thread_t t);




typedef void* (*xt_pool_task)(void* arg);


void xt_threadpool_init(int worker_count);


void xt_threadpool_shutdown(void);


int xt_threadpool_submit(xt_pool_task func, void* arg);


void* xt_threadpool_wait(int task_id);
void* xt_threadpool_try_wait(int task_id);


int xt_threadpool_worker_count(void);


int xt_threadpool_pending_count(void);

#ifdef __cplusplus
}
#endif

#endif 
