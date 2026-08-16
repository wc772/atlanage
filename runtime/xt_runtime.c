 














#define __USE_MINGW_ANSI_STDIO 1 
#include "xt_runtime.h"
#pragma comment(lib, "ws2_32")
#pragma comment(lib, "wininet")
#include "xt_threadpool.h"
#include "xt_net.h"
#include <inttypes.h>
#include <time.h>
#include <locale.h>
#include <stddef.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <math.h>

#ifdef _WIN32
#include <shellapi.h>
#endif


static void print_pool_stats();
static int xt_is_real_ptr(XTValue val);

 







typedef struct XTArena {
    XTObject header;    
    char* buffer;       
    size_t size;        
    size_t offset;      
    struct XTArena* next; 
} XTArena;


XTArena* g_current_arena = NULL; 

#ifdef _WIN32
static __declspec(thread) XTArena* g_thread_arena = NULL; 
#else
static __thread XTArena* g_thread_arena = NULL;
#endif

static XTValue g_xt_args = XT_NULL;      
static XTWeakSlot* g_weak_slots = NULL;   
static xt_chan_mutex_t g_weak_mutex;      



#if defined(_WIN32)
static DWORD g_main_thread_id = 0;
#define XT_THREAD_SELF() GetCurrentThreadId()
#define XT_THREAD_EQ(a,b) ((a)==(b))
#else
static pthread_t g_main_thread_id;
static int g_main_thread_id_set = 0;
#define XT_THREAD_SELF() pthread_self()
#define XT_THREAD_EQ(a,b) pthread_equal((a),(b))
#endif


#define WEAK_LOCK()   XT_CHAN_MUTEX_LOCK(&g_weak_mutex)
#define WEAK_UNLOCK() XT_CHAN_MUTEX_UNLOCK(&g_weak_mutex)


XTArena* xt_arena_new(size_t size);
void* xt_arena_alloc_raw(size_t size);
void* xt_arena_alloc(size_t size, uint32_t type_id);
static uint64_t xt_hash_value(XTValue val);

 










XTScheduler* g_scheduler = NULL;

#if defined(_WIN32)
static int64_t _sched_now_us() {
    static LARGE_INTEGER _freq = {0};
    if (_freq.QuadPart == 0) { QueryPerformanceFrequency(&_freq); }
    LARGE_INTEGER _now; QueryPerformanceCounter(&_now);
    return (int64_t)((_now.QuadPart * 1000000) / _freq.QuadPart);
}


#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002  
#endif
#define _sched_sleep_us(us) do { if (us > 0) { \
    HANDLE _t = CreateWaitableTimerEx(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS); \
    if (!_t) { _t = CreateWaitableTimer(NULL, TRUE, NULL); } \
    LARGE_INTEGER _due; _due.QuadPart = -(LONGLONG)(us * 10); \
    SetWaitableTimer(_t, &_due, 0, NULL, NULL, FALSE); WaitForSingleObject(_t, INFINITE); CloseHandle(_t); } } while(0)
#else
static int64_t _sched_now_us() {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}
#define _sched_sleep_us(us) do { if (us > 0) { struct timespec _ts = {0, (long)(us * 1000)}; nanosleep(&_ts, NULL); } } while(0)
#endif

static void _sched_timer_up(XTFiber** heap, int idx) {
    while (idx > 0) { int p = (idx-1)/2; if (heap[idx]->wakeup_at >= heap[p]->wakeup_at) break;
        XTFiber* t = heap[idx]; heap[idx] = heap[p]; heap[p] = t; idx = p; }
}
static void _sched_timer_down(XTFiber** heap, int count, int idx) {
    for (;;) { int l=idx*2+1, r=idx*2+2, s=idx;
        if (l<count && heap[l]->wakeup_at < heap[s]->wakeup_at) s=l;
        if (r<count && heap[r]->wakeup_at < heap[s]->wakeup_at) s=r;
        if (s==idx) break; XTFiber* t=heap[idx]; heap[idx]=heap[s]; heap[s]=t; idx=s; }
}



static xt_chan_mutex_t g_sched_mutex;
static int g_sched_mutex_inited = 0;
#define SCHED_LOCK()   do { if (g_sched_mutex_inited) XT_CHAN_MUTEX_LOCK(&g_sched_mutex); } while(0)
#define SCHED_UNLOCK() do { if (g_sched_mutex_inited) XT_CHAN_MUTEX_UNLOCK(&g_sched_mutex); } while(0)



#if defined(_WIN32)
static HANDLE g_sched_wake_event = NULL;
#endif



static int* g_fiber_free_ids = NULL;
static int  g_fiber_free_top = 0;


static void _xt_fiber_recycle(int fid) {
    if (!g_scheduler || fid < 0 || fid >= g_scheduler->fiber_count) return;
    XTFiber* f = &g_scheduler->fibers[fid];
    if (f->status != XT_FIBER_DONE || !f->result_consumed || f->in_free_list) return;
    if (f->state) { free(f->state); f->state = NULL; }
    if (f->result) { xt_release(f->result); f->result = 0; }
    f->in_free_list = 1;
    SCHED_LOCK();
    g_fiber_free_ids[g_fiber_free_top++] = fid;
    SCHED_UNLOCK();
}


static void _sched_enqueue_nolock(XTFiber* f) {
    f->next = NULL;
    if (g_scheduler->ready_tail) { g_scheduler->ready_tail->next = f; }
    else { g_scheduler->ready_head = f; }
    g_scheduler->ready_tail = f;
}

void xt_scheduler_timer_add(XTFiber* f, int64_t wakeup_at) {
    SCHED_LOCK();
    if (g_scheduler->timer_count >= XT_TIMER_HEAP_SIZE) {
        SCHED_UNLOCK();
        fprintf(stderr, "[玄铁运行时错误] 定时器堆溢出(超过 %d 个并发睡眠),fiber 无法注册睡眠\n", XT_TIMER_HEAP_SIZE);
        exit(1);
    }
    f->wakeup_at = wakeup_at; f->status = XT_FIBER_SLEEPING;
    int idx = g_scheduler->timer_count++;
    g_scheduler->timer_heap[idx] = f;
    _sched_timer_up(g_scheduler->timer_heap, idx);
    SCHED_UNLOCK();
}
void xt_scheduler_timer_tick(int64_t now) {
    SCHED_LOCK();
    while (g_scheduler->timer_count > 0) {
        XTFiber* f = g_scheduler->timer_heap[0];
        if (f->wakeup_at > now) break;
        g_scheduler->timer_heap[0] = g_scheduler->timer_heap[--g_scheduler->timer_count];
        if (g_scheduler->timer_count > 0) _sched_timer_down(g_scheduler->timer_heap, g_scheduler->timer_count, 0);
        f->status = XT_FIBER_READY; _sched_enqueue_nolock(f);
    }
    SCHED_UNLOCK();
}
void xt_scheduler_enqueue(XTFiber* f) {
    SCHED_LOCK();
    _sched_enqueue_nolock(f);
    SCHED_UNLOCK();
}
XTFiber* xt_scheduler_dequeue() {
    SCHED_LOCK();
    XTFiber* f = g_scheduler->ready_head;
    if (f) { g_scheduler->ready_head = f->next; if (!g_scheduler->ready_head) g_scheduler->ready_tail = NULL; f->next = NULL; }
    SCHED_UNLOCK();
    return f;
}
void xt_scheduler_init() {
    g_scheduler = (XTScheduler*)calloc(1, sizeof(XTScheduler));
    g_scheduler->fibers = (XTFiber*)calloc(XT_MAX_FIBERS, sizeof(XTFiber));
    g_fiber_free_ids = (int*)malloc(sizeof(int) * XT_MAX_FIBERS);
    g_fiber_free_top = 0;
    if (!g_sched_mutex_inited) { XT_CHAN_MUTEX_INIT(&g_sched_mutex); g_sched_mutex_inited = 1; }
#if defined(_WIN32)
    if (!g_sched_wake_event) { g_sched_wake_event = CreateEvent(NULL, FALSE, FALSE, NULL); } 
#endif
}
XTFiber* xt_scheduler_spawn(void* state, int (*poll)(void*)) {
    if (!g_scheduler) return NULL;
    SCHED_LOCK();
    XTFiber* f = NULL;
    if (g_fiber_free_top > 0) {
        
        int idx = g_fiber_free_ids[--g_fiber_free_top];
        f = &g_scheduler->fibers[idx];
    } else {
        if (g_scheduler->fiber_count >= XT_MAX_FIBERS) { SCHED_UNLOCK(); return NULL; }
        f = &g_scheduler->fibers[g_scheduler->fiber_count++];
    }
    f->state = state; f->poll = poll; f->status = XT_FIBER_READY;
    f->wait_target = NULL; f->result = 0; f->next = NULL; f->wakeup_at = 0;
    f->result_consumed = 0; f->in_free_list = 0;
    _sched_enqueue_nolock(f);
    SCHED_UNLOCK();
    return f;
}

XTValue xt_fiber_spawn(void* state, int (*poll)(void*)) {
    XTFiber* f = xt_scheduler_spawn(state, poll);
    if (!f) {
        
        fprintf(stderr, "[玄铁运行时错误] fiber 池耗尽(超过 %d 个),无法创建新 fiber\n", XT_MAX_FIBERS);
        exit(1);
    }
    
    
    return (XTValue)(uintptr_t)(f - g_scheduler->fibers) + 1;
}

void xt_scheduler_run() {
    if (!g_scheduler || g_scheduler->fiber_count == 0) return;
    
    
    SCHED_LOCK();
    if (g_scheduler->running) { SCHED_UNLOCK(); return; }
    g_scheduler->running = 1;
    SCHED_UNLOCK();
    while (g_scheduler->running) {
        g_scheduler->now_us = _sched_now_us();
        xt_scheduler_timer_tick(g_scheduler->now_us);
        XTFiber* f = xt_scheduler_dequeue();
        if (!f) {
            int64_t next_wake = 0;
            SCHED_LOCK();
            if (g_scheduler->timer_count > 0) {
                next_wake = g_scheduler->timer_heap[0]->wakeup_at - g_scheduler->now_us;
                if (next_wake < 0) next_wake = 0;
            }
            if (next_wake == 0) {
                int has_waiting = 0;
                for (int i = 0; i < g_scheduler->fiber_count; i++)
                    if (g_scheduler->fibers[i].status == XT_FIBER_WAITING) { has_waiting = 1; break; }
                SCHED_UNLOCK();
                if (has_waiting) {
#if defined(_WIN32)
                    
                    if (g_sched_wake_event) { WaitForSingleObject(g_sched_wake_event, 1); }
                    else { _sched_sleep_us(1000); }
#else
                    _sched_sleep_us(1000);
#endif
                }
                else { g_scheduler->running = 0; }
            } else { SCHED_UNLOCK(); _sched_sleep_us(next_wake < 100 ? 100 : next_wake); }
            continue;
        }
        g_scheduler->current = f;
        int result = f->poll(f->state);
        g_scheduler->current = NULL;
        if (result != 0) {
            f->status = XT_FIBER_DONE;
            xt_scheduler_wake_task(f);
            xt_scheduler_wake_task((void*)(uintptr_t)(f - g_scheduler->fibers + 1));
        }
    }
}
void xt_scheduler_yield() {
    SCHED_LOCK();
    if (g_scheduler->current) { g_scheduler->current->status = XT_FIBER_READY; _sched_enqueue_nolock(g_scheduler->current); }
    SCHED_UNLOCK();
}
void xt_scheduler_sleep_us(int64_t us) {
    if (g_scheduler->current) { xt_scheduler_timer_add(g_scheduler->current, g_scheduler->now_us + us); }
}
void xt_scheduler_wait_task(void* task) {
    SCHED_LOCK();
    if (g_scheduler->current) { g_scheduler->current->status = XT_FIBER_WAITING; g_scheduler->current->wait_target = task; }
    SCHED_UNLOCK();
}
void xt_scheduler_wake_task(void* task) {
    if (!g_scheduler) return;
    SCHED_LOCK();
    for (int i = 0; i < g_scheduler->fiber_count; i++) {
        XTFiber* f = &g_scheduler->fibers[i];
        if (f->status == XT_FIBER_WAITING && f->wait_target == task) {
            f->status = XT_FIBER_READY; f->wait_target = NULL; _sched_enqueue_nolock(f);
        }
    }
    SCHED_UNLOCK();
#if defined(_WIN32)
    if (g_sched_wake_event) SetEvent(g_sched_wake_event);  
#endif
}
void xt_fiber_set_result(uintptr_t v) {
    if (!g_scheduler || !g_scheduler->current) return;
    
    
    SCHED_LOCK();
    XTValue old = g_scheduler->current->result;
    g_scheduler->current->result = v;
    SCHED_UNLOCK();
    if (old) xt_release(old);
}

void xt_init() {
#ifdef _WIN32
    SetConsoleOutputCP(65001);   
    setlocale(LC_ALL, ".UTF8");  
    fflush(stdout);              
#endif
    XT_CHAN_MUTEX_INIT(&g_weak_mutex);  
    g_main_thread_id = XT_THREAD_SELF(); 
    xt_threadpool_init(0);              
    xt_net_init();                      
    xt_scheduler_init();                
}

 




