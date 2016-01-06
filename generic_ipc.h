#ifndef __GENERIC_IPC_H__
#define __GENERIC_IPC_H__

#include <stdint.h>

enum {
    IPC_OWNER_CLIENT = 0,
    IPC_OWNER_SERVER,
};

enum {
    IPC_PAK_MIN = 100,
    IPC_PAK_INIT,
    IPC_PAK_ACK,
    IPC_PAK_REQ_DATA,
    IPC_PAK_RESP,

    IPC_PAK_MAX,
};

typedef struct ipc_req_s {
    int req_type;
    void *req_data;
    uint32_t req_data_size;
} ipc_req_t;

typedef struct ipc_resp_s {
    void *resp_data;
    uint32_t resp_data_size;
} ipc_resp_t;

int
generic_ipc_request(ipc_req_t *ipc_req_p, ipc_resp_t *ipc_resp_p);

int
generic_ipc_init(int owner_id,
                 void (*handler)(ipc_req_t *, ipc_resp_t *));

void
generic_ipc_clean(int owner_id);
#endif //__GENERIC_IPC_H__
