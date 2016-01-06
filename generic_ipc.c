/*
 * There is a client/server pair at the two ends of an IPC channel.
 *
 * When client request something to server, it should use below interface,
 * xxx_ipc_request (ipc_req_t *ipc_req_p, ipc_resp_t *ipc_resp_p),
 * ipc_req_p is the request sent from client to server,
 * if the ipc_req_p->req_data is malloc on heap, client side should free it
 * ipc_resp_p is the response back from server to client,
 * client side should free ipc_resp_p->resp_data
 *
 * When server handle request from client, it should register below interface,
 * void (*ipc_req_server)(ipc_req_t *, ipc_resp_t *),
 * If the server handler want to return data, it should malloc
 * ipc_resp_p->resp_data, and it will be freed in IPC infra in this file.
 * Also IPC infra will free the ipc_req_p->req_data
 *
 * NOTE: ipc_req_p->req_data must not be NULL
 *       ipc_resp_p->resp_data could be NULL, and peer side should know it
 */

#define _GNU_SOURCE
#include <time.h>
#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>
#include <stdbool.h>
#include <pthread.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/types.h>
#include "util.h"
#include "generic_ipc.h"

#define IPC_BUF_SIZE (4096*32) //128KB
#define IPC_BUF_DATA_MAX_SIZE (IPC_BUF_SIZE - sizeof(ipc_pak_t))

// For client, R/W/ACK/BUF, for server, W/R/ACK/BUF
#define SEM_CNT 4

#define WAIT_QUEUE_ENTRY_POOL_SIZE 20

#define WAIT_QUEUE_MAX_SIZE 100

#define IPC_ERROR_PAK_MAX_CNT 2000

#define __DEBUG_IPC__

#ifdef __DEBUG_IPC__
static FILE *__dbg_fp = NULL;

#define IPC_DBG_INIT(suffix_id) \
do {\
    char __dbg_file_path[256];\
    snprintf(__dbg_file_path, 256, ".ipc_dbg_%d", suffix_id);\
    __dbg_fp = fopen(__dbg_file_path, "w+");\
} while (0)

#define IPC_DBG_CLEAN() \
do {\
    if (__dbg_fp) {\
        fclose(__dbg_fp);\
    }\
} while (0)