void xt_init_args(int argc, char** argv) {
#ifdef _WIN32
    
    int wargc;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (wargv) {
        g_xt_args = xt_array_new(wargc);
        for (int i = 0; i < wargc; i++) {
            int utf8_len = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, NULL, 0, NULL, NULL);
            if (utf8_len > 0) {
                char* utf8_str = (char*)malloc(utf8_len);
                WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, utf8_str, utf8_len, NULL, NULL);
                XTString* s = xt_string_new(utf8_str);
                xt_array_append(g_xt_args, (XTValue)s);
                xt_release((XTValue)s);
                free(utf8_str);
            } else {
                XTString* s = xt_string_new("");
                xt_array_append(g_xt_args, (XTValue)s);
                xt_release((XTValue)s);
            }
        }
        LocalFree(wargv);
        return;
    }
#endif

    
    g_xt_args = xt_array_new(argc);
    for (int i = 0; i < argc; i++) {
        if (argv[i]) {
            XTString* s = xt_string_new(argv[i]);
            xt_array_append(g_xt_args, (XTValue)s);
            xt_release((XTValue)s);
        } else {
            XTString* s = xt_string_new("");
            xt_array_append(g_xt_args, (XTValue)s);
            xt_release((XTValue)s);
        }
    }
}

 




XTValue xt_get_args() {
    if (g_xt_args == XT_NULL) {
        return xt_array_new(0);
    }
    xt_retain(g_xt_args); 
    return g_xt_args;
}


#define XT_DEBUG_MODE 0
#if XT_DEBUG_MODE
#define XT_DEBUG_PRINT(...) do { printf("DEBUG: " __VA_ARGS__); fflush(stdout); } while(0)
#else
#define XT_DEBUG_PRINT(...)
#endif

 


static char* xt_strdup(const char* s) {
    if (!s) return NULL;
    size_t len = strlen(s) + 1;
    char* res = (char*)malloc(len);
    if (res) memcpy(res, s, len);
    return res;
}

 


void xt_print_int(int64_t val) {
    printf("%" PRId64 "\n", val);
    fflush(stdout); 
}

 









int xt_ffi_printf(XTString* fmt, XTValue arg) {
    
    if (!fmt || !xt_is_real_ptr((XTValue)fmt)) {
        return 0;
    }
    if (!fmt->data) return 0;
    
    
    int res = fprintf(stdout, fmt->data, arg);
    fflush(stdout);
    return res;
}

 





XTValue xt_int_new(int64_t val) {
    return XT_FROM_INT(val);
}

 




void* xt_float_new(double val) {
    typedef struct { XTObject header; double value; } XTFloat;
    XTFloat* obj = (XTFloat*)xt_malloc(sizeof(XTFloat), XT_TYPE_FLOAT);
    obj->value = val;
    return (void*)obj;
}

 




XTValue xt_bool_new(int val) {
    return XT_FROM_BOOL(val);
}

 







XTString* xt_string_new_len(const char* data, size_t len) {
    XTString* s = (XTString*)xt_malloc(sizeof(XTString), XT_TYPE_STRING);
    s->length = len;
    
    if (g_current_arena) {
        
        s->data = (char*)xt_arena_alloc_raw(len + 1);
        s->data_in_arena = 1;
    } else {
        s->data = (char*)malloc(len + 1);
        s->data_in_arena = 0;
    }
    
    if (s->data) {
        memcpy(s->data, data, len);
        s->data[len] = '\0'; 
    }
    return s;
}

 


XTString* xt_string_new(const char* data) {
    if (!data) data = "";
    return xt_string_new_len(data, strlen(data));
}

 


XTString* xt_string_from_char(char c) {
    char buf[2] = {c, '\0'};
    return xt_string_new(buf);
}

 





XTValue xt_string_get_char(XTValue str_val, int64_t index) {
    if (!XT_IS_REAL_PTR(str_val)) return XT_NULL;
    XTObject* obj = (XTObject*)str_val;
    if (obj->type_id != XT_TYPE_STRING) return XT_NULL;
    
    XTString* s = (XTString*)str_val;
    if (index < 0) return XT_NULL;
    
    const char* p = s->data;
    int64_t current = 0;
    
    while (*p && current < index) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x80) p += 1;
        else if ((c & 0xE0) == 0xC0) p += 2;
        else if ((c & 0xF0) == 0xE0) p += 3;
        else if ((c & 0xF8) == 0xF0) p += 4;
        else p += 1; 
        current++;
    }
    
    if (!*p) return XT_NULL;
    
    
    int len = 0;
    unsigned char c = (unsigned char)*p;
    if (c < 0x80) len = 1;
    else if ((c & 0xE0) == 0xC0) len = 2;
    else if ((c & 0xF0) == 0xE0) len = 3;
    else if ((c & 0xF8) == 0xF0) len = 4;
    else len = 1;
    
    char buf[5] = {0};
    for (int i = 0; i < len && p[i]; i++) buf[i] = p[i];
    
    return (XTValue)xt_string_new(buf);
}

 


XTValue xt_string_get_byte(XTValue str_val, int64_t byte_index) {
    if (!XT_IS_REAL_PTR(str_val)) return XT_FROM_INT(0);
    XTObject* obj = (XTObject*)str_val;
    if (obj->type_id != XT_TYPE_STRING) return XT_FROM_INT(0);
    
    XTString* s = (XTString*)str_val;
    if (byte_index < 0 || (size_t)byte_index >= s->length) return XT_FROM_INT(0);
    
    unsigned char b = (unsigned char)s->data[byte_index];
    return XT_FROM_INT((int64_t)b);
}

 


XTValue xt_string_byte_length(XTValue str_val) {
    if (!XT_IS_REAL_PTR(str_val)) return XT_FROM_INT(0);
    XTObject* obj = (XTObject*)str_val;
    if (obj->type_id != XT_TYPE_STRING) return XT_FROM_INT(0);
    
    XTString* s = (XTString*)str_val;
    return XT_FROM_INT((int64_t)s->length);
}

 


XTValue xt_string_char_count(XTValue str_val) {
    if (!XT_IS_REAL_PTR(str_val)) return XT_FROM_INT(0);
    XTObject* obj = (XTObject*)str_val;
    if (obj->type_id != XT_TYPE_STRING) return XT_FROM_INT(0);
    
    XTString* s = (XTString*)str_val;
    const char* p = s->data;
    int64_t count = 0;
    while (*p) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x80) p += 1;
        else if ((c & 0xE0) == 0xC0) p += 2;
        else if ((c & 0xF0) == 0xE0) p += 3;
        else if ((c & 0xF8) == 0xF0) p += 4;
        else p += 1;
        count++;
    }
    return XT_FROM_INT(count);
}

 




XTValue xt_string_to_hex_string(XTValue str_val) {
    if (!XT_IS_REAL_PTR(str_val)) return XT_NULL;
    XTString* s = (XTString*)str_val;

    size_t new_len = s->length * 3;
    char* buf = (char*)malloc(new_len + 1);
    char* p = buf;
    const char* hex = "0123456789ABCDEF";

    for (size_t i = 0; i < s->length; i++) {
        unsigned char b = (unsigned char)s->data[i];
        *p++ = '\\';
        *p++ = hex[b >> 4];
        *p++ = hex[b & 0x0F];
    }
    *p = '\0';

    XTString* res = xt_string_new_len(buf, new_len);
    free(buf);
    return (XTValue)res;
}

 


XTString* xt_string_next_char(XTString* s, int64_t* offset) {
    if (!s || *offset >= (int64_t)s->length) return xt_string_new("");
    unsigned char* d = (unsigned char*)s->data + *offset;
    int len = 1;
    if (*d >= 0xf0) len = 4;
    else if (*d >= 0xe0) len = 3;
    else if (*d >= 0xc0) len = 2;
    
    if (*offset + len > (int64_t)s->length) len = (int)(s->length - *offset);
    
    char buf[5] = {0};
    memcpy(buf, d, len);
    *offset += len;
    return xt_string_new(buf);
}

 


void xt_print_string(XTString* str) {
    if (!str) { printf("空\n"); return; }
    printf("%s\n", str->data);
}

void xt_print_bool(int val) {
    printf("%s\n", val ? "真" : "假");
}

void xt_print_float(double val) {
    printf("%g\n", val);
}



 


XTArena* xt_arena_new(size_t size) {
    XTArena* arena = (XTArena*)malloc(sizeof(XTArena));
    if (!arena) return NULL;
    
    
    atomic_init(&arena->header.ref_count, XT_REF_COUNT_IMMORTAL);
    arena->header.type_id = XT_TYPE_ARENA;
    arena->header.magic = XT_MAGIC;

    arena->buffer = (char*)calloc(1, size);
    if (!arena->buffer) { free(arena); return NULL; }
    arena->size = size;
    arena->offset = 0;
    arena->next = NULL;
    return arena;
}

 


void* xt_arena_alloc_raw(size_t size) {
    if (!g_current_arena) return malloc(size);
    
    
    size = (size + 7) & ~7;
    
    
    if (g_current_arena->offset + size > g_current_arena->size) {
        size_t next_size = (size > 100 * 1024 * 1024) ? size : 100 * 1024 * 1024;
        XTArena* new_block = xt_arena_new(next_size);
        if (!new_block) { fprintf(stderr, "Fatal error: out of memory (Arena raw expand)\n"); exit(1); }
        
        
        
        
        
        char* old_buffer = g_current_arena->buffer;
        size_t old_size = g_current_arena->size;
        size_t old_offset = g_current_arena->offset;
        
        g_current_arena->buffer = new_block->buffer;
        g_current_arena->size = new_block->size;
        g_current_arena->offset = 0;
        
        new_block->buffer = old_buffer;
        new_block->size = old_size;
        new_block->offset = old_offset;
        
        
        new_block->next = g_current_arena->next;
        g_current_arena->next = new_block;
    }
    
    void* ptr = g_current_arena->buffer + g_current_arena->offset;
    g_current_arena->offset += size;
    return ptr;
}

 


void* xt_arena_alloc(size_t size, uint32_t type_id) {
    void* ptr = xt_arena_alloc_raw(size);
    XTObject* obj = (XTObject*)ptr;
    
    atomic_init(&obj->ref_count, XT_REF_COUNT_IMMORTAL); 
    obj->type_id = type_id;
    obj->magic = XT_MAGIC;
    return ptr;
}

 


XTValue xt_arena_use(XTArena* arena) {
    g_current_arena = arena;
    return XT_NULL;
}

XTArena* xt_arena_disable(void) {
    XTArena* old = g_current_arena;
    g_current_arena = NULL;
    return old;
}

void xt_arena_restore(XTArena* arena) {
    g_current_arena = arena;
}

 







XTValue xt_arena_destroy(XTArena* arena) {
    if (!arena) return XT_NULL;
    
    
    
    if (g_current_arena == arena) {
        g_current_arena = NULL;
    }

    
    XTArena* curr = arena->next;
    while (curr) {
        XTArena* next = curr->next;
        if (curr->buffer) {
            free(curr->buffer);
            curr->buffer = NULL;
        }
        free(curr);
        curr = next;
    }
    arena->next = NULL;

    
    
    
    atomic_store(&arena->header.ref_count, 2);

    return XT_NULL;
}

 




void* xt_malloc(size_t size, uint32_t type_id) {
    XTObject* obj;
    if (g_current_arena) {
        obj = (XTObject*)xt_arena_alloc(size, type_id);
    } else {
        obj = (XTObject*)malloc(size);
        if (obj) {
            atomic_init(&obj->ref_count, 1);
            obj->type_id = type_id;
        }
    }
    if (obj) {
        obj->magic = XT_MAGIC;
    } else {
        fprintf(stderr, "Fatal error: out of memory (xt_malloc)\n");
        exit(1);
    }
    return (void*)obj;
}

static inline void xt_check_obj(void* val) {
    if (!xt_is_real_ptr((XTValue)val)) return;
    XTObject* obj = (XTObject*)val;
    if (obj->magic != XT_MAGIC) {
        fprintf(stderr, "运行时错误: 检测到堆损坏或非法指针访问 (Addr=%p Type=%08x Magic=%08x)\n", val, obj->type_id, obj->magic);
        fprintf(stderr, "  值低8字节(hex): ");
        for (int di = 0; di < 32 && di < (int)sizeof(XTObject); di++) {
            fprintf(stderr, "%02x ", ((unsigned char*)val)[di]);
        }
        fprintf(stderr, "\n");
        
        #ifdef _WIN32
        {
            void* stack[16];
            unsigned short frames = CaptureStackBackTrace(0, 16, stack, NULL);
            fprintf(stderr, "  栈回溯 (%u 帧):\n", frames);
            for (unsigned short fi = 0; fi < frames; fi++) {
                fprintf(stderr, "    [%u] %p\n", fi, stack[fi]);
            }
        }
        #endif
        exit(-1);
    }
}

 




void xt_weak_init(XTValue* slot_addr, XTValue obj_val) {
    if (!xt_is_real_ptr(obj_val) || obj_val == XT_NULL) return;
    XTObject* obj = (XTObject*)obj_val;
    XTWeakSlot* ws = (XTWeakSlot*)malloc(sizeof(XTWeakSlot));
    if (!ws) return;
    ws->obj = (void*)obj;
    ws->slot_addr = slot_addr;
    WEAK_LOCK();
    ws->next = g_weak_slots;
    g_weak_slots = ws;
    WEAK_UNLOCK();
}

 


