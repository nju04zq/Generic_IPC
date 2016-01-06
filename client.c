#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "util.h"
#include "generic_ipc.h"

#define MAX_SEND_CNT 100
#define SEND_INTERVAL 1 //seconds
#define IPC_MSG_MAX_LEN 255

static void
client_hander (ipc_req_t *req_p, ipc_resp_t *resp_p)
{
    // Dummy one, server won't send msg in our case
    return;
}

static void
send_one_ipc_msg (char *ipc_msg)
{
    ipc_req_t req;
    ipc_resp_t resp;
    uint32_t ipc_msg_size;
    int rc;

    ipc_msg_size = strlen(ipc_msg) + 1;

    printf("##client send ipc msg size %d, msg %s\n", ipc_msg_size, ipc_msg);

    req.req_type = IPC_PAK_REQ_DATA;
    req.req_data = ipc_msg;
    req.req_data_size = ipc_msg_size;

    rc = generic_ipc_request(&req, &resp);
    if (rc != 0) {
        printf("Fail to send ipc request.");
        return;
    }

    printf("##client get resp size %d, resp %s\n",
           resp.resp_data_size, (char *)resp.resp_data);

    if (resp.resp_data) {
        free(resp.resp_data);
    }
    return;
}

static void
compose_ipc_msg (char *ipc_msg)
{
    char timestamp[TIMESTAMP_MAX_LEN+1];

    get_current_timestamp(timestamp);
    snprintf(ipc_msg, IPC_MSG_MAX_LEN+1, "##Client msg at %s##", timestamp);
    return;
}

static void
test_send (void)
{
    char ipc_msg[IPC_MSG_MAX_LEN+1];
    uint32_t i;

    for (i = 0; i < MAX_SEND_CNT; i++) {
        compose_ipc_msg(ipc_msg);
        send_one_ipc_msg(ipc_msg);
        sleep(SEND_INTERVAL);
    }
    return;
}

int
main (void)
{
    int rc;

    rc = generic_ipc_init(IPC_OWNER_CLIENT, client_hander);
    if (rc != 0) {
        printf("Fail to init ipc.\n");
        return -1;
    }

    test_send();

    generic_ipc_clean(IPC_OWNER_CLIENT);
    return 0;
}