#define IPC_DBG(FMT, args...) \
do {\
    char __timestamp[TIMESTAMP_MAX_LEN+1];\
    get_current_timestamp(__timestamp);\
    if (__dbg_fp) {\
        fprintf(__dbg_fp, "*%s: "FMT"\n", __timestamp, ##args);\
        fflush(__dbg_fp);\
    }\
    printf("*%s: "FMT"\n", __timestamp, ##args);\
} while (0)
#else // __DEBUG_IPC__
#define IPC_DBG_INIT(suffix_id)
#define IPC_DBG(FMT, args...)
#define IPC_DBG_CLEAN()
#endif

//! index for those 4 semphmores
enum {
    SEM_IDX_SERVER_0 = 0,
    SEM_IDX_SERVER_1,
    SEM_IDX_ACK,
    SEM_IDX_BUF,
};

union semun {
    int val;
    struct semid_ds *buf;
    unsigned short *array;
};

typedef struct wait_queue_s {
    queue_t queue;
    pthread_mutex_t queue_mutex;
    pthread_cond_t queue_cond;
} wait_queue_t;

typedef struct wait_queue_entry_s {
    queue_header_t header;
    uint32_t seq_num;
    int pak_type;
    void *data;
    uint32_t data_size;
    bool in_pool;
    bool free;
} wait_queue_entry_t;

typedef struct ipc_db_s {
    uint32_t next_seq_num;
    wait_queue_entry_t *entry_pool;
    uint32_t entry_cnt;
    uint32_t error_pak_cnt;
    bool terminate_flag;
} ipc_db_t;

typedef struct ipc_pak_s {
    int pak_type;
    uint32_t seq_num;
    uint32_t data_size;
    char data[0];
} ipc_pak_t;

static struct sembuf sem_op_ack_wait = {2, -1, SEM_UNDO};
static struct sembuf sem_op_ack_wakeup = {2, 1, SEM_UNDO};
static struct sembuf sem_op_buf_wait = {3, -1, SEM_UNDO};
static struct sembuf sem_op_buf_wakeup = {3, 1, SEM_UNDO};

// 250ms timeout for client ipc
static struct timespec ipc_timeout = {0, 250*1000*1000};

typedef struct ipc_ctx_s {
    bool init_done;

    pthread_mutex_t ipc_db_mutex;
    ipc_db_t ipc_db;
    wait_queue_t resp_wait_queue;
    wait_queue_t server_wait_queue;

    int ipc_sem_id;
    int ipc_shm_id;
    char *ipc_shm_buf;

    struct sembuf sem_op_server_wait;
    struct sembuf sem_op_server_term;
    struct sembuf sem_op_server_wakeup;

    pthread_t pthread_server1;
    pthread_t pthread_server2;
    pthread_t pthread_listener;

    void (*ipc_req_server)(ipc_req_t *, ipc_resp_t *);
} ipc_ctx_t;

static ipc_ctx_t generic_ipc_ctx;

static void
send_data (ipc_ctx_t *ctx_p, int pak_type, uint32_t seq_num,
           void *data, uint32_t data_size)
{
    ipc_pak_t *pak_p;

    memset(ctx_p->ipc_shm_buf, 0, IPC_BUF_SIZE);

    if (data_size > IPC_BUF_DATA_MAX_SIZE) {
        data_size = IPC_BUF_DATA_MAX_SIZE;
    }

    pak_p = (ipc_pak_t *)ctx_p->ipc_shm_buf;
    pak_p->pak_type = pak_type;
    pak_p->seq_num = seq_num;
    pak_p->data_size = data_size;
    if (data) {
        memcpy(pak_p->data, data, data_size);
    }

    IPC_DBG("Send IPC data done, type %d, SEQ number %d.", pak_type, seq_num);
    return;
}

static ipc_pak_t *
read_data (ipc_ctx_t *ctx_p)
{
    return (ipc_pak_t *)(ctx_p->ipc_shm_buf);
}

static uint32_t
get_seq_num (ipc_ctx_t *ctx_p)
{
    uint32_t seq_num;

    pthread_mutex_lock(&ctx_p->ipc_db_mutex);
    seq_num = (ctx_p->ipc_db.next_seq_num++);
    pthread_mutex_unlock(&ctx_p->ipc_db_mutex);

    IPC_DBG("Get SEQ number as %d", seq_num);
    return seq_num;
}

static void
inc_error_pak_cnt (ipc_ctx_t *ctx_p)
{
    pthread_mutex_lock(&ctx_p->ipc_db_mutex);
    ctx_p->ipc_db.error_pak_cnt++;
    pthread_mutex_unlock(&ctx_p->ipc_db_mutex);
    return;
}

static bool
is_error_pak_cnt_reach_max (ipc_ctx_t *ctx_p)
{
    bool result = FALSE;

    pthread_mutex_lock(&ctx_p->ipc_db_mutex);
    if (ctx_p->ipc_db.error_pak_cnt > IPC_ERROR_PAK_MAX_CNT) {
        result = TRUE;
    }
    pthread_mutex_unlock(&ctx_p->ipc_db_mutex);
    return result;
}

static void
set_terminate_flag (ipc_ctx_t *ctx_p)
{
    pthread_mutex_lock(&ctx_p->ipc_db_mutex);
    ctx_p->ipc_db.terminate_flag = TRUE;
    pthread_mutex_unlock(&ctx_p->ipc_db_mutex);
    return;
}

static bool
is_terminate_flag_set (ipc_ctx_t *ctx_p)
{
    bool status;

    pthread_mutex_lock(&ctx_p->ipc_db_mutex);
    status = ctx_p->ipc_db.terminate_flag;
    pthread_mutex_unlock(&ctx_p->ipc_db_mutex);
    return status;
}

static void
init_queue_entry (wait_queue_entry_t *entry_p)
{
    memset(entry_p, 0, sizeof(wait_queue_entry_t));
    entry_p->in_pool = TRUE;
    entry_p->free = TRUE;
    return;
}

static wait_queue_entry_t *
create_queue_entry (ipc_ctx_t *ctx_p)
{
    wait_queue_entry_t *db_entry_p, *entry_p = NULL;
    ipc_db_t *ipc_db_p = &ctx_p->ipc_db;
    uint32_t i;

    pthread_mutex_lock(&ctx_p->ipc_db_mutex);

    for (i = 0; i < ipc_db_p->entry_cnt; i++) {
        db_entry_p = &ipc_db_p->entry_pool[i];
        if (db_entry_p->free == TRUE) {
            db_entry_p->free = FALSE;
            entry_p = db_entry_p;
            break;
        }
    }

    pthread_mutex_unlock(&ctx_p->ipc_db_mutex);

    if (entry_p) {
        return entry_p;
    }

    IPC_DBG("No entry in pool, allocate it.");

    entry_p = calloc(1, sizeof(wait_queue_entry_t));
    if (entry_p == NULL) {
        return NULL;
    }

    entry_p->in_pool = FALSE;
    return entry_p;
}

static void
free_queue_entry (ipc_ctx_t *ctx_p, wait_queue_entry_t *entry_p)
{
    if (entry_p->in_pool == FALSE) {
        free(entry_p);
    }

    pthread_mutex_lock(&ctx_p->ipc_db_mutex);
    init_queue_entry(entry_p);
    pthread_mutex_unlock(&ctx_p->ipc_db_mutex);
    return;
}

static bool
is_valid_ipc_pak (ipc_ctx_t *ctx_p, ipc_pak_t *pak_p)
{
    if (pak_p->pak_type <= IPC_PAK_MIN || pak_p->pak_type >= IPC_PAK_MAX) {
        inc_error_pak_cnt(ctx_p);
        IPC_DBG("Incoming IPC pak not valid, type %d", pak_p->pak_type);
        return FALSE;
    }
    return TRUE;
}

static bool
is_queue_reach_max_size (wait_queue_t *wait_queue_p)
{
    uint32_t max_size, queue_size;

    max_size = queue_get_max_size(&wait_queue_p->queue);
    if (max_size == 0) {
        return FALSE;
    }

    queue_size = queue_get_size(&wait_queue_p->queue);
    if (queue_size >= max_size) {
        return TRUE;
    }

    return FALSE;
}

static void
enqueue_ipc_pak (ipc_ctx_t *ctx_p, ipc_pak_t *pak_p)
{
    wait_queue_t *wait_queue_p;
    wait_queue_entry_t *entry_p;

    if (is_valid_ipc_pak(ctx_p, pak_p) == FALSE) {
        return;
    }

    if (pak_p->pak_type == IPC_PAK_RESP) {
        wait_queue_p = &ctx_p->resp_wait_queue;
    } else {
        wait_queue_p = &ctx_p->server_wait_queue;
    }

    if (is_queue_reach_max_size(wait_queue_p) == TRUE) {
        return;
    }

    entry_p = create_queue_entry(ctx_p);
    if (!entry_p) {
        return;
    }

    entry_p->pak_type = pak_p->pak_type;
    entry_p->seq_num = pak_p->seq_num;
    entry_p->data_size = pak_p->data_size;
    entry_p->data = calloc(1, pak_p->data_size);
    if (!entry_p->data) {
        free_queue_entry(ctx_p, entry_p);
        return;
    }
    memcpy(entry_p->data, pak_p->data, pak_p->data_size);

    pthread_mutex_lock(&wait_queue_p->queue_mutex);
    (void)enqueue(&wait_queue_p->queue, entry_p);
    pthread_mutex_unlock(&wait_queue_p->queue_mutex);

    IPC_DBG("Enqueue incoming IPC pak, type %d, SEQ number %d",
            entry_p->pak_type, entry_p->seq_num);

    pthread_cond_broadcast(&wait_queue_p->queue_cond);

    return;
}

static void
sem_wait (int sem_id, struct sembuf *sem_op_p)
{
    int rc;

    for (;;) {
        rc = semop(sem_id, sem_op_p, 1);
        if (rc == 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }

        IPC_DBG("sem wait error, errno %d", errno);
        break;
    }

    return;
}

static int
sem_wait_timeout (int sem_id, struct sembuf *sem_op_p, struct timespec *ts_p)
{
    int rc;

    for (;;) {
        rc = semtimedop(sem_id, sem_op_p, 1, ts_p);
        if (rc == 0) {
            return 0;
        }
        if (errno == EINTR) {
            continue;
        }

        IPC_DBG("sem wait error, errno %d", errno);
        return -1;
    }

    return -1;
}

static void
sem_wakeup (int sem_id, struct sembuf *sem_op_p)
{
    semop(sem_id, sem_op_p, 1);
    return;
}

static void *
listener_thread (void *arg)
{
    ipc_pak_t *pak_p = NULL;
    ipc_ctx_t *ctx_p = (ipc_ctx_t *)arg;

    for (;;) {
        sem_wait(ctx_p->ipc_sem_id, &ctx_p->sem_op_server_wait);

        if (is_terminate_flag_set(ctx_p) == TRUE) {
            pthread_exit(0);
        }

        pak_p = read_data(ctx_p);
        if (pak_p->pak_type == IPC_PAK_INIT) {
            send_data(ctx_p, IPC_PAK_ACK, pak_p->seq_num, NULL, 0);
            sem_wakeup(ctx_p->ipc_sem_id, &sem_op_ack_wakeup);
            IPC_DBG("Send back IPC ACK, SEQ number %d.", pak_p->seq_num);
            continue;
        }

        enqueue_ipc_pak(ctx_p, pak_p);

        if (is_error_pak_cnt_reach_max(ctx_p) == TRUE) {
            IPC_DBG("IPC error pak reach max limit, terminate.");
            pthread_exit(0);
        }

        memset(ctx_p->ipc_shm_buf, 0, IPC_BUF_SIZE);
        sem_wakeup(ctx_p->ipc_sem_id, &sem_op_buf_wakeup);
        IPC_DBG("Release buf lock.");
    }

    return NULL;
}

static void
terminate_listener_thread (ipc_ctx_t *ctx_p)
{
    set_terminate_flag(ctx_p);
    sem_wakeup(ctx_p->ipc_sem_id, &ctx_p->sem_op_server_term);
    return;
}

static void
get_ipc_request (ipc_ctx_t *ctx_p, ipc_req_t *ipc_req_p, uint32_t *seq_num_p)
{
    wait_queue_entry_t *entry_p;

    memset(ipc_req_p, 0, sizeof(ipc_req_t));

    entry_p = dequeue(&ctx_p->server_wait_queue.queue);
    if (!entry_p) {
        return;
    }

    ipc_req_p->req_type = entry_p->pak_type;
    ipc_req_p->req_data = entry_p->data;
    ipc_req_p->req_data_size = entry_p->data_size;
    *seq_num_p = entry_p->seq_num;

    free_queue_entry(ctx_p, entry_p);
    return;
}

static void
serve_ipc_request (ipc_ctx_t *ctx_p, ipc_req_t *ipc_req_p, uint32_t seq_num)
{
    ipc_resp_t ipc_resp;

    ctx_p->ipc_req_server(ipc_req_p, &ipc_resp);

    IPC_DBG("Send back response data, SEQ number %d", seq_num);

    IPC_DBG("Acquiring buf lock.");
    sem_wait(ctx_p->ipc_sem_id, &sem_op_buf_wait);

    send_data(ctx_p, IPC_PAK_RESP, seq_num,
              ipc_resp.resp_data, ipc_resp.resp_data_size);
    sem_wakeup(ctx_p->ipc_sem_id, &ctx_p->sem_op_server_wakeup);

    if (ipc_resp.resp_data) {
        free(ipc_resp.resp_data);
    }
    return;
}

static void *
server_thread (void *arg)
{
    ipc_req_t ipc_req;
    ipc_ctx_t *ctx_p = (ipc_ctx_t *)arg;
    pthread_t thread_id;
    uint32_t seq_num = 0;

    thread_id = pthread_self();

    for (;;) {
        pthread_mutex_lock(&ctx_p->server_wait_queue.queue_mutex);

        for (;;) {
            if (is_terminate_flag_set(ctx_p) == TRUE) {
                pthread_mutex_unlock(&ctx_p->server_wait_queue.queue_mutex);
                pthread_exit(0);
            }

            get_ipc_request(ctx_p, &ipc_req, &seq_num);
            if (ipc_req.req_data != NULL) {
                break;
            }

            pthread_cond_wait(&ctx_p->server_wait_queue.queue_cond,
                              &ctx_p->server_wait_queue.queue_mutex);
        }

        pthread_mutex_unlock(&ctx_p->server_wait_queue.queue_mutex);

        IPC_DBG("Serve incoming request, thread #%d, SEQ number %d",
                (uint32_t)thread_id, seq_num);
        serve_ipc_request(ctx_p, &ipc_req, seq_num);
        free(ipc_req.req_data);
    }

    return NULL;
}

static void
terminate_server_thread (ipc_ctx_t *ctx_p)
{
    set_terminate_flag(ctx_p);
    pthread_cond_broadcast(&ctx_p->server_wait_queue.queue_cond);
    return;
}

static int
reset_sem_val (int sem_id, int sem_idx)
{
    int rc;
    union semun sem_args;

    sem_args.val = 0;
    rc = semctl(sem_id, sem_idx, SETVAL, sem_args);
    if (rc == -1) {
        return -1;
    }

    return 0;
}

static int
check_ipc_connection (ipc_ctx_t *ctx_p, uint32_t seq_num)
{
    ipc_pak_t *pak_p;
    int ipc_sem_id = ctx_p->ipc_sem_id;
    int rc;

    IPC_DBG("Check IPC connection.");

    rc = reset_sem_val(ipc_sem_id, SEM_IDX_ACK);
    if (rc != 0) {
        IPC_DBG("Reset ACK sem failed.");
        return -1;
    }

    send_data(ctx_p, IPC_PAK_INIT, seq_num, NULL, 0);

    sem_wakeup(ipc_sem_id, &ctx_p->sem_op_server_wakeup);

    rc = sem_wait_timeout(ipc_sem_id, &sem_op_ack_wait, &ipc_timeout);
    if (rc != 0) {
        IPC_DBG("IPC peer side not exist, errno %d.", errno);
        return -1;
    }

    pak_p = read_data(ctx_p);
    if (pak_p->pak_type != IPC_PAK_ACK) {
        IPC_DBG("IPC resp type is not ACK, type %d.", pak_p->pak_type);
        return -1;
    }

    IPC_DBG("IPC connection OK.");
    return 0;
}

static int
send_request (ipc_ctx_t *ctx_p, ipc_req_t *ipc_req_p, uint32_t seq_num)
{
    int pak_type = ipc_req_p->req_type;
    void *data = ipc_req_p->req_data;
    uint32_t size = ipc_req_p->req_data_size;
    int ipc_sem_id = ctx_p->ipc_sem_id;
    int rc;

    IPC_DBG("Send IPC request.");

    if (!data) {
        return -1;
    }

    if (size > IPC_BUF_DATA_MAX_SIZE) {
        return -1;
    }

    // For normal case, will relase it on server side
    IPC_DBG("Acquiring buf lock.");
    sem_wait(ipc_sem_id, &sem_op_buf_wait);

    rc = check_ipc_connection(ctx_p, seq_num);
    if (rc == -1) {
        sem_wakeup(ipc_sem_id, &sem_op_buf_wakeup);
        IPC_DBG("Release buf lock.");
        return -1;
    }

    send_data(ctx_p, pak_type, seq_num, data, size);
    sem_wakeup(ipc_sem_id, &ctx_p->sem_op_server_wakeup);

    return 0;
}

static int
get_resp_entry (ipc_ctx_t *ctx_p, uint32_t seq_num, ipc_resp_t *ipc_resp_p)
{
    wait_queue_entry_t *entry_p;
    queue_t *queue_p;

    memset(ipc_resp_p, 0, sizeof(ipc_resp_t));

    queue_p = &ctx_p->resp_wait_queue.queue;
    for (entry_p = queue_peek_head(queue_p);
         entry_p != NULL;
         entry_p = queue_get_next(queue_p, entry_p)) {
        if (entry_p->seq_num == seq_num) {
            break;
        }
    }

    if (!entry_p) {
        return -1;
    }

    (void)unqueue(queue_p, entry_p);

    ipc_resp_p->resp_data = entry_p->data;
    ipc_resp_p->resp_data_size = entry_p->data_size;

    free_queue_entry(ctx_p, entry_p);
    return 0;
}

static void
wait_for_resp (ipc_ctx_t *ctx_p, uint32_t seq_num, ipc_resp_t *ipc_resp_p)
{
    int rc;

    pthread_mutex_lock(&ctx_p->resp_wait_queue.queue_mutex);

    for (;;) {
        rc = get_resp_entry(ctx_p, seq_num, ipc_resp_p);
        if (rc == 0) {
            break;
        }
        pthread_cond_wait(&ctx_p->resp_wait_queue.queue_cond,
                          &ctx_p->resp_wait_queue.queue_mutex);

    }

    pthread_mutex_unlock(&ctx_p->resp_wait_queue.queue_mutex);

    IPC_DBG("Get resp, SEQ number %d", seq_num);
    return;
}

int
generic_ipc_request (ipc_req_t *ipc_req_p, ipc_resp_t *ipc_resp_p)
{
    ipc_ctx_t *ctx_p = &generic_ipc_ctx;
    uint32_t seq_num;
    int rc;

    if (!ctx_p->init_done) {
        IPC_DBG("IPC infra not initialized.");
        return -1;
    }

    memset(ipc_resp_p, 0, sizeof(ipc_resp_t));

    seq_num = get_seq_num(ctx_p);

    rc = send_request(ctx_p, ipc_req_p, seq_num);
    if (rc == -1) {
        return -1;
    }

    wait_for_resp(ctx_p, seq_num, ipc_resp_p);
    return 0;
}

static int 
sem_int (key_t ipc_key)
{
    int rc, sem_id;
    union semun sem_args;
    unsigned short sem_init_array[SEM_CNT] = {0, 0, 0, 1};

    sem_id = semget(ipc_key, SEM_CNT, 0666 | IPC_CREAT);
    if (sem_id == -1) {
        return -1;
    }

    sem_args.array = sem_init_array;
    rc = semctl(sem_id, 0, SETALL, sem_args);
    if (rc == -1) {
        return -1;
    }

    return sem_id;
}

static void
sem_clean (int sem_id)
{
    union semun sem_arg;

    semctl(sem_id, 0, IPC_RMID, &sem_arg);
    return;
}

static int
shm_init (int ipc_key)
{
    int shm_id;

    shm_id = shmget(ipc_key, IPC_BUF_SIZE, 0666 | IPC_CREAT);
    return shm_id;
}

static char *
shm_buf_init (int shm_id)
{
    void *shm_buf;

    shm_buf = shmat(shm_id, 0, 0);
    if (shm_buf == ((void *)-1)) {
        return NULL;
    }

    return ((char *)shm_buf);
}

static void
shm_clean (int shm_id)
{
    shmctl(shm_id, IPC_RMID, 0);
    return;
}

static void
shm_buf_clean (char *shm_buf)
{
    shmdt(shm_buf);
    return;
}

static key_t
get_ipc_key (char *path)
{
    key_t ipc_key;

    ipc_key = ftok(path, 0);
    if (ipc_key == -1) {
        return -1;
    }

    return ipc_key;
}

static int
ipc_infra_init (ipc_ctx_t *ctx_p, key_t ipc_key)
{
    ctx_p->ipc_sem_id = sem_int(ipc_key);
    if (ctx_p->ipc_sem_id == -1) {
        return -1;
    }
    IPC_DBG("Get sem id as %d", ctx_p->ipc_sem_id);

    ctx_p->ipc_shm_id = shm_init(ipc_key);
    if (ctx_p->ipc_shm_id == -1) {
        sem_clean(ctx_p->ipc_sem_id);
        return -1;
    }
    IPC_DBG("Get shm id as %d", ctx_p->ipc_shm_id);

    ctx_p->ipc_shm_buf = shm_buf_init(ctx_p->ipc_shm_id);
    if (!ctx_p->ipc_shm_buf) {
        sem_clean(ctx_p->ipc_sem_id);
        shm_clean(ctx_p->ipc_shm_id);
        return -1;
    }

    memset(ctx_p->ipc_shm_buf, 0, IPC_BUF_SIZE);

    return 0;
}

static int
get_cur_dir (char *path)
{
    uint32_t i, len;
    int rc;

    memset(path, 0, _POSIX_PATH_MAX+1);
    
    rc = readlink("/proc/self/exe", path, _POSIX_PATH_MAX);
    if (rc == -1) {
        return -1;
    }

    len = strlen(path);
    for (i = len; i >= 0; i--) {
        if (path[i] == '/') {
            path[i] = '\0';
            break;
        }
    }

    return 0;
}

static int
generic_ipc_infra_init (int owner_id)
{
    key_t ipc_key;
    ipc_ctx_t *ctx_p = &generic_ipc_ctx;
    char path[_POSIX_PATH_MAX+1];
    int rc;

    rc = get_cur_dir(path);
    if (rc != 0) {
        return rc;
    }

    ipc_key = get_ipc_key(path);
    if (ipc_key == -1) {
        return -1;
    }

    rc = ipc_infra_init(ctx_p, ipc_key);
    if (rc != 0) {
        return rc;
    }

    ctx_p->sem_op_server_wait.sem_op = -1;
    ctx_p->sem_op_server_term.sem_op = 1;
    ctx_p->sem_op_server_wakeup.sem_op = 1;

    ctx_p->sem_op_server_wait.sem_flg = SEM_UNDO;
    ctx_p->sem_op_server_term.sem_flg = SEM_UNDO;
    ctx_p->sem_op_server_wakeup.sem_flg = SEM_UNDO;

    if (owner_id == IPC_OWNER_CLIENT) {
        ctx_p->sem_op_server_wait.sem_num = 0;
        ctx_p->sem_op_server_term.sem_num = 0;
        ctx_p->sem_op_server_wakeup.sem_num = 1;
    } else { // IPC_OWNER_SERVER
        ctx_p->sem_op_server_wait.sem_num = 1;
        ctx_p->sem_op_server_term.sem_num = 1;
        ctx_p->sem_op_server_wakeup.sem_num = 0;
    }

    return 0;
}

static void
generic_ipc_infra_clean (int owner_id)
{
    if (owner_id == IPC_OWNER_CLIENT) {
        sem_clean(generic_ipc_ctx.ipc_sem_id);
        shm_clean(generic_ipc_ctx.ipc_shm_id);
        shm_buf_clean(generic_ipc_ctx.ipc_shm_buf);
    }
    return;
}

static int
worker_threads_create (ipc_ctx_t *ctx_p)
{
    int rc;

    rc = pthread_create(&ctx_p->pthread_server1, NULL, server_thread, ctx_p);
    if (rc != 0) {
        return -1;
    }

    rc = pthread_create(&ctx_p->pthread_server2, NULL, server_thread, ctx_p);
    if (rc != 0) {
        return -1;
    }

    rc = pthread_create(&ctx_p->pthread_listener, NULL, listener_thread, ctx_p);
    if (rc != 0) {
        return -1;
    }

    return 0;
}

static void
worker_threads_clean (ipc_ctx_t *ctx_p)
{
    terminate_server_thread(ctx_p);
    terminate_listener_thread(ctx_p);
    pthread_join(ctx_p->pthread_server1, NULL);
    pthread_join(ctx_p->pthread_server2, NULL);
    pthread_join(ctx_p->pthread_listener, NULL);
    return;
}

static int
init_wait_queue (wait_queue_t *wait_queue_p)
{
    int rc;

    memset(wait_queue_p, 0, sizeof(wait_queue_t));

    queue_init(&wait_queue_p->queue, WAIT_QUEUE_MAX_SIZE);

    rc = pthread_mutex_init(&wait_queue_p->queue_mutex, NULL);
    if (rc != 0) {
        return -1;
    }

    rc = pthread_cond_init(&wait_queue_p->queue_cond, NULL);
    if (rc != 0) {
        pthread_mutex_destroy(&wait_queue_p->queue_mutex);
        return -1;
    }

    return 0;
}

static void
free_wait_queue_entries (ipc_ctx_t *ctx_p, wait_queue_t *wait_queue_p)
{
    wait_queue_entry_t *entry_p;

    for (;;) {
        entry_p = dequeue(&wait_queue_p->queue);
        if (!entry_p) {
            return;
        }
        free_queue_entry(ctx_p, entry_p);
    }
    return;
}

static void
clean_wait_queue (ipc_ctx_t *ctx_p, wait_queue_t *wait_queue_p)
{
    free_wait_queue_entries(ctx_p, wait_queue_p);
    pthread_mutex_destroy(&wait_queue_p->queue_mutex);
    pthread_cond_destroy(&wait_queue_p->queue_cond);
    return;
}

static int
wait_queues_init (ipc_ctx_t *ctx_p)
{
    int rc;

    rc = init_wait_queue(&ctx_p->resp_wait_queue);
    if (rc != 0) {
        return rc;
    }

    rc = init_wait_queue(&ctx_p->server_wait_queue);
    if (rc != 0) {
        clean_wait_queue(ctx_p, &ctx_p->resp_wait_queue);
        return rc;
    }
    return 0;
}

static void
wait_queues_clean (ipc_ctx_t *ctx_p)
{
    clean_wait_queue(ctx_p, &ctx_p->resp_wait_queue);
    clean_wait_queue(ctx_p, &ctx_p->server_wait_queue);
    return;
}

static int
ipc_db_init (ipc_ctx_t *ctx_p)
{
    wait_queue_entry_t *entry_p;
    ipc_db_t *ipc_db_p;
    uint32_t i, entry_cnt;
    int rc;

    ipc_db_p = &ctx_p->ipc_db;
    memset(ipc_db_p, 0, sizeof(ipc_db_t));

    rc = pthread_mutex_init(&ctx_p->ipc_db_mutex, NULL);
    if (rc != 0) {
        return -1;
    }

    entry_cnt = WAIT_QUEUE_ENTRY_POOL_SIZE;
    entry_p = calloc(entry_cnt, sizeof(wait_queue_entry_t));
    if (!entry_p) {
        pthread_mutex_destroy(&ctx_p->ipc_db_mutex);
        return -1;
    }

    ipc_db_p->entry_pool = entry_p;
    ipc_db_p->entry_cnt = entry_cnt;
    for (i = 0; i < entry_cnt; i++) {
        init_queue_entry(entry_p);
        entry_p++;
    }

    return 0;
}

static int
generic_ipc_db_init (int owner_id)
{
    int rc;

    rc = ipc_db_init(&generic_ipc_ctx);
    if (rc != 0) {
        return rc;
    }

    if (owner_id == IPC_OWNER_CLIENT) {
        generic_ipc_ctx.ipc_db.next_seq_num = 1000;
    } else { // IPC_OWNER_SERVER
        generic_ipc_ctx.ipc_db.next_seq_num = 5000;
    }

    return 0;
}

static void
ipc_db_clean (ipc_ctx_t *ctx_p)
{
    pthread_mutex_destroy(&ctx_p->ipc_db_mutex);
    free(ctx_p->ipc_db.entry_pool);
    return;
}

static void
generic_ipc_db_clean (void)
{
    ipc_db_clean(&generic_ipc_ctx);
    return;
}

int
generic_ipc_init (int owner_id,
                  void (*handler)(ipc_req_t *, ipc_resp_t *))
{
    int rc;

    IPC_DBG_INIT(owner_id);

    rc = generic_ipc_db_init(owner_id);
    if (rc != 0) {
        return -1;
    }

    rc = wait_queues_init(&generic_ipc_ctx);
    if (rc != 0) {
        generic_ipc_db_clean();
        return -1;
    }

    rc = generic_ipc_infra_init(owner_id);
    if (rc != 0) {
        generic_ipc_db_clean();
        wait_queues_clean(&generic_ipc_ctx);
        return -1;
    }

    rc = worker_threads_create(&generic_ipc_ctx);
    if (rc != 0) {
        generic_ipc_db_clean();
        wait_queues_clean(&generic_ipc_ctx);
        generic_ipc_infra_clean(owner_id);
        return -1;
    }

    generic_ipc_ctx.ipc_req_server = handler;
    generic_ipc_ctx.init_done = TRUE;
    return 0;
}

void
generic_ipc_clean (int owner_id)
{
    worker_threads_clean(&generic_ipc_ctx);
    generic_ipc_infra_clean(owner_id);
    wait_queues_clean(&generic_ipc_ctx);
    generic_ipc_db_clean();
    IPC_DBG_CLEAN();
    return;
}