void xt_dict_set_weak(XTValue dict_val, XTValue key, XTValue value) {
    if (!XT_IS_REAL_PTR(dict_val)) return;
    XTObject* obj = (XTObject*)dict_val;
    if (obj->type_id != XT_TYPE_DICT && obj->type_id != XT_TYPE_INSTANCE) return;
    XTDict* dict = (XTDict*)dict_val;

    uint64_t hash = xt_hash_value(key);
    size_t idx = hash % dict->capacity;

    XTDictEntry* entry = dict->buckets[idx];
    while (entry) {
        if (xt_eq(entry->key, key)) {
            entry->value = value;  
            return;
        }
        entry = entry->next;
    }
    
    XTDictEntry* new_entry = (XTDictEntry*)malloc(sizeof(XTDictEntry));
    if (!new_entry) return;
    new_entry->key = key; new_entry->value = value;
    new_entry->next = dict->buckets[idx];
    dict->buckets[idx] = new_entry;
    dict->size++;
    xt_retain(key);   
    
}

 


void xt_dict_weak_init(XTValue dict_val, XTValue key, XTValue obj_val) {
    if (!xt_is_real_ptr(obj_val) || obj_val == XT_NULL) return;
    XTObject* obj = (XTObject*)obj_val;
    XTWeakSlot* ws = (XTWeakSlot*)malloc(sizeof(XTWeakSlot));
    if (!ws) return;
    ws->obj = (void*)obj;
    ws->slot_addr = NULL;
    ws->dict_val = dict_val;
    ws->dict_key = key;
    xt_retain(key);
    WEAK_LOCK();
    ws->next = g_weak_slots;
    g_weak_slots = ws;
    WEAK_UNLOCK();
}

 


static void xt_weak_clear(XTObject* obj) {
    XTWeakSlot** p = &g_weak_slots;
    while (*p) {
        XTWeakSlot* ws = *p;
        if (ws->obj == (void*)obj) {
            if (ws->slot_addr) {
                if (*ws->slot_addr == (XTValue)obj) { *ws->slot_addr = XT_NULL; }
            } else {
                xt_dict_set_weak(ws->dict_val, ws->dict_key, XT_NULL);
                xt_release(ws->dict_key);
            }
            *p = ws->next;
            free(ws);
        } else {
            p = &ws->next;
        }
    }
}

 




static void xt_free_obj(XTObject* obj) {
    if (!obj) return;


    
    if (atomic_load(&obj->ref_count) >= XT_REF_COUNT_IMMORTAL) return;

    
    xt_weak_clear(obj);

    switch (obj->type_id) {
        case XT_TYPE_STRING: {
            XTString* s = (XTString*)obj;
            
            if (s->data && !s->data_in_arena) free(s->data);
            break;
        }
        case XT_TYPE_ARRAY: {
            XTArray* arr = (XTArray*)obj;
            
            for (size_t i = 0; i < arr->length; i++) {
                xt_release((XTValue)arr->elements[i]);
            }
            if (arr->elements) free(arr->elements);
            break;
        }
        case XT_TYPE_DICT: {
            XTDict* dict = (XTDict*)obj;
            
            for (size_t i = 0; i < dict->capacity; i++) {
                XTDictEntry* entry = dict->buckets[i];
                while (entry) {
                    XTDictEntry* next = entry->next;
                    xt_release(entry->key);
                    xt_release(entry->value);
                    free(entry);
                    entry = next;
                }
            }
            if (dict->buckets) free(dict->buckets);
            break;
        }
        case XT_TYPE_INSTANCE: {
            XTInstance* inst = (XTInstance*)obj;
            
            for (size_t i = 0; i < inst->capacity; i++) {
                XTDictEntry* entry = inst->buckets[i];
                while (entry) {
                    xt_release(entry->key);
                    xt_release(entry->value);
                    XTDictEntry* next = entry->next;
                    free(entry);
                    entry = next;
                }
            }
            if (inst->buckets) free(inst->buckets);
            break;
        }
        case XT_TYPE_RESULT: {
            XTResult* res = (XTResult*)obj;
            if (res->value) xt_release((XTValue)res->value);
            if (res->error) xt_release((XTValue)res->error);
            break;
        }
        case XT_TYPE_CHANNEL: {
            XTChannel* chan = (XTChannel*)obj;
            XT_CHAN_MUTEX_DESTROY(&chan->mu);
            XT_CHAN_COND_DESTROY(&chan->recv_cv);
            XT_CHAN_COND_DESTROY(&chan->send_cv);
            if (chan->buffer) {
                for (size_t i = 0; i < chan->size; i++) {
                    size_t idx = (chan->head + i) % chan->capacity;
                    xt_release(chan->buffer[idx]);
                }
                free(chan->buffer);
            }
            break;
        }
        case XT_TYPE_SOCKET: {
            XTSocket* s = (XTSocket*)obj;
            xt_net_close_obj(s);
            break;
        }
        case XT_TYPE_BYTES: {
            XTBytes* bytes = (XTBytes*)obj;
            if (bytes->data && !bytes->header.type_id) { 
                
            }
            
            
            if (bytes->data) {
                
                if (atomic_load(&bytes->header.ref_count) < XT_REF_COUNT_IMMORTAL) {
                    free(bytes->data);
                }
            }
            break;
        }
        case XT_TYPE_TASK: {
            XTTask* task = (XTTask*)obj;
            if (task->result != XT_NULL) xt_release(task->result);
            break;
        }
        case XT_TYPE_FUNCTION:
            
            break;
        case XT_TYPE_ARENA: {
            XTArena* arena = (XTArena*)obj;
            
            XTArena* curr = arena->next;
            while (curr) {
                XTArena* next = curr->next;
                if (curr->buffer) free(curr->buffer);
                free(curr);
                curr = next;
            }
            if (arena->buffer) free(arena->buffer);
            break;
        }
        default:
            break;
    }
    
    free(obj); 
}

 




static int xt_is_real_ptr(XTValue val) {
    
    return XT_IS_REAL_PTR(val);
}

 








void xt_retain(XTValue val) {
    if (XT_IS_INT(val)) return;
    if (xt_is_real_ptr(val)) {
        xt_check_obj((void*)val);
        XTObject* obj = (XTObject*)val;
        if (atomic_load_explicit(&obj->ref_count, memory_order_relaxed) >= XT_REF_COUNT_IMMORTAL) return;
        atomic_fetch_add_explicit(&obj->ref_count, 1, memory_order_relaxed);
    }
}

void xt_release(XTValue val) {
    if (XT_IS_INT(val)) return;
    if (xt_is_real_ptr(val)) {
        xt_check_obj((void*)val);
        XTObject* obj = (XTObject*)val;
        if (atomic_load_explicit(&obj->ref_count, memory_order_relaxed) >= XT_REF_COUNT_IMMORTAL) return;

        
        uint32_t old_ref = atomic_fetch_sub(&obj->ref_count, 1);
        if (old_ref == 1) {
            xt_free_obj(obj);
        }
    }
}

void xt_retain_forever(XTValue val) {
    if (!xt_is_real_ptr(val)) return;
    XTObject* obj = (XTObject*)val;
    atomic_store(&obj->ref_count, XT_REF_COUNT_IMMORTAL);
}



 


int64_t xt_to_int(XTValue val) {
    if (XT_IS_INT(val)) return XT_TO_INT(val);
    if (val == XT_TRUE) return 1;
    if (val == XT_FALSE) return 0;
    if (val == XT_NULL) return 0;
    if (XT_IS_REAL_PTR(val)) {
        XTObject* obj = (XTObject*)val;
        if (obj->type_id == XT_TYPE_INT) return ((XTInt*)val)->value;
        if (obj->type_id == XT_TYPE_FLOAT) return (int64_t)((struct { XTObject h; double v; }*)val)->v;
        if (obj->type_id == XT_TYPE_STRING) return atoll(((XTString*)val)->data); 
    }
    return 0;
}

XTValue xt_convert_to_int(XTValue val) {
    return XT_FROM_INT(xt_to_int(val));
}

 


XTValue xt_convert_to_float(XTValue val) {
    double d = 0.0;
    if (XT_IS_INT(val)) d = (double)XT_TO_INT(val);
    else if (val == XT_TRUE) d = 1.0;
    else if (XT_IS_REAL_PTR(val)) {
        XTObject* obj = (XTObject*)val;
        if (obj->type_id == XT_TYPE_FLOAT) d = ((struct { XTObject h; double v; }*)val)->v;
        else if (obj->type_id == XT_TYPE_INT) d = (double)((XTInt*)val)->value;
        else if (obj->type_id == XT_TYPE_STRING) d = atof(((XTString*)val)->data);
    }
    return (XTValue)xt_float_new(d);
}

XTValue xt_convert_to_string(XTValue val) {
    return (XTValue)xt_obj_to_string(val);
}



 


XTValue xt_array_new(size_t capacity) {
    XTArray* arr = (XTArray*)xt_malloc(sizeof(XTArray), XT_TYPE_ARRAY);
    arr->length = 0;
    arr->capacity = capacity > 0 ? capacity : 4; 
    
    if (g_current_arena) {
        arr->elements = (void**)xt_arena_alloc_raw(sizeof(void*) * arr->capacity);
        arr->elements_in_arena = 1;
    } else {
        arr->elements = (void**)malloc(sizeof(void*) * arr->capacity);
        arr->elements_in_arena = 0;
    }
    return (XTValue)arr;
}

 


void xt_array_append(XTValue arr_val, XTValue element) {
    if (!XT_IS_REAL_PTR(arr_val)) return;
    XTArray* arr = (XTArray*)arr_val;
    
    if (arr->length >= arr->capacity) {
        size_t new_capacity = arr->capacity == 0 ? 4 : arr->capacity * 2;
        void** new_elements;
        if (g_current_arena) {
            new_elements = (void**)xt_arena_alloc_raw(sizeof(void*) * new_capacity);
            if (arr->elements) memcpy(new_elements, arr->elements, sizeof(void*) * arr->length);
            
        } else {
            if (arr->elements && !arr->elements_in_arena) {
                new_elements = (void**)realloc(arr->elements, sizeof(void*) * new_capacity);
            } else {
                
                new_elements = (void**)malloc(sizeof(void*) * new_capacity);
                if (arr->elements) memcpy(new_elements, arr->elements, sizeof(void*) * arr->length);
            }
            arr->elements_in_arena = 0;
        }
        if (!new_elements) return;
        arr->elements = new_elements;
        arr->capacity = new_capacity;
    }
    xt_retain(element); 
    arr->elements[arr->length++] = (void*)element;
}

 


XTValue xt_array_get(XTValue arr_val, XTValue index_val) {
    if (!XT_IS_REAL_PTR(arr_val)) return XT_NULL;
    XTArray* arr = (XTArray*)arr_val;
    int64_t index = xt_to_int(index_val);
    if (index < 0 || (size_t)index >= arr->length) return XT_NULL;
    return (XTValue)arr->elements[index];
}

 


XTValue xt_array_pop(XTArray* arr) {
    if (!arr || arr->header.type_id != XT_TYPE_ARRAY || arr->length == 0) return XT_NULL;
    arr->length--;
    XTValue val = (XTValue)arr->elements[arr->length];
    arr->elements[arr->length] = NULL; 
    return val;
}

 


void xt_array_set(XTValue arr_val, XTValue index_val, XTValue value) {
    if (!XT_IS_REAL_PTR(arr_val)) return;
    XTArray* arr = (XTArray*)arr_val;
    int64_t index = xt_to_int(index_val);
    if (index < 0 || (size_t)index >= arr->length) return;
    
    xt_release((XTValue)arr->elements[index]); 
    arr->elements[index] = (void*)value;
    xt_retain(value); 
}

 


void xt_array_remove(XTValue arr_val, XTValue index_val) {
    if (!XT_IS_REAL_PTR(arr_val)) return;
    XTArray* arr = (XTArray*)arr_val;
    int64_t index = xt_to_int(index_val);
    if (index < 0 || (size_t)index >= arr->length) return;
    xt_release((XTValue)arr->elements[index]);
    for (size_t i = (size_t)index; i < arr->length - 1; i++) {
        arr->elements[i] = arr->elements[i+1];
    }
    arr->length--;
}

 


void xt_array_insert(XTValue arr_val, XTValue index_val, XTValue value) {
    if (!XT_IS_REAL_PTR(arr_val)) return;
    XTArray* arr = (XTArray*)arr_val;
    int64_t index = xt_to_int(index_val);
    if (index < 0 || (size_t)index > arr->length) return;
    
    xt_array_append(arr_val, value);
    for (size_t i = arr->length - 1; i > (size_t)index; i--) {
        void* temp = arr->elements[i];
        arr->elements[i] = arr->elements[i-1];
        arr->elements[i-1] = temp;
    }
}

 


XTValue xt_array_contains(XTValue arr_val, XTValue element) {
    if (!XT_IS_REAL_PTR(arr_val)) return XT_FALSE;
    XTArray* arr = (XTArray*)arr_val;
    for (size_t i = 0; i < arr->length; i++) {
        if (xt_compare((XTValue)arr->elements[i], element) == 0) return XT_TRUE;
    }
    return XT_FALSE;
}

 


