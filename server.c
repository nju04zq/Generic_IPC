#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>

#include "util.h"
#include "generic_ipc.h"

#define IPC_MSG_MAX_LEN 511
#define SERVER_SLEEP_INTERVAL 2 // seconds

static pid_t client_pid = 0;

static void
compose_reply_msg (char *req_msg, char *reply_msg)
{
    char timestamp[TIMESTAMP_MAX_LEN+1];

    get_current_timestamp(timestamp);
    snprintf(reply_msg, IPC_MSG_MAX_LEN+1, "##Server reply at %s ==> %s##",
             timestamp, req_msg);
    return;
}

static void
compose_resp (char *req_msg, ipc_resp_t *resp_p)
{
    char reply_msg[IPC_MSG_MAX_LEN+1];
    uint32_t reply_msg_size;

    compose_reply_msg(req_msg, reply_msg);
    reply_msg_size = strlen(reply_msg) + 1;

    resp_p->resp_data = calloc(reply_msg_size, sizeof(char));
    if (!resp_p->resp_data) {
        return;
    }

    memcpy(resp_p->resp_data, reply_msg, reply_msg_size);
    resp_p->resp_data_size = reply_msg_size;
    return;
}

static void
get_client_pid (void)
{
    ipc_req_t req;
    ipc_resp_t resp;
    pid_t pid;
    int rc;

    pid = getpid();

    req.req_type = IPC_PAK_REQ_DATA;
    req.req_data = &pid;
    req.req_data_size = sizeof(pid_t);

    rc = generic_ipc_request(&req, &resp);
    if (rc != 0) {
        printf("Fail to send ipc request.");
        return;
    }

    if (resp.resp_data) {
        pid = *(pid_t *)resp.resp_data;
        client_pid = pid;
        printf("##Client PID %u\n", pid);
        free(resp.resp_data);
    } 
    return;
}

static void
server_hander (ipc_req_t *req_p, ipc_resp_t *resp_p)
{
    char *req_msg;

    req_msg = req_p->req_data;

    printf("##Server recieved msg size %d, %s\n",
           req_p->req_data_size, req_msg);

    if (client_pid == 0) {
        get_client_pid();
    }

    compose_resp(req_msg, resp_p);

    printf("##Server sleeping for %d seconds\n", SERVER_SLEEP_INTERVAL);
    sleep(SERVER_SLEEP_INTERVAL); // simulate a long time handling 
    return;
}

int
main (void)
{
    int rc;

    rc = generic_ipc_init(IPC_OWNER_SERVER, server_hander);
    if (rc != 0) {
        printf("Fail to init ipc.\n");
        return -1;
    }

    for (;;) {
        pause();
    }

    generic_ipc_clean(IPC_OWNER_SERVER);
    return 0;
}

