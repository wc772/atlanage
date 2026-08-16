

#ifndef XT_NET_H
#define XT_NET_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif


int xt_net_init(void);


void xt_net_cleanup(void);



void* xt_net_http_get(const char* url);



void* xt_net_connect(const char* host, int port);



int xt_net_listen(int port, void (*callback)(void* stream));


void* xt_net_read(void* sock_obj, int max_bytes);


int xt_net_write(void* sock_obj, const char* data, int len);


void xt_net_close(void* sock_obj);

#ifdef __cplusplus
}
#endif

#endif
