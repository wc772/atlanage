#ifndef XT_RUNTIME_H
#define XT_RUNTIME_H

#ifdef _WIN32
#define _WIN32_WINNT 0x0600
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

#ifdef _WIN32
#include <windows.h>
#endif

 







 





typedef struct {
    uint32_t magic;             
    _Atomic uint32_t ref_count; 
    uint32_t type_id;           
} XTObject;

#define XT_MAGIC 0x58544F42

 








typedef uintptr_t XTValue;


#define XT_TAG_INT      0x1ULL
#define XT_TAG_MASK     0x1ULL


#define XT_IS_INT(v)    (((v) & XT_TAG_MASK) == XT_TAG_INT)
#define XT_IS_PTR(v)    (!XT_IS_INT(v))




#define XT_IS_REAL_PTR(v)  (!XT_IS_INT(v) && (v) > 4096)


#define XT_FROM_INT(i)  (((XTValue)(i) << 1) | XT_TAG_INT)

#define XT_TO_INT(v)    ((int64_t)((intptr_t)(v) >> 1))

 




#define XT_NULL         ((XTValue)0x0ULL) 
#define XT_FALSE        ((XTValue)0x2ULL) 
#define XT_TRUE         ((XTValue)0x4ULL) 

#define XT_IS_BOOL(v)   ((v) == XT_TRUE || (v) == XT_FALSE)
#define XT_TO_BOOL(v)   ((v) == XT_TRUE)
#define XT_FROM_BOOL(b) ((b) ? XT_TRUE : XT_FALSE)

 


#define XT_TYPE_INT       1 
#define XT_TYPE_FLOAT     2 
#define XT_TYPE_STRING    3 
#define XT_TYPE_BOOL      4 
#define XT_TYPE_ARRAY     5 
#define XT_TYPE_DICT      6 
#define XT_TYPE_INSTANCE  7 
#define XT_TYPE_RESULT    8 
#define XT_TYPE_FUNCTION  9 
#define XT_TYPE_BYTES     10 
#define XT_TYPE_TASK      11 
#define XT_TYPE_CHANNEL   12 
#define XT_TYPE_ARENA     13 
#define XT_TYPE_SOCKET    14 


#define XT_REF_COUNT_IMMORTAL 0x7FFFFFFF 


typedef struct XTWeakSlot {
    void* obj;                  
    XTValue* slot_addr;         
    XTValue dict_val;           
    XTValue dict_key;           
    struct XTWeakSlot* next;    
} XTWeakSlot;


void xt_weak_init(XTValue* slot_addr, XTValue obj_val);

void xt_dict_set_weak(XTValue dict_val, XTValue key, XTValue value);

void xt_dict_weak_init(XTValue dict_val, XTValue key, XTValue obj_val);

 


typedef struct {
    XTObject header;
    void* func_ptr; 
    void* env;      
} XTFunction;

 


typedef struct {
    XTObject header;
    uint8_t* data;
    size_t length;
    size_t capacity;
} XTBytes;

 


typedef struct {
    XTObject header;
    XTValue result;
    int status;   
    int pool_id;  
} XTTask;


#include "xt_scheduler.h"

XTTask* xt_async_spawn(void* func_ptr, XTValue arg);
XTValue xt_async_wait(XTValue task);
void    xt_async_notify_complete(XTTask* task);

 


