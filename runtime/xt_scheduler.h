

#ifndef XT_SCHEDULER_H
#define XT_SCHEDULER_H

#include <stdint.h>


#define XT_FIBER_READY    0
#define XT_FIBER_WAITING  1  
#define XT_FIBER_SLEEPING 2  
#define XT_FIBER_DONE     3  


#define XT_MAX_FIBERS      65535 
#define XT_TIMER_HEAP_SIZE 8192 
#define XT_SCHED_TICK_US   1000  

typedef struct XTFiber {
    void* state;              
    int   (*poll)(void*);     
    int   status;
    int64_t wakeup_at;        
    void* wait_target;        
    uintptr_t result;         
    int   result_consumed;    
    int   in_free_list;       
    struct XTFiber* next;     
} XTFiber;

typedef struct {
    XTFiber* fibers;          
    int      fiber_count;

    XTFiber* ready_head;      
    XTFiber* ready_tail;

    XTFiber* timer_heap[XT_TIMER_HEAP_SIZE]; 
    int      timer_count;

    int64_t  now_us;          
    int      running;
    XTFiber* current;         
} XTScheduler;


extern XTScheduler* g_scheduler;


void      xt_scheduler_init();
XTFiber*  xt_scheduler_spawn(void* state, int (*poll)(void*));
void      xt_scheduler_run();       
void      xt_scheduler_yield();     
void      xt_scheduler_sleep_us(int64_t us); 
void      xt_scheduler_wait_task(void* task); 
void      xt_scheduler_wake_task(void* task); 
void      xt_fiber_set_result(uintptr_t v);   


void      xt_scheduler_enqueue(XTFiber* f);
XTFiber*  xt_scheduler_dequeue();
void      xt_scheduler_timer_add(XTFiber* f, int64_t wakeup_at);
void      xt_scheduler_timer_tick(int64_t now);

#endif