XTValue xt_array_find(XTValue arr_val, XTValue element) {
    if (!XT_IS_REAL_PTR(arr_val)) return XT_FROM_INT(-1);
    XTArray* arr = (XTArray*)arr_val;
    for (size_t i = 0; i < arr->length; i++) {
        if (xt_compare((XTValue)arr->elements[i], element) == 0) return XT_FROM_INT(i);
    }
    return XT_FROM_INT(-1);
}

 


XTValue xt_array_slice(XTValue arr_val, XTValue start_val) {
    if (!XT_IS_REAL_PTR(arr_val)) return xt_array_new(0);
    XTArray* arr = (XTArray*)arr_val;
    int64_t start = xt_to_int(start_val);
    if (start < 0) start = 0;
    if ((size_t)start >= arr->length) return xt_array_new(0);
    
    size_t new_len = arr->length - (size_t)start;
    XTValue new_arr_val = xt_array_new(new_len);
    for (size_t i = 0; i < new_len; i++) {
        xt_array_append(new_arr_val, (XTValue)arr->elements[start + i]);
    }
    return new_arr_val;
}

 


XTValue xt_array_range(XTValue start_val, XTValue end_val) {
    int64_t start = xt_to_int(start_val);
    int64_t end = xt_to_int(end_val);
    if (start >= end) return xt_array_new(0);
    
    size_t len = (size_t)(end - start);
    XTValue new_arr_val = xt_array_new(len);
    for (int64_t i = start; i < end; i++) {
        xt_array_append(new_arr_val, XT_FROM_INT(i));
    }
    return new_arr_val;
}



 


XTInstance* xt_instance_new(void* class_ptr, size_t field_count) {
    XTInstance* inst = (XTInstance*)xt_malloc(sizeof(XTInstance), XT_TYPE_INSTANCE);
    inst->class_ptr = class_ptr;
    
    
    inst->capacity = 8;
    inst->size = 0;
    inst->buckets = (XTDictEntry**)malloc(sizeof(XTDictEntry*) * inst->capacity);
    memset(inst->buckets, 0, sizeof(XTDictEntry*) * inst->capacity);
    
    return inst;
}

 


void* xt_result_new(int is_success, void* value, void* error) {
    XTResult* res = (XTResult*)xt_malloc(sizeof(XTResult), XT_TYPE_RESULT);
    res->is_success = is_success;
    res->value = value;
    res->error = error;
    if (value) xt_retain((XTValue)value);
    if (error) xt_retain((XTValue)error);
    return (void*)res;
}

 


XTValue xt_func_new(void* func_ptr) {
    XTFunction* obj = (XTFunction*)xt_malloc(sizeof(XTFunction), XT_TYPE_FUNCTION);
    obj->func_ptr = func_ptr;
    obj->env = NULL;
    return (XTValue)obj;
}




static XTValue xt_socket_read_method(XTValue self) {
    if (!xt_is_real_ptr(self)) return XT_NULL;
    XTSocket* s = (XTSocket*)self;
    if (s->is_closed) return XT_NULL;
    char* data = (char*)xt_net_read(s, 4096);
    if (!data) return XT_NULL;
    XTString* str = xt_string_new(data);
    free(data);
    return (XTValue)str;
}


static XTValue xt_socket_write_method(XTValue self, XTValue msg) {
    if (!xt_is_real_ptr(self)) return XT_NULL;
    const char* text = "";
    if (xt_is_real_ptr(msg)) {
        XTObject* o = (XTObject*)msg;
        if (o->type_id == XT_TYPE_STRING) text = ((XTString*)msg)->data;
    }
    XTSocket* s = (XTSocket*)self;
    int rc = xt_net_write(s, text, (int)strlen(text));
    return XT_FROM_BOOL(rc > 0);
}


static XTValue xt_socket_close_method(XTValue self) {
    if (!xt_is_real_ptr(self)) return XT_NULL;
    xt_net_close_obj((XTSocket*)self);
    return XT_TRUE;
}


static XTValue xt_socket_tostring_method(XTValue self) {
    return (XTValue)xt_string_new("Socket连接");
}


void xt_socket_register_methods(XTSocket* s) {
    
    
    
    (void)s;
}




static XTValue xt_result_then_method(XTValue self, XTValue callback) {
    XTResult* r = (XTResult*)self;
    if (r->is_success) {
        
        typedef XTValue (*xt_cb)(XTValue);
        xt_cb cb = (xt_cb)((XTFunction*)callback)->func_ptr;
        return cb((XTValue)r->value);
    }
    return self; 
}


static XTValue xt_result_else_method(XTValue self, XTValue callback) {
    XTResult* r = (XTResult*)self;
    if (!r->is_success) {
        typedef XTValue (*xt_cb)(XTValue);
        xt_cb cb = (xt_cb)((XTFunction*)callback)->func_ptr;
        return cb((XTValue)r->error);
    }
    return self;
}


static void dict_set_method(XTValue obj, const char* name, void* func_ptr) {
    XTValue key = (XTValue)xt_string_new(name);
    XTValue fn  = xt_func_new(func_ptr);
    xt_dict_set(obj, key, fn);
    xt_release(key);
    xt_release(fn);
}



 


XTString* xt_string_substring(XTString* s, int64_t start, int64_t end) {
    if (!s) return xt_string_new("");
    
    const char* p = s->data;
    int64_t current = 0;
    const char* start_p = NULL;
    const char* end_p = NULL;
    
    while (*p) {
        if (current == start) start_p = p;
        if (current == end) { end_p = p; break; }
        
        unsigned char c = (unsigned char)*p;
        if (c < 0x80) p += 1;
        else if ((c & 0xE0) == 0xC0) p += 2;
        else if ((c & 0xF0) == 0xE0) p += 3;
        else if ((c & 0xF8) == 0xF0) p += 4;
        else p += 1;
        current++;
    }
    
    if (start_p && !end_p) end_p = p;
    if (!start_p) return xt_string_new("");
    
    size_t len = end_p - start_p;
    char* buf = (char*)malloc(len + 1);
    memcpy(buf, start_p, len);
    buf[len] = '\0';
    XTString* res = xt_string_new(buf);
    free(buf);
    return res;
}

 


XTString* xt_array_join(XTValue arr_val, XTString* sep) {
    if (!XT_IS_REAL_PTR(arr_val)) return xt_string_new("");
    XTArray* arr = (XTArray*)arr_val;
    if (arr->length == 0) return xt_string_new("");
    
    size_t total_len = 0;
    size_t sep_len = sep ? sep->length : 0;
    
    
    for (size_t i = 0; i < arr->length; i++) {
        XTString* s = xt_obj_to_string((XTValue)arr->elements[i]);
        total_len += s->length;
        if (i < arr->length - 1) total_len += sep_len;
        xt_release((XTValue)s);
    }
    
    
    char* buf = (char*)malloc(total_len + 1);
    char* p = buf;
    for (size_t i = 0; i < arr->length; i++) {
        XTString* s = xt_obj_to_string((XTValue)arr->elements[i]);
        memcpy(p, s->data, s->length);
        p += s->length;
        if (i < arr->length - 1 && sep) {
            memcpy(p, sep->data, sep->length);
            p += sep->length;
        }
        xt_release((XTValue)s);
    }
    *p = '\0';
    
    XTString* res = xt_string_new(buf);
    free(buf);
    return res;
}

int xt_string_contains(XTString* s, XTString* sub) {
    if (!s || !sub) return 0;
    return strstr(s->data, sub->data) != NULL;
}

 


XTString* xt_string_concat(XTString* s1, XTString* s2) {
    size_t len1 = s1 ? s1->length : 0;
    size_t len2 = s2 ? s2->length : 0;
    size_t total_len = len1 + len2;
    
    char* data = (char*)malloc(total_len + 1);
    if (len1 > 0) memcpy(data, s1->data, len1);
    if (len2 > 0) memcpy(data + len1, s2->data, len2);
    data[total_len] = '\0';
    
    XTString* res = xt_string_new_len(data, total_len);
    free(data);
    return res;
}

 


void xt_print_value(XTValue val) {
    XTString* s = xt_obj_to_string(val);
    if (s) {
        printf("%s\n", s->data);
        fflush(stdout); 
        xt_release((XTValue)s);
    }
}

XTString* xt_int_to_string(int64_t val) {
    char buf[32];
    sprintf(buf, "%lld", val);
    return xt_string_new(buf);
}

XTString* xt_float_to_string(double val) {
    char buf[64];
    sprintf(buf, "%g", val);
    return xt_string_new(buf);
}

 


XTString* xt_obj_to_string(XTValue val) {
    if (XT_IS_INT(val)) return xt_int_to_string(XT_TO_INT(val));
    if (val == XT_TRUE) return xt_string_new("真");
    if (val == XT_FALSE) return xt_string_new("假");
    if (val == XT_NULL) return xt_string_new("空");

    if (!xt_is_real_ptr(val)) return xt_string_new("非法地址");

    XTObject* header = (XTObject*)val;
    switch (header->type_id) {
        case XT_TYPE_INT: 
            return xt_int_to_string(((XTInt*)val)->value);
        case XT_TYPE_STRING:
            xt_retain(val);
            return (XTString*)val;
        case XT_TYPE_FLOAT:
            return xt_float_to_string(((struct { XTObject h; double v; }*)val)->v);
        case XT_TYPE_BOOL:
            return xt_string_new(((XTInt*)val)->value ? "真" : "假");
        case XT_TYPE_INSTANCE:
            return xt_string_new("实例对象");
        case XT_TYPE_RESULT: {
            XTResult* r = (XTResult*)val;
            XTString* prefix = r->is_success ? xt_string_new("成功(") : xt_string_new("失败(");
            XTString* inner = xt_obj_to_string((XTValue)(r->is_success ? r->value : r->error));
            XTString* suffix = xt_string_new(")");
            XTString* res1 = xt_string_concat(prefix, inner);
            XTString* res2 = xt_string_concat(res1, suffix);
            xt_release((XTValue)prefix); xt_release((XTValue)inner);
            xt_release((XTValue)suffix); xt_release((XTValue)res1);
            return res2;
        }
        case XT_TYPE_DICT: return xt_string_new("字典对象");
        case XT_TYPE_ARRAY: return xt_string_new("数组对象");
        case XT_TYPE_SOCKET: return xt_string_new("Socket连接");
        case XT_TYPE_CHANNEL: return xt_string_new("通道对象");
        default: return xt_string_new("未知对象");
    }
}



 


static uint64_t xt_hash_value(XTValue val) {
    if (XT_IS_INT(val)) return (uint64_t)XT_TO_INT(val);
    if (val == XT_TRUE) return 4;
    if (val == XT_FALSE) return 2;
    if (val == XT_NULL) return 0;
    if (!xt_is_real_ptr(val)) return (uint64_t)val;

    XTObject* obj = (XTObject*)val;
    if (obj->type_id == XT_TYPE_STRING) {
        XTString* s = (XTString*)val;
        uint64_t hash = 5381;
        for (size_t i = 0; i < s->length; i++) {
            hash = ((hash << 5) + hash) + (unsigned char)s->data[i];
        }
        return hash;
    }
    return (uint64_t)val; 
}

 


int xt_compare(XTValue a, XTValue b) {
    if (a == b) return 0;
    if (XT_IS_INT(a) && XT_IS_INT(b)) {
        int64_t ia = XT_TO_INT(a); int64_t ib = XT_TO_INT(b);
        return (ia < ib) ? -1 : 1;
    }
    if (XT_IS_REAL_PTR(a) && XT_IS_REAL_PTR(b)) {
        XTObject* oa = (XTObject*)a; XTObject* ob = (XTObject*)b;
        if (oa->type_id == XT_TYPE_STRING && ob->type_id == XT_TYPE_STRING) {
            return strcmp(((XTString*)a)->data, ((XTString*)b)->data);
        }
    }
    return (a < b) ? -1 : 1;
}

 


XTValue xt_dict_new(size_t capacity) {
    if (capacity < 8) capacity = 8;
    XTDict* dict = (XTDict*)xt_malloc(sizeof(XTDict), XT_TYPE_DICT);
    dict->capacity = capacity;
    dict->size = 0;
    dict->buckets = (XTDictEntry**)calloc(capacity, sizeof(XTDictEntry*));
    return (XTValue)dict;
}

 


void xt_dict_set(XTValue dict_val, XTValue key, XTValue value) {
    if (!XT_IS_REAL_PTR(dict_val)) return;
    XTObject* obj = (XTObject*)dict_val;
    if (obj->type_id != XT_TYPE_DICT && obj->type_id != XT_TYPE_INSTANCE) return;
    XTDict* dict = (XTDict*)dict_val;
    
    uint64_t hash = xt_hash_value(key);
    size_t idx = hash % dict->capacity;

    XTDictEntry* entry = dict->buckets[idx];
    while (entry) {
        if (xt_eq(entry->key, key)) {
            xt_release(entry->value);
            entry->value = value;
            xt_retain(value);
            return;
        }
        entry = entry->next;
    }

    XTDictEntry* new_entry = (XTDictEntry*)malloc(sizeof(XTDictEntry));
    new_entry->key = key; new_entry->value = value;
    new_entry->next = dict->buckets[idx];
    dict->buckets[idx] = new_entry;
    dict->size++;
    xt_retain(key); xt_retain(value);
}

 