#if defined(_WIN32)
#include <windows.h>
typedef CRITICAL_SECTION xt_chan_mutex_t;
typedef CONDITION_VARIABLE xt_chan_cond_t;
#define XT_CHAN_MUTEX_INIT(m)   InitializeCriticalSection(m)
#define XT_CHAN_MUTEX_DESTROY(m) DeleteCriticalSection(m)
#define XT_CHAN_MUTEX_LOCK(m)    EnterCriticalSection(m)
#define XT_CHAN_MUTEX_UNLOCK(m)  LeaveCriticalSection(m)
#define XT_CHAN_COND_INIT(c)     InitializeConditionVariable(c)
#define XT_CHAN_COND_DESTROY(c)   
#define XT_CHAN_COND_WAIT(c,m,ms) SleepConditionVariableCS(c,m,ms)
#define XT_CHAN_COND_SIGNAL(c)   WakeConditionVariable(c)
#define XT_CHAN_COND_BROADCAST(c) WakeAllConditionVariable(c)
#else
#include <pthread.h>
typedef pthread_mutex_t xt_chan_mutex_t;
typedef pthread_cond_t  xt_chan_cond_t;
#define XT_CHAN_MUTEX_INIT(m)   pthread_mutex_init(m, NULL)
#define XT_CHAN_MUTEX_DESTROY(m) pthread_mutex_destroy(m)
#define XT_CHAN_MUTEX_LOCK(m)    pthread_mutex_lock(m)
#define XT_CHAN_MUTEX_UNLOCK(m)  pthread_mutex_unlock(m)
#define XT_CHAN_COND_INIT(c)     pthread_cond_init(c, NULL)
#define XT_CHAN_COND_DESTROY(c)  pthread_cond_destroy(c)
#define XT_CHAN_COND_WAIT(c,m,ms) ({ \
    struct timespec _ts; clock_gettime(CLOCK_REALTIME, &_ts); \
    _ts.tv_sec += (ms)/1000; _ts.tv_nsec += ((ms)%1000)*1000000; \
    if (_ts.tv_nsec >= 1000000000) { _ts.tv_sec++; _ts.tv_nsec -= 1000000000; } \
    pthread_cond_timedwait(c, m, &_ts); })
#define XT_CHAN_COND_SIGNAL(c)   pthread_cond_signal(c)
#define XT_CHAN_COND_BROADCAST(c) pthread_cond_broadcast(c)
#endif

typedef struct {
    XTObject header;
    XTValue* buffer;
    size_t size;
    size_t capacity;
    size_t head;
    size_t tail;
    xt_chan_mutex_t mu;
    xt_chan_cond_t  recv_cv;  
    xt_chan_cond_t  send_cv;  
} XTChannel;

 


typedef struct XTSocket {
    XTObject header;
    void* sock;       
    int is_closed;    
    int is_listener;  
} XTSocket;

 


typedef struct {
    XTObject header;
    int64_t value;
} XTInt;

 


typedef struct {
    XTObject header;
    char* data;           
    size_t length;        
    uint8_t data_in_arena; 
} XTString;

 


typedef struct {
    XTObject header;
    void** elements;          
    size_t length;            
    size_t capacity;          
    uint8_t elements_in_arena; 
} XTArray;

 


typedef struct XTDictEntry {
    XTValue key;
    XTValue value;
    struct XTDictEntry* next;
} XTDictEntry;

 


typedef struct {
    XTObject header;
    XTDictEntry** buckets; 
    size_t size;           
    size_t capacity;       
} XTDict;

 


typedef struct {
    XTObject header;
    XTDictEntry** buckets; 
    size_t size;           
    size_t capacity;       
    void* class_ptr;       
} XTInstance;

 


typedef struct {
    XTObject header;
    int32_t is_success; 
    int32_t padding;    
    void* value;        
    void* error;        
} XTResult;




void xt_init();


void xt_init_args(int argc, char** argv);
XTValue xt_get_args();


void xt_print_int(int64_t val);
void xt_print_string(XTString* str);
void xt_print_bool(int val);
void xt_print_float(double val);
void xt_print_value(XTValue val); 




XTValue xt_int_new(int64_t val);

void* xt_float_new(double val);

XTValue xt_bool_new(int val);

XTValue xt_func_new(void* func_ptr);


XTString* xt_string_new(const char* data);

XTString* xt_string_new_len(const char* data, size_t len);

XTString* xt_string_from_char(char c);
XTValue xt_string_get_char(XTValue str_val, int64_t index);
XTValue xt_string_get_byte(XTValue str_val, int64_t byte_index);
XTValue xt_string_byte_length(XTValue str_val);
XTValue xt_string_char_count(XTValue str_val);
XTValue xt_string_to_hex_string(XTValue str_val);
XTString* xt_string_next_char(XTString* s, int64_t* offset);


XTValue xt_array_new(size_t capacity);