XTValue xt_dict_get(XTValue dict_val, XTValue key) {
    if (!XT_IS_REAL_PTR(dict_val)) return XT_NULL;
    XTObject* obj = (XTObject*)dict_val;

    
    if (obj->type_id == XT_TYPE_SOCKET) {
        if (!xt_is_real_ptr(key)) return XT_NULL;
        const char* m = ((XTString*)key)->data;
        if (strcmp(m, "读") == 0 || strcmp(m, "read") == 0) return xt_func_new((void*)xt_socket_read_method);
        if (strcmp(m, "写") == 0 || strcmp(m, "发") == 0 || strcmp(m, "write") == 0 || strcmp(m, "send") == 0) return xt_func_new((void*)xt_socket_write_method);
        if (strcmp(m, "关") == 0 || strcmp(m, "close") == 0) return xt_func_new((void*)xt_socket_close_method);
        if (strcmp(m, "转字") == 0 || strcmp(m, "toString") == 0) return xt_func_new((void*)xt_socket_tostring_method);
        return XT_NULL;
    }
    if (obj->type_id == XT_TYPE_RESULT) {
        if (!xt_is_real_ptr(key)) return XT_NULL;
        const char* m = ((XTString*)key)->data;
        if (strcmp(m, "接着") == 0 || strcmp(m, "then") == 0) return xt_func_new((void*)xt_result_then_method);
        if (strcmp(m, "否则") == 0 || strcmp(m, "else") == 0) return xt_func_new((void*)xt_result_else_method);
        return XT_NULL;
    }

    if (obj->type_id != XT_TYPE_DICT && obj->type_id != XT_TYPE_INSTANCE) return XT_NULL;
    XTDict* dict = (XTDict*)dict_val;
    if (dict->capacity == 0) return XT_NULL;
    uint64_t hash = xt_hash_value(key);
    size_t idx = hash % dict->capacity;

    XTDictEntry* entry = dict->buckets[idx];
    while (entry) {
        if (xt_eq(entry->key, key)) return entry->value;
        entry = entry->next;
    }
    return XT_NULL;
}

size_t xt_dict_size(XTValue dict_val) {
    if (!XT_IS_REAL_PTR(dict_val)) return 0;
    XTObject* obj = (XTObject*)dict_val;
    if (obj->type_id != XT_TYPE_DICT && obj->type_id != XT_TYPE_INSTANCE) return 0;
    return ((XTDict*)dict_val)->size;
}

size_t xt_array_length(XTValue arr_val) {
    if (!XT_IS_REAL_PTR(arr_val)) return 0;
    XTObject* obj = (XTObject*)arr_val;
    if (obj->type_id != XT_TYPE_ARRAY) return 0;
    return ((XTArray*)arr_val)->length;
}

int xt_dict_contains(XTValue dict_val, XTValue key) {
    return xt_dict_get(dict_val, key) != XT_NULL;
}

 


XTValue xt_get_member(XTValue obj_val, XTValue key_val) {
    if (!XT_IS_REAL_PTR(obj_val)) return XT_NULL;
    XTObject* obj = (XTObject*)obj_val;

    
    if (obj->type_id == XT_TYPE_DICT || obj->type_id == XT_TYPE_INSTANCE) {
        return xt_dict_get(obj_val, key_val);
    }

    
    if (obj->type_id == XT_TYPE_SOCKET) {
        if (!xt_is_real_ptr(key_val)) return XT_NULL;
        const char* method = ((XTString*)key_val)->data;
        if (strcmp(method, "读") == 0 || strcmp(method, "read") == 0)
            return xt_func_new((void*)xt_socket_read_method);
        if (strcmp(method, "写") == 0 || strcmp(method, "发") == 0 || strcmp(method, "write") == 0 || strcmp(method, "send") == 0)
            return xt_func_new((void*)xt_socket_write_method);
        if (strcmp(method, "关") == 0 || strcmp(method, "close") == 0)
            return xt_func_new((void*)xt_socket_close_method);
        return XT_NULL;
    }

    
    if (obj->type_id == XT_TYPE_RESULT) {
        if (!xt_is_real_ptr(key_val)) return XT_NULL;
        const char* method = ((XTString*)key_val)->data;
        if (strcmp(method, "接着") == 0 || strcmp(method, "then") == 0)
            return xt_func_new((void*)xt_result_then_method);
        if (strcmp(method, "否则") == 0 || strcmp(method, "else") == 0)
            return xt_func_new((void*)xt_result_else_method);
        return XT_NULL;
    }

    return XT_NULL;
}

XTValue xt_dict_keys(XTValue dict_val) {
    if (!XT_IS_REAL_PTR(dict_val)) return XT_NULL;
    XTObject* obj = (XTObject*)dict_val;
    if (obj->type_id != XT_TYPE_DICT && obj->type_id != XT_TYPE_INSTANCE) return XT_NULL;
    XTDict* dict = (XTDict*)dict_val;
    XTValue arr = xt_array_new(dict->size);
    for (size_t i = 0; i < dict->capacity; i++) {
        XTDictEntry* entry = dict->buckets[i];
        while (entry) { xt_array_append(arr, entry->key); entry = entry->next; }
    }
    return arr;
}

XTValue xt_dict_values(XTValue dict_val) {
    if (!XT_IS_REAL_PTR(dict_val)) return XT_NULL;
    XTDict* dict = (XTDict*)dict_val;
    XTValue arr = xt_array_new(dict->size);
    for (size_t i = 0; i < dict->capacity; i++) {
        XTDictEntry* entry = dict->buckets[i];
        while (entry) { xt_array_append(arr, entry->value); entry = entry->next; }
    }
    return arr;
}

void xt_dict_remove(XTValue dict_val, XTValue key) {
    if (!XT_IS_REAL_PTR(dict_val)) return;
    XTDict* dict = (XTDict*)dict_val;
    uint64_t hash = xt_hash_value(key) % dict->capacity;
    XTDictEntry* entry = dict->buckets[hash];
    XTDictEntry* prev = NULL;
    while (entry) {
        if (xt_compare(entry->key, key) == 0) {
            if (prev) prev->next = entry->next; else dict->buckets[hash] = entry->next;
            xt_release(entry->key); xt_release(entry->value);
            free(entry); dict->size--; return;
        }
        prev = entry; entry = entry->next;
    }
}

int xt_eq(XTValue a, XTValue b) {
    return xt_compare(a, b) == 0;
}



#ifdef _WIN32
static wchar_t* xt_utf8_to_utf16(const char* utf8_str) {
    if (!utf8_str) return NULL;
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8_str, -1, NULL, 0);
    if (len <= 0) return NULL;
    wchar_t* wstr = (wchar_t*)malloc(len * sizeof(wchar_t));
    if (wstr) {
        MultiByteToWideChar(CP_UTF8, 0, utf8_str, -1, wstr, len);
    }
    return wstr;
}

static char* xt_utf16_to_utf8(const wchar_t* wstr) {
    if (!wstr) return NULL;
    int len = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, NULL, 0, NULL, NULL);
    if (len <= 0) return NULL;
    char* utf8_str = (char*)malloc(len);
    if (utf8_str) {
        WideCharToMultiByte(CP_UTF8, 0, wstr, -1, utf8_str, len, NULL, NULL);
    }
    return utf8_str;
}
#endif

 


XTValue xt_file_read(XTValue path_val) {
    if (!XT_IS_REAL_PTR(path_val)) return (XTValue)xt_result_new(0, NULL, (void*)xt_string_new("路径无效"));
    XTObject* obj = (XTObject*)path_val;
    if (obj->type_id != XT_TYPE_STRING) return (XTValue)xt_result_new(0, NULL, (void*)xt_string_new("路径无效"));
    XTString* path = (XTString*)path_val;
    FILE* f = NULL;
#ifdef _WIN32
    wchar_t* wpath = xt_utf8_to_utf16(path->data);
    if (wpath) {
        f = _wfopen(wpath, L"rb");
        free(wpath);
    }
#else
    f = fopen(path->data, "rb");
#endif
    if (!f) return (XTValue)xt_result_new(0, NULL, (void*)xt_string_new("无法打开文件"));

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return (XTValue)xt_result_new(0, NULL, (void*)xt_string_new("无法获取文件大小"));
    }
    long size = ftell(f);
    if (size < 0) {
        fclose(f);
        return (XTValue)xt_result_new(0, NULL, (void*)xt_string_new("无法获取文件大小"));
    }
    rewind(f);

    char* buf = (char*)malloc(size + 1);
    if (!buf) {
        fclose(f);
        return (XTValue)xt_result_new(0, NULL, (void*)xt_string_new("内存不足"));
    }
    size_t read = fread(buf, 1, size, f);
    fclose(f);
    buf[read] = '\0';

    XTString* content = xt_string_new_len(buf, read);
    free(buf);
    return (XTValue)xt_result_new(1, (void*)content, NULL);
}

 


XTValue xt_file_write(XTValue path_val, XTValue content_val) {
    if (!xt_is_real_ptr(path_val)) return (XTValue)xt_result_new(0, NULL, (void*)xt_string_new("路径无效"));
    XTObject* obj = (XTObject*)path_val;
    if (obj->type_id != XT_TYPE_STRING) return (XTValue)xt_result_new(0, NULL, (void*)xt_string_new("路径无效"));
    XTString* path = (XTString*)path_val;
    if (!path->data) return (XTValue)xt_result_new(0, NULL, (void*)xt_string_new("路径数据为空"));

    XTString* content = xt_obj_to_string(content_val);
    if (!content || !content->data) {
        if (content) xt_release((XTValue)content);
        return (XTValue)xt_result_new(0, NULL, (void*)xt_string_new("内容转换失败"));
    }

    FILE* f = NULL;
#ifdef _WIN32
    wchar_t* wpath = xt_utf8_to_utf16(path->data);
    if (wpath) {
        f = _wfopen(wpath, L"wb");
        free(wpath);
    }
#else
    f = fopen(path->data, "wb");
#endif
    if (!f) { 
        xt_release((XTValue)content); 
        return (XTValue)xt_result_new(0, NULL, (void*)xt_string_new("无法写入文件 (打开失败)")); 
    }
    fwrite(content->data, 1, content->length, f);
    fclose(f);
    xt_release((XTValue)content);
    return (XTValue)xt_result_new(1, (void*)XT_TRUE, NULL);
}

XTValue xt_file_exists(XTValue path_val) {
    if (!xt_is_real_ptr(path_val)) return XT_FALSE;
    XTObject* obj = (XTObject*)path_val;
    if (obj->type_id != XT_TYPE_STRING) return XT_FALSE;
    XTString* path = (XTString*)path_val;
    if (!path->data) return XT_FALSE;

#ifdef _WIN32
    wchar_t* wpath = xt_utf8_to_utf16(path->data);
    if (wpath) {
        struct _stat64 st;
        int res = _wstat64(wpath, &st);
        free(wpath);
        return res == 0 ? XT_TRUE : XT_FALSE;
    }
#else
    struct stat st;
    return stat(path->data, &st) == 0 ? XT_TRUE : XT_FALSE;
#endif
    return XT_FALSE;
}



XTValue xt_bytes_new(size_t capacity) {
    XTBytes* b = (XTBytes*)xt_malloc(sizeof(XTBytes), XT_TYPE_BYTES);
    b->data = g_current_arena ? (uint8_t*)xt_arena_alloc_raw(capacity) : (uint8_t*)malloc(capacity);
    b->length = 0; b->capacity = capacity;
    return (XTValue)b;
}

void xt_bytes_append(XTValue bytes_val, uint8_t b_val) {
    if (XT_IS_INT(bytes_val)) return;
    XTBytes* b = (XTBytes*)bytes_val;
    if (b->length >= b->capacity) {
        size_t new_capacity = b->capacity * 2;
        if (atomic_load(&b->header.ref_count) >= XT_REF_COUNT_IMMORTAL) {
            uint8_t* new_data = (uint8_t*)xt_arena_alloc_raw(new_capacity);
            memcpy(new_data, b->data, b->length); b->data = new_data;
        } else {
            b->data = realloc(b->data, new_capacity);
        }
        b->capacity = new_capacity;
    }
    b->data[b->length++] = b_val;
}



XTValue xt_task_new(XTValue result) {
    XTTask* t = (XTTask*)xt_malloc(sizeof(XTTask), XT_TYPE_TASK);
    t->result = result;
    if (result != XT_NULL) xt_retain(result);
    t->status = 1;
    t->pool_id = -1;
    return (XTValue)t;
}

XTValue xt_wait(XTValue task_val) {
    if (!xt_is_real_ptr(task_val)) return XT_NULL;
    XTTask* t = (XTTask*)task_val;
    return xt_async_wait((XTValue)(uintptr_t)t);
}


typedef struct {
    void* func_ptr;
    XTValue arg;
    XTTask* task;  
} async_ctx;

static void* async_runner(void* p) {
    async_ctx* ctx = (async_ctx*)p;
    typedef XTValue (*xt_func)(XTValue);
    xt_func f = (xt_func)ctx->func_ptr;
    XTValue result = f(ctx->arg);
    ctx->task->result = result;
    ctx->task->status = 1;
    xt_async_notify_complete(ctx->task);
    free(ctx);
    return (void*)result;
}

void xt_async_notify_complete(XTTask* task) {
    if (g_scheduler && task) {
        xt_scheduler_wake_task(task);
    }
}

XTTask* xt_async_spawn(void* func_ptr, XTValue arg) {
    XTTask* t = (XTTask*)xt_malloc(sizeof(XTTask), XT_TYPE_TASK);
    t->result = XT_NULL;
    t->status = 0;  
    async_ctx* ctx = (async_ctx*)malloc(sizeof(async_ctx));
    ctx->func_ptr = func_ptr;
    ctx->arg = arg;
    ctx->task = t;
    t->pool_id = xt_threadpool_submit(async_runner, ctx);
    return t;
}

XTValue xt_async_wait(XTValue task) {
    
    if (task >= 1 && task <= 0x10000 && g_scheduler) {
        int fid = (int)task - 1;
        if (fid >= 0 && fid < g_scheduler->fiber_count) {
            xt_scheduler_run();
            
            
            while (g_scheduler->fibers[fid].status != XT_FIBER_DONE
                   && !g_scheduler->fibers[fid].result_consumed) {
                _sched_sleep_us(1000);
            }
            
            XTFiber* fb = &g_scheduler->fibers[fid];
            XTValue r = fb->result;
            fb->result = 0;
            fb->result_consumed = 1;
            _xt_fiber_recycle(fid);
            return r;
        }
        return XT_NULL;
    }
    XTTask* t = (XTTask*)(uintptr_t)task;
    if (!t || t->pool_id < 0) return t ? t->result : XT_NULL;
    void* raw = xt_threadpool_wait(t->pool_id);
    t->result = (XTValue)raw;
    t->status = 1;
    return t->result;
}

int xt_async_try_wait(XTValue task) {
    
    if (task == XT_NULL) return 1;
    if (task >= 1 && task <= 0x10000 && g_scheduler) {
        int fid = (int)task - 1;
        if (fid >= 0 && fid < g_scheduler->fiber_count)
            return (g_scheduler->fibers[fid].status == XT_FIBER_DONE) ? 1 : 0;
        return 1;
    }
    XTTask* t = (XTTask*)task;
    if (!t || t->pool_id < 0 || t->status == 1) return 1;
    void* raw = xt_threadpool_try_wait(t->pool_id);
    if (raw == NULL) return 0;
    t->result = (XTValue)raw;
    t->status = 1;
    return 1;
}


int64_t xt_now_ms() {
    return _sched_now_us() / 1000;
}



XTValue xt_task_result(XTValue task) {
    if (task == XT_NULL) return XT_NULL;
    if (task >= 1 && task <= 0x10000 && g_scheduler) {
        int fid = (int)task - 1;
        if (fid >= 0 && fid < g_scheduler->fiber_count) {
            XTValue r = g_scheduler->fibers[fid].result;
            xt_retain(r);  
            g_scheduler->fibers[fid].result_consumed = 1;
            _xt_fiber_recycle(fid);
            return r;
        }
        return XT_NULL;
    }
    XTTask* t = (XTTask*)task;
    if (!t) return XT_NULL;
    xt_retain(t->result);
    return t->result;
}

XTValue xt_channel_new(size_t capacity) {
    XTChannel* c = (XTChannel*)xt_malloc(sizeof(XTChannel), XT_TYPE_CHANNEL);
    c->buffer = (XTValue*)calloc(capacity, sizeof(XTValue));
    c->size = 0; c->capacity = capacity; c->head = 0; c->tail = 0;
    XT_CHAN_MUTEX_INIT(&c->mu);
    XT_CHAN_COND_INIT(&c->recv_cv);
    XT_CHAN_COND_INIT(&c->send_cv);
    return (XTValue)c;
}

void xt_channel_send(XTValue chan_val, XTValue val) {
    if (XT_IS_INT(chan_val)) return;
    XTChannel* c = (XTChannel*)chan_val;
    XT_CHAN_MUTEX_LOCK(&c->mu);
    if (c->size >= c->capacity) { XT_CHAN_MUTEX_UNLOCK(&c->mu); return; }
    if (c->buffer[c->tail] != XT_NULL) {
        xt_release(c->buffer[c->tail]);
    }
    c->buffer[c->tail] = val;
    xt_retain(val);
    c->tail = (c->tail + 1) % c->capacity; c->size++;
    XT_CHAN_COND_SIGNAL(&c->recv_cv);  
    xt_scheduler_wake_task((void*)chan_val);  
    XT_CHAN_MUTEX_UNLOCK(&c->mu);
}

XTValue xt_channel_receive(XTValue chan_val) {
    if (XT_IS_INT(chan_val)) return XT_NULL;
    XTChannel* c = (XTChannel*)chan_val;
    XT_CHAN_MUTEX_LOCK(&c->mu);
    if (c->size == 0) { XT_CHAN_MUTEX_UNLOCK(&c->mu); return XT_NULL; }
    XTValue val = c->buffer[c->head];
    c->buffer[c->head] = XT_NULL;
    c->head = (c->head + 1) % c->capacity; c->size--;
    XT_CHAN_COND_SIGNAL(&c->send_cv);  
    xt_scheduler_wake_task((void*)chan_val);  
    XT_CHAN_MUTEX_UNLOCK(&c->mu);
    return val;
}


XTValue xt_channel_receive_blocking(XTValue chan_val, int timeout_ms) {
    if (XT_IS_INT(chan_val)) return XT_NULL;
    XTChannel* c = (XTChannel*)chan_val;
    XT_CHAN_MUTEX_LOCK(&c->mu);
    while (c->size == 0) {
        if (timeout_ms == 0) { XT_CHAN_MUTEX_UNLOCK(&c->mu); return XT_NULL; }
        
        
        if (timeout_ms < 0 && g_scheduler && !g_scheduler->running
            && g_main_thread_id != 0 && XT_THREAD_EQ(XT_THREAD_SELF(), g_main_thread_id)) {
            XT_CHAN_MUTEX_UNLOCK(&c->mu);
            xt_scheduler_run();
            XT_CHAN_MUTEX_LOCK(&c->mu);
            if (c->size > 0) break;
            
        }
        DWORD wait_ms = (timeout_ms < 0) ? INFINITE : (DWORD)timeout_ms;
        if (!XT_CHAN_COND_WAIT(&c->recv_cv, &c->mu, wait_ms)) {
            
            XT_CHAN_MUTEX_UNLOCK(&c->mu);
            return XT_NULL;
        }
        if (timeout_ms > 0) timeout_ms = 0;  
    }
    XTValue val = c->buffer[c->head];
    c->buffer[c->head] = XT_NULL;
    c->head = (c->head + 1) % c->capacity; c->size--;
    XT_CHAN_COND_SIGNAL(&c->send_cv);
    xt_scheduler_wake_task((void*)chan_val);  
    XT_CHAN_MUTEX_UNLOCK(&c->mu);
    return val;
}


int xt_channel_send_blocking(XTValue chan_val, XTValue val, int timeout_ms) {
    if (XT_IS_INT(chan_val)) return 0;
    XTChannel* c = (XTChannel*)chan_val;
    XT_CHAN_MUTEX_LOCK(&c->mu);
    while (c->size >= c->capacity) {
        if (timeout_ms == 0) { XT_CHAN_MUTEX_UNLOCK(&c->mu); return 0; }
        
        if (timeout_ms < 0 && g_scheduler && !g_scheduler->running
            && g_main_thread_id != 0 && XT_THREAD_EQ(XT_THREAD_SELF(), g_main_thread_id)) {
            XT_CHAN_MUTEX_UNLOCK(&c->mu);
            xt_scheduler_run();
            XT_CHAN_MUTEX_LOCK(&c->mu);
            if (c->size < c->capacity) break;
        }
        DWORD wait_ms = (timeout_ms < 0) ? INFINITE : (DWORD)timeout_ms;
        if (!XT_CHAN_COND_WAIT(&c->send_cv, &c->mu, wait_ms)) {
            XT_CHAN_MUTEX_UNLOCK(&c->mu);
            return 0;
        }
        if (timeout_ms > 0) timeout_ms = 0;
    }
    if (c->buffer[c->tail] != XT_NULL) xt_release(c->buffer[c->tail]);
    c->buffer[c->tail] = val;
    xt_retain(val);
    c->tail = (c->tail + 1) % c->capacity; c->size++;
    XT_CHAN_COND_SIGNAL(&c->recv_cv);
    xt_scheduler_wake_task((void*)chan_val);  
    XT_CHAN_MUTEX_UNLOCK(&c->mu);
    return 1;
}


int xt_channel_select(XTValue* channels, int count, int timeout_ms) {
    if (!channels || count <= 0) return -1;
    for (;;) {
        for (int i = 0; i < count; i++) {
            if (XT_IS_INT(channels[i])) continue;
            XTChannel* c = (XTChannel*)channels[i];
            XT_CHAN_MUTEX_LOCK(&c->mu);
            if (c->size > 0) {
                XT_CHAN_MUTEX_UNLOCK(&c->mu);
                return i;
            }
            XT_CHAN_MUTEX_UNLOCK(&c->mu);
        }
        if (timeout_ms == 0) return -1;
        for (int i = 0; i < count; i++) {
            if (XT_IS_INT(channels[i])) continue;
            XTChannel* c = (XTChannel*)channels[i];
            XT_CHAN_MUTEX_LOCK(&c->mu);
            if (c->size > 0) { XT_CHAN_MUTEX_UNLOCK(&c->mu); break; }
            DWORD wait_ms = (timeout_ms < 0) ? 100 : (DWORD)(timeout_ms < 100 ? timeout_ms : 100);
            XT_CHAN_COND_WAIT(&c->recv_cv, &c->mu, wait_ms);
            int had_data = (c->size > 0);
            XT_CHAN_MUTEX_UNLOCK(&c->mu);
            if (had_data) break;
        }
        if (timeout_ms > 0) {
            timeout_ms -= 100;
            if (timeout_ms <= 0) return -1;
        }
    }
}


int xt_channel_select_array(XTValue arr_val, int timeout_ms) {
    if (!xt_is_real_ptr(arr_val)) return -1;
    XTArray* arr = (XTArray*)arr_val;
    if (arr->length == 0) return -1;
    return xt_channel_select((XTValue*)arr->elements, (int)arr->length, timeout_ms);
}



XTValue xt_http_request(XTValue url_val) {
    if (!xt_is_real_ptr(url_val)) return (XTValue)xt_result_new(0, NULL, (void*)xt_string_new("URL无效"));
    XTString* url = (XTString*)url_val;
    void* result = xt_net_http_get(url->data);
    
    
    const char* resp = (const char*)result;
    if (!resp) return (XTValue)xt_result_new(0, NULL, (void*)xt_string_new("请求失败"));
    if (strncmp(resp, "不支持", 9) == 0 || strncmp(resp, "无法", 6) == 0 || strncmp(resp, "HTTP", 4) == 0 || strncmp(resp, "发送", 6) == 0) {
        XTString* err = xt_string_new(resp);
        free(result);
        return (XTValue)xt_result_new(0, NULL, (void*)err);
    }
    XTString* body = xt_string_new(resp);
    free(result);
    return (XTValue)xt_result_new(1, (void*)body, NULL);
}

XTValue xt_listen(XTValue port_val, XTValue callback_val) {
    int64_t port = xt_to_int(port_val);
    
    if (!xt_is_real_ptr(callback_val)) {
        return (XTValue)xt_result_new(0, NULL, (void*)xt_string_new("回调函数无效"));
    }
    typedef XTValue (*xt_cb)(XTValue);
    xt_cb cb = (xt_cb)((XTFunction*)callback_val)->func_ptr;
    int rc = xt_net_listen((int)port, (void (*)(void*))cb);
    if (rc < 0) {
        char err[64];
        snprintf(err, sizeof(err), "监听端口 %d 失败", (int)port);
        return (XTValue)xt_result_new(0, NULL, (void*)xt_string_new(err));
    }
    return (XTValue)xt_result_new(1, (void*)XT_TRUE, NULL);
}

XTValue xt_connect(XTValue addr_val) {
    if (!xt_is_real_ptr(addr_val)) return (XTValue)xt_result_new(0, NULL, (void*)xt_string_new("地址无效"));
    XTString* addr = (XTString*)addr_val;

    
    char host[256] = {0};
    int port = 80;
    const char* colon = strchr(addr->data, ':');
    if (colon) {
        size_t hlen = (size_t)(colon - addr->data);
        if (hlen >= sizeof(host)) hlen = sizeof(host) - 1;
        memcpy(host, addr->data, hlen);
        port = atoi(colon + 1);
    } else {
        size_t len = strlen(addr->data);
        if (len >= sizeof(host)) len = sizeof(host) - 1;
        memcpy(host, addr->data, len);
    }
    if (port <= 0) port = 80;

    void* sock = xt_net_connect(host, port);
    if (!sock) {
        char err[256];
        snprintf(err, sizeof(err), "无法连接到 %s", addr->data);
        return (XTValue)xt_result_new(0, NULL, (void*)xt_string_new(err));
    }
    return (XTValue)xt_result_new(1, sock, NULL);
}