void xt_array_append(XTValue arr_val, XTValue element);
void xt_array_set(XTValue arr_val, XTValue index_val, XTValue value);
void xt_array_remove(XTValue arr_val, XTValue index_val);
void xt_array_insert(XTValue arr_val, XTValue index_val, XTValue value);
XTValue xt_array_contains(XTValue arr_val, XTValue element);
XTValue xt_array_find(XTValue arr_val, XTValue element);
XTString* xt_array_join(XTValue arr_val, XTString* sep);
XTValue xt_array_slice(XTValue arr_val, XTValue start_val);
XTValue xt_array_range(XTValue start_val, XTValue end_val);


XTValue xt_dict_new(size_t capacity);

void xt_dict_set(XTValue dict_val, XTValue key, XTValue value);

XTValue xt_dict_get(XTValue dict_val, XTValue key);

int xt_dict_contains(XTValue dict_val, XTValue key);
void xt_dict_remove(XTValue dict_val, XTValue key);
XTValue xt_dict_keys(XTValue dict_val);
XTValue xt_dict_values(XTValue dict_val);
size_t xt_dict_size(XTValue dict_val);
size_t xt_array_length(XTValue arr_val);


int xt_compare(XTValue a, XTValue b);


void* xt_result_new(int is_success, void* value, void* error);


void* xt_malloc(size_t size, uint32_t type_id);




void xt_retain(XTValue val);

void xt_release(XTValue val);


typedef struct XTArena XTArena;
XTArena* xt_arena_new(size_t size);
void* xt_arena_alloc(size_t size, uint32_t type_id);
XTValue xt_arena_use(XTArena* arena);
XTValue xt_arena_destroy(XTArena* arena);
XTArena* xt_arena_disable(void);
void xt_arena_restore(XTArena* arena);
void xt_retain_forever(XTValue val);




int64_t xt_to_int(XTValue val);

XTValue xt_convert_to_int(XTValue val);

XTValue xt_convert_to_float(XTValue val);

XTValue xt_convert_to_string(XTValue val);


XTString* xt_string_concat(XTString* s1, XTString* s2);

XTString* xt_string_substring(XTString* s, int64_t start, int64_t end);

int xt_string_contains(XTString* s, XTString* sub);
XTValue xt_string_split(XTValue str_val, XTValue sep_val);
XTValue xt_string_replace(XTValue str_val, XTValue old_val, XTValue new_val);

XTString* xt_int_to_string(int64_t val);

XTString* xt_obj_to_string(XTValue val);


XTValue xt_bytes_new(size_t capacity);
void xt_bytes_append(XTValue bytes, uint8_t b);


XTValue xt_task_new(XTValue result);
XTValue xt_wait(XTValue task_val);


XTValue xt_channel_new(size_t capacity);
void xt_channel_send(XTValue chan_val, XTValue val);
XTValue xt_channel_receive(XTValue chan_val);

XTValue xt_channel_receive_blocking(XTValue chan_val, int timeout_ms);
int    xt_channel_send_blocking(XTValue chan_val, XTValue val, int timeout_ms);

int    xt_channel_select(XTValue* channels, int count, int timeout_ms);
int    xt_channel_select_array(XTValue arr_val, int timeout_ms);


struct XTSocket;
void xt_net_close_obj(struct XTSocket* s);


XTValue xt_http_request(XTValue url_val);
XTValue xt_listen(XTValue port_val, XTValue callback_val);
XTValue xt_connect(XTValue addr_val);
XTValue xt_execute(XTValue cmd_val);
XTValue xt_input(XTValue prompt_val);
XTValue xt_get_temp_path();




XTString* xt_json_serialize(XTValue val);

XTValue xt_json_deserialize(XTString* json_str);




XTValue xt_file_read(XTValue path);

XTValue xt_file_write(XTValue path, XTValue content);
XTValue xt_file_exists(XTValue path);


XTValue xt_add(XTValue a, XTValue b);
XTValue xt_sub(XTValue a, XTValue b);
XTValue xt_mul(XTValue a, XTValue b);
XTValue xt_div(XTValue a, XTValue b);
XTValue xt_mod(XTValue a, XTValue b);
XTValue xt_bit_and(XTValue a, XTValue b);
XTValue xt_bit_or(XTValue a, XTValue b);
XTValue xt_bit_xor(XTValue a, XTValue b);
XTValue xt_bit_shl(XTValue a, XTValue b);
XTValue xt_bit_shr(XTValue a, XTValue b);


int xt_eq(XTValue a, XTValue b);

#endif