XTValue xt_get_temp_path() {
#ifdef _WIN32
    wchar_t wpath[MAX_PATH];
    if (GetTempPathW(MAX_PATH, wpath) > 0) {
        char* path = xt_utf16_to_utf8(wpath);
        if (path) {
            
            size_t len = strlen(path);
            if (len > 0 && (path[len-1] == '\\' || path[len-1] == '/')) {
                path[len-1] = '\0';
            }
            XTString* s = xt_string_new(path);
            free(path);
            return (XTValue)s;
        }
    }
    return (XTValue)xt_string_new("C:\\temp");
#else
    const char* tmp = getenv("TMPDIR");
    if (!tmp) tmp = "/tmp";
    return (XTValue)xt_string_new(tmp);
#endif
}

 


XTValue xt_execute(XTValue cmd_val) {
    if (!XT_IS_REAL_PTR(cmd_val)) return (XTValue)xt_result_new(0, NULL, (void*)xt_string_new("指令无效"));
    XTObject* obj = (XTObject*)cmd_val;
    if (obj->type_id != XT_TYPE_STRING) return (XTValue)xt_result_new(0, NULL, (void*)xt_string_new("指令无效"));
    XTString* cmd = (XTString*)cmd_val;

#ifdef _WIN32
    
    
    char temp_path[MAX_PATH];
    char bat_path[MAX_PATH];
    DWORD dwRet = GetTempPathA(MAX_PATH, temp_path);
    if (dwRet == 0 || dwRet > MAX_PATH) {
        char err_msg[128];
        snprintf(err_msg, sizeof(err_msg), "获取临时路径失败, Error: %lu", GetLastError());
        return (XTValue)xt_result_new(0, NULL, (void*)xt_string_new(err_msg));
    }
    
    
    if (strlen(temp_path) + 10 >= MAX_PATH) {
        return (XTValue)xt_result_new(0, NULL, (void*)xt_string_new("临时路径过长"));
    }
    strcat(temp_path, "XuanTie");
    if (!CreateDirectoryA(temp_path, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
        char err_msg[256];
        snprintf(err_msg, sizeof(err_msg), "创建临时目录失败, Path: %s, Error: %lu", temp_path, GetLastError());
        return (XTValue)xt_result_new(0, NULL, (void*)xt_string_new(err_msg));
    }

    
    if (snprintf(bat_path, MAX_PATH, "%s\\xt_exec_%lu_%llu.bat", 
            temp_path, 
            GetCurrentProcessId(), 
            (unsigned long long)GetTickCount64()) >= MAX_PATH) {
        return (XTValue)xt_result_new(0, NULL, (void*)xt_string_new("临时批处理路径过长"));
    }
    
    FILE* fbat = NULL;
#ifdef _WIN32
    wchar_t* wbat_path_init = xt_utf8_to_utf16(bat_path);
    if (wbat_path_init) {
        fbat = _wfopen(wbat_path_init, L"w");
        free(wbat_path_init);
    }
#else
    fbat = fopen(bat_path, "w");
#endif

    if (!fbat) return (XTValue)xt_result_new(0, NULL, (void*)xt_string_new("创建临时批处理失败"));
    
    
    fprintf(fbat, "@echo off\n%s 2>&1\nexit /b %%ERRORLEVEL%%\n", cmd->data);
    fclose(fbat);

    
    wchar_t wcmd[1024]; 
    wchar_t* wbat = xt_utf8_to_utf16(bat_path);
    if (!wbat) {
        remove(bat_path);
        return (XTValue)xt_result_new(0, NULL, (void*)xt_string_new("路径转换失败"));
    }

    int required = _snwprintf(wcmd, 1024, L"\"\"%ls\"\"", wbat);
    free(wbat);

    if (required < 0 || required >= 1024) {
        remove(bat_path);
        return (XTValue)xt_result_new(0, NULL, (void*)xt_string_new("执行指令路径过长，超出了运行时缓冲区限制"));
    }

    FILE* pipe = _wpopen(wcmd, L"r");
    if (!pipe) {
        remove(bat_path);
        return (XTValue)xt_result_new(0, NULL, (void*)xt_string_new("执行管道打开失败"));
    }

    char buffer[1024]; 
    XTString* res = xt_string_new("");
    while (fgets(buffer, sizeof(buffer), pipe) != NULL) {
        XTString* temp = res;
        XTString* buf_str = xt_string_new(buffer);
        res = xt_string_concat(res, buf_str);
        xt_release((XTValue)temp);
        xt_release((XTValue)buf_str);
    }

    int status = _pclose(pipe);
    
    
    if (remove(bat_path) != 0) {
        
    }

    if (status != 0 && status != -1) {
        char err_msg[1024];
        snprintf(err_msg, sizeof(err_msg), "执行失败 (退出码: %d). 输出: %s", status, res->data);
        xt_release((XTValue)res);
        return (XTValue)xt_result_new(0, NULL, (void*)xt_string_new(err_msg));
    }
    return (XTValue)xt_result_new(1, (void*)res, NULL);

#else
    
    char cmd_with_stderr[2048];
    snprintf(cmd_with_stderr, sizeof(cmd_with_stderr), "%s 2>&1", cmd->data);
    FILE* pipe = popen(cmd_with_stderr, "r");
    if (!pipe) return (XTValue)xt_result_new(0, NULL, (void*)xt_string_new("执行失败"));
    
    char buffer[1024];
    XTString* res = xt_string_new("");
    while (fgets(buffer, sizeof(buffer), pipe) != NULL) {
        XTString* temp = res;
        XTString* buf_str = xt_string_new(buffer);
        res = xt_string_concat(res, buf_str);
        xt_release((XTValue)temp);
        xt_release((XTValue)buf_str);
    }
    int status = pclose(pipe);
    if (status != 0) {
        xt_release((XTValue)res);
        char err_msg[128];
        sprintf(err_msg, "执行失败，状态码: %d", status);
        return (XTValue)xt_result_new(0, NULL, (void*)xt_string_new(err_msg));
    }
    return (XTValue)xt_result_new(1, (void*)res, NULL);
#endif
}

 


XTValue xt_input(XTValue prompt_val) {
    if (XT_IS_REAL_PTR(prompt_val)) {
        XTString* prompt = (XTString*)prompt_val;
        printf("%s", prompt->data);
    }
    char buf[1024];
    if (fgets(buf, sizeof(buf), stdin)) {
        size_t len = strlen(buf);
        if (len > 0 && buf[len-1] == '\n') buf[len-1] = '\0';
        return (XTValue)xt_string_new(buf);
    }
    return (XTValue)xt_string_new("");
}

static double xt_to_double(XTValue val);

XTValue xt_math_random(XTValue max_val) {
    int64_t max = xt_to_int(max_val);
    if (max <= 0) max = 100;
    return XT_FROM_INT(rand() % max);
}

XTValue xt_math_abs(XTValue n_val) {
    if (XT_IS_INT(n_val)) {
        int64_t v = XT_TO_INT(n_val);
        
        if (v == INT64_MIN) return XT_FROM_INT(INT64_MAX);
        return XT_FROM_INT(v < 0 ? -v : v);
    }
    double d = xt_to_double(n_val);
    return (XTValue)xt_float_new(fabs(d));
}

XTValue xt_math_sin(XTValue n_val) {
    return (XTValue)xt_float_new(sin(xt_to_double(n_val)));
}

XTValue xt_math_cos(XTValue n_val) {
    return (XTValue)xt_float_new(cos(xt_to_double(n_val)));
}

XTValue xt_math_sqrt(XTValue n_val) {
    return (XTValue)xt_float_new(sqrt(xt_to_double(n_val)));
}

XTValue xt_math_floor(XTValue n_val) {
    return XT_FROM_INT((int64_t)floor(xt_to_double(n_val)));
}

XTValue xt_math_ceil(XTValue n_val) {
    return XT_FROM_INT((int64_t)ceil(xt_to_double(n_val)));
}

XTValue xt_math_round(XTValue n_val) {
    return XT_FROM_INT((int64_t)round(xt_to_double(n_val)));
}

XTValue xt_math_pow(XTValue base_val, XTValue exp_val) {
    return (XTValue)xt_float_new(pow(xt_to_double(base_val), xt_to_double(exp_val)));
}

XTValue xt_math_srand(XTValue seed_val) {
    srand((unsigned int)xt_to_int(seed_val));
    return XT_NULL;
}

XTValue xt_math_max(XTValue arr_val) {
    if (!XT_IS_REAL_PTR(arr_val) || ((XTObject*)arr_val)->type_id != XT_TYPE_ARRAY) return XT_FROM_INT(0);
    XTArray* arr = (XTArray*)arr_val;
    if (arr->length == 0) return XT_FROM_INT(0);
    
    XTValue max_val = (XTValue)arr->elements[0];
    double max_d = xt_to_double(max_val);
    
    for (size_t i = 1; i < arr->length; i++) {
        XTValue cur = (XTValue)arr->elements[i];
        double cur_d = xt_to_double(cur);
        if (cur_d > max_d) {
            max_d = cur_d;
            max_val = cur;
        }
    }
    xt_retain(max_val);
    return max_val;
}

XTValue xt_math_min(XTValue arr_val) {
    if (!XT_IS_REAL_PTR(arr_val) || ((XTObject*)arr_val)->type_id != XT_TYPE_ARRAY) return XT_FROM_INT(0);
    XTArray* arr = (XTArray*)arr_val;
    if (arr->length == 0) return XT_FROM_INT(0);
    
    XTValue min_val = (XTValue)arr->elements[0];
    double min_d = xt_to_double(min_val);
    
    for (size_t i = 1; i < arr->length; i++) {
        XTValue cur = (XTValue)arr->elements[i];
        double cur_d = xt_to_double(cur);
        if (cur_d < min_d) {
            min_d = cur_d;
            min_val = cur;
        }
    }
    xt_retain(min_val);
    return min_val;
}

XTValue xt_math_pi() {
    return (XTValue)xt_float_new(3.14159265358979323846);
}

XTValue xt_math_e() {
    return (XTValue)xt_float_new(2.71828182845904523536);
}

static double xt_to_double(XTValue val) {
    if (XT_IS_INT(val)) return (double)XT_TO_INT(val);
    if (val == XT_TRUE) return 1.0;
    if (val == XT_FALSE) return 0.0;
    if (XT_IS_REAL_PTR(val)) {
        XTObject* obj = (XTObject*)val;
        if (obj->type_id == XT_TYPE_FLOAT) {
            return ((struct { XTObject h; double v; }*)val)->v;
        }
        if (obj->type_id == XT_TYPE_INT) {
            return (double)((XTInt*)val)->value;
        }
    }
    return 0.0;
}

XTValue xt_time_now() {
    return XT_FROM_INT((int64_t)time(NULL));
}

 


XTValue xt_time_ms() {
#ifdef _WIN32
    static int initialized = 0;
    static LARGE_INTEGER frequency;
    if (!initialized) { QueryPerformanceFrequency(&frequency); initialized = 1; }
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return XT_FROM_INT((int64_t)(counter.QuadPart * 1000 / frequency.QuadPart));
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return XT_FROM_INT((int64_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000));
#endif
}

 


XTValue xt_time_micro() {
#ifdef _WIN32
    static int initialized = 0;
    static LARGE_INTEGER frequency;
    if (!initialized) { QueryPerformanceFrequency(&frequency); initialized = 1; }
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    
    return XT_FROM_INT((int64_t)(counter.QuadPart * 1000000 / frequency.QuadPart));
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return XT_FROM_INT((int64_t)(ts.tv_sec * 1000000 + ts.tv_nsec / 1000));
#endif
}

XTValue xt_time_sleep(XTValue ms_val) {
    int64_t ms = xt_to_int(ms_val);
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    usleep(ms * 1000);
#endif
    return XT_NULL;
}

 


XTValue xt_string_split(XTValue str_val, XTValue sep_val) {
    if (!XT_IS_REAL_PTR(str_val)) return XT_NULL;
    XTString* s = (XTString*)str_val; 
    XTString* sep = (XTString*)sep_val;
    XTValue arr = xt_array_new(4);
    
    if (sep && sep->length == 0) {
        
        int64_t char_count = XT_TO_INT(xt_string_char_count(str_val));
        for (int64_t i = 0; i < char_count; i++) {
            XTValue c = xt_string_get_char(str_val, i);
            xt_array_append(arr, c);
            xt_release(c);
        }
        return arr;
    }

    char* data = xt_strdup(s->data);
    char* token; char* rest = data;
    while ((token = strtok_s(rest, sep ? sep->data : "", &rest))) {
        XTValue s_new = (XTValue)xt_string_new(token);
        xt_array_append(arr, s_new);
        xt_release(s_new);
    }
    free(data); return arr;
}

 


XTValue xt_string_replace(XTValue str_val, XTValue old_val, XTValue new_val) {
    if (!XT_IS_REAL_PTR(str_val)) return XT_NULL;
    XTString* s = (XTString*)str_val;
    XTString* old_s = (XTString*)old_val;
    XTString* new_s = (XTString*)new_val;
    if (old_s->length == 0) { xt_retain(str_val); return str_val; }

    
    size_t count = 0;
    const char* scan = s->data;
    while ((scan = strstr(scan, old_s->data)) != NULL) { count++; scan += old_s->length; }
    if (count == 0) { xt_retain(str_val); return str_val; }

    
    size_t total_len = s->length + count * (new_s->length - old_s->length);
    char* buf = (char*)malloc(total_len + 1);
    if (!buf) return XT_NULL;

    const char* src = s->data;
    char* dst = buf;
    while (1) {
        const char* pos = strstr(src, old_s->data);
        if (!pos) { size_t rem = s->length - (size_t)(src - s->data); memcpy(dst, src, rem); dst += rem; break; }
        size_t pre = (size_t)(pos - src);
        memcpy(dst, src, pre); dst += pre;
        memcpy(dst, new_s->data, new_s->length); dst += new_s->length;
        src = pos + old_s->length;
    }
    buf[total_len] = '\0';
    XTString* res = xt_string_new_len(buf, total_len);
    free(buf); return (XTValue)res;
}

 


XTString* xt_json_serialize(XTValue val) {
    if (XT_IS_INT(val)) return xt_int_to_string(XT_TO_INT(val));
    if (val == XT_TRUE) return xt_string_new("true");
    if (val == XT_FALSE) return xt_string_new("false");
    if (val == XT_NULL) return xt_string_new("null");
    if (!XT_IS_REAL_PTR(val)) return xt_string_new("\"illegal\"");

    XTObject* header = (XTObject*)val;
    switch (header->type_id) {
        case XT_TYPE_STRING: {
            XTString* s = (XTString*)val;
            
            size_t esc_max = s->length * 6 + 3;
            char* buf = malloc(esc_max);
            if (!buf) return xt_string_new("\"\"");
            size_t w = 0;
            buf[w++] = '"';
            for (size_t i = 0; i < s->length; i++) {
                char c = s->data[i];
                switch (c) {
                    case '"':  buf[w++] = '\\'; buf[w++] = '"'; break;
                    case '\\': buf[w++] = '\\'; buf[w++] = '\\'; break;
                    case '\n': buf[w++] = '\\'; buf[w++] = 'n'; break;
                    case '\r': buf[w++] = '\\'; buf[w++] = 'r'; break;
                    case '\t': buf[w++] = '\\'; buf[w++] = 't'; break;
                    case '\b': buf[w++] = '\\'; buf[w++] = 'b'; break;
                    case '\f': buf[w++] = '\\'; buf[w++] = 'f'; break;
                    default:
                        if ((unsigned char)c < 0x20) {
                            w += sprintf(buf + w, "\\u%04x", (unsigned char)c);
                        } else {
                            buf[w++] = c;
                        }
                        break;
                }
            }
            buf[w++] = '"';
            buf[w] = '\0';
            XTString* res = xt_string_new_len(buf, w);
            free(buf); return res;
        }
        case XT_TYPE_ARRAY: {
            XTArray* arr = (XTArray*)val;
            XTString* res = xt_string_new("[");
            for (size_t i = 0; i < arr->length; i++) {
                XTString* item = xt_json_serialize((XTValue)arr->elements[i]);
                XTString* temp = xt_string_concat(res, item);
                xt_release((XTValue)res); xt_release((XTValue)item); res = temp;
                if (i < arr->length - 1) {
                    temp = xt_string_concat(res, xt_string_new(", "));
                    xt_release((XTValue)res); res = temp;
                }
            }
            XTString* suffix = xt_string_new("]");
            XTString* final_res = xt_string_concat(res, suffix);
            xt_release((XTValue)res); xt_release((XTValue)suffix); return final_res;
        }
        case XT_TYPE_DICT: {
            XTDict* dict = (XTDict*)val;
            XTString* res = xt_string_new("{");
            int first = 1;
            for (size_t i = 0; i < dict->capacity; i++) {
                XTDictEntry* entry = dict->buckets[i];
                while (entry) {
                    if (!first) {
                        XTString* comma = xt_string_new(", ");
                        XTString* temp = xt_string_concat(res, comma);
                        xt_release((XTValue)res); xt_release((XTValue)comma); res = temp;
                    }
                    XTString* key = xt_json_serialize(entry->key);
                    XTString* colon = xt_string_new(": ");
                    XTString* val_str = xt_json_serialize(entry->value);
                    XTString* temp1 = xt_string_concat(res, key);
                    XTString* temp2 = xt_string_concat(temp1, colon);
                    XTString* temp3 = xt_string_concat(temp2, val_str);
                    xt_release((XTValue)res); xt_release((XTValue)key); xt_release((XTValue)colon);
                    xt_release((XTValue)val_str); xt_release((XTValue)temp1); xt_release((XTValue)temp2);
                    res = temp3; first = 0; entry = entry->next;
                }
            }
            XTString* suffix = xt_string_new("}");
            XTString* final_res = xt_string_concat(res, suffix);
            xt_release((XTValue)res); xt_release((XTValue)suffix); return final_res;
        }
        default: return xt_string_new("\"object\"");
    }
}

 





static void json_skip_ws(const char** p) {
    while (**p == ' ' || **p == '\t' || **p == '\n' || **p == '\r') (*p)++;
}

static XTValue json_parse_value(const char** p);

static XTString* json_parse_string(const char** p) {
    if (**p != '"') return NULL;
    (*p)++;
    char buf[4096];
    size_t pos = 0;
    while (**p && **p != '"' && pos < sizeof(buf) - 1) {
        if (**p == '\\') {
            (*p)++;
            switch (**p) {
                case '"':  buf[pos++] = '"';  break;
                case '\\': buf[pos++] = '\\'; break;
                case '/':  buf[pos++] = '/';  break;
                case 'n':  buf[pos++] = '\n'; break;
                case 't':  buf[pos++] = '\t'; break;
                case 'r':  buf[pos++] = '\r'; break;
                case 'f':  buf[pos++] = '\f'; break;
                case 'b':  buf[pos++] = '\b'; break;
                case 'u': {
                    char hex[5] = {0};
                    int h = 0;
                    for (; h < 4 && **p; h++) hex[h] = *((*p)++);
                    if (h == 4) {
                        unsigned codepoint = (unsigned)strtol(hex, NULL, 16);
                        if (codepoint < 0x80) { buf[pos++] = (char)codepoint; }
                        else if (codepoint < 0x800) { buf[pos++] = (char)(0xC0 | (codepoint >> 6)); buf[pos++] = (char)(0x80 | (codepoint & 0x3F)); }
                        else { buf[pos++] = (char)(0xE0 | (codepoint >> 12)); buf[pos++] = (char)(0x80 | ((codepoint >> 6) & 0x3F)); buf[pos++] = (char)(0x80 | (codepoint & 0x3F)); }
                        (*p)--;
                    }
                    break;
                }
                default: buf[pos++] = **p; break;
            }
        } else {
            buf[pos++] = **p;
        }
        (*p)++;
    }
    if (**p == '"') (*p)++;
    buf[pos] = '\0';
    return xt_string_new(buf);
}

static XTValue json_parse_number(const char** p) {
    const char* start = *p;
    if (**p == '-') (*p)++;
    while (**p >= '0' && **p <= '9') (*p)++;
    if (**p == '.') {
        (*p)++;
        while (**p >= '0' && **p <= '9') (*p)++;
    }
    if (**p == 'e' || **p == 'E') {
        (*p)++;
        if (**p == '+' || **p == '-') (*p)++;
        while (**p >= '0' && **p <= '9') (*p)++;
    }
    size_t len = *p - start;
    if (len == 0 || (len == 1 && start[0] == '-')) { *p = start; return XT_NULL; }
    char num[128];
    if (len >= sizeof(num)) len = sizeof(num) - 1;
    memcpy(num, start, len); num[len] = '\0';
    return XT_FROM_INT((int64_t)atoll(num));
}

static XTValue json_parse_object(const char** p) {
    if (**p != '{') return XT_NULL;
    (*p)++;
    XTValue dict = xt_dict_new(16);
    json_skip_ws(p);
    if (**p == '}') { (*p)++; return dict; }
    while (1) {
        json_skip_ws(p);
        XTString* key = json_parse_string(p);
        if (!key) { xt_release(dict); return XT_NULL; }
        json_skip_ws(p);
        if (**p != ':') { xt_release((XTValue)key); xt_release(dict); return XT_NULL; }
        (*p)++;
        json_skip_ws(p);
        XTValue val = json_parse_value(p);
        xt_dict_set(dict, (XTValue)key, val);
        xt_release((XTValue)key);
        if (val != XT_NULL && !XT_IS_INT(val)) xt_release(val);
        json_skip_ws(p);
        if (**p == '}') { (*p)++; return dict; }
        if (**p != ',') { xt_release(dict); return XT_NULL; }
        (*p)++;
    }
}

static XTValue json_parse_array(const char** p) {
    if (**p != '[') return XT_NULL;
    (*p)++;
    XTValue arr = xt_array_new(16);
    json_skip_ws(p);
    if (**p == ']') { (*p)++; return arr; }
    while (1) {
        json_skip_ws(p);
        XTValue val = json_parse_value(p);
        xt_array_append(arr, val);
        if (val != XT_NULL && !XT_IS_INT(val)) xt_release(val);
        json_skip_ws(p);
        if (**p == ']') { (*p)++; return arr; }
        if (**p != ',') { xt_release(arr); return XT_NULL; }
        (*p)++;
    }
}

static XTValue json_parse_value(const char** p) {
    json_skip_ws(p);
    if (**p == '"') {
        XTString* s = json_parse_string(p);
        return s ? (XTValue)s : XT_NULL;
    }
    if ((**p >= '0' && **p <= '9') || **p == '-') return json_parse_number(p);
    if (**p == '{') return json_parse_object(p);
    if (**p == '[') return json_parse_array(p);
    if (strncmp(*p, "true", 4) == 0) { *p += 4; return XT_TRUE; }
    if (strncmp(*p, "false", 5) == 0) { *p += 5; return XT_FALSE; }
    if (strncmp(*p, "null", 4) == 0) { *p += 4; return XT_NULL; }
    return XT_NULL;
}

XTValue xt_json_deserialize(XTString* json_str) {
    if (!json_str || json_str->length == 0) return XT_NULL;
    const char* p = json_str->data;
    return json_parse_value(&p);
}



XTValue xt_add(XTValue a, XTValue b) {
    if (XT_IS_INT(a) && XT_IS_INT(b)) return XT_FROM_INT(XT_TO_INT(a) + XT_TO_INT(b));
    return XT_FROM_INT(xt_to_int(a) + xt_to_int(b));
}

XTValue xt_sub(XTValue a, XTValue b) {
    if (XT_IS_INT(a) && XT_IS_INT(b)) return XT_FROM_INT(XT_TO_INT(a) - XT_TO_INT(b));
    return XT_FROM_INT(xt_to_int(a) - xt_to_int(b));
}

XTValue xt_mul(XTValue a, XTValue b) {
    if (XT_IS_INT(a) && XT_IS_INT(b)) return XT_FROM_INT(XT_TO_INT(a) * XT_TO_INT(b));
    return XT_FROM_INT(xt_to_int(a) * xt_to_int(b));
}

XTValue xt_div(XTValue a, XTValue b) {
    int64_t vb = xt_to_int(b);
    if (vb == 0) return XT_FROM_INT(0);
    return XT_FROM_INT(xt_to_int(a) / vb);
}

XTValue xt_mod(XTValue a, XTValue b) {
    int64_t vb = xt_to_int(b);
    if (vb == 0) return XT_FROM_INT(0);
    return XT_FROM_INT(xt_to_int(a) % vb);
}

XTValue xt_bit_and(XTValue a, XTValue b) {
    return XT_FROM_INT(xt_to_int(a) & xt_to_int(b));
}

XTValue xt_bit_or(XTValue a, XTValue b) {
    return XT_FROM_INT(xt_to_int(a) | xt_to_int(b));
}

XTValue xt_bit_xor(XTValue a, XTValue b) {
    return XT_FROM_INT(xt_to_int(a) ^ xt_to_int(b));
}

XTValue xt_bit_shl(XTValue a, XTValue b) {
    return XT_FROM_INT(xt_to_int(a) << xt_to_int(b));
}

XTValue xt_bit_shr(XTValue a, XTValue b) {
    return XT_FROM_INT(xt_to_int(a) >> xt_to_int(b));
}
 
#include "xt_threadpool.c"
#include "xt_net.c"
#pragma comment(lib,"ws2_32")

 
static int _wsa_stub(void){ return 0; }
static void *_imp_stub_fns[] = {
 (void*)_wsa_stub
};
void *__imp_WSAStartup = (void*)_wsa_stub;
void *__imp_WSACleanup = (void*)_wsa_stub;
void *__imp_send = (void*)_wsa_stub;
void *__imp_recv = (void*)_wsa_stub;
void *__imp_closesocket = (void*)_wsa_stub;
void *__imp_socket = (void*)_wsa_stub;
void *__imp_bind = (void*)_wsa_stub;
void *__imp_listen = (void*)_wsa_stub;
void *__imp_accept = (void*)_wsa_stub;
void *__imp_connect = (void*)_wsa_stub;
void *__imp_htons = (void*)_wsa_stub;
void *__imp_select = (void*)_wsa_stub;
void *__imp_ioctlsocket = (void*)_wsa_stub;
void *__imp_setsockopt = (void*)_wsa_stub;
void *__imp_getsockopt = (void*)_wsa_stub;
void *__imp_shutdown = (void*)_wsa_stub;
void *__imp_getaddrinfo = (void*)_wsa_stub;
void *__imp_gethostbyname = (void*)_wsa_stub;
void *__imp_WSAGetLastError = (void*)_wsa_stub;
