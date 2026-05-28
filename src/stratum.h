#ifndef STRATUM_H
#define STRATUM_H

#include <stdint.h>
#include <pthread.h>
#include "yespower.h"

#define MAX_MERKLE_BRANCH 32

typedef struct {
    char job_id[128];
    uint8_t prevhash[32];
    uint8_t coinb1[256];
    int coinb1_len;
    uint8_t coinb2[256];
    int coinb2_len;
    int merkle_count;
    uint8_t merkle_branch[MAX_MERKLE_BRANCH][32];
    uint32_t version;
    uint32_t nbits;
    uint32_t ntime;
    int clean_jobs;
} stratum_job;

typedef struct {
    int sock;
    char host[256];
    int port;
    char worker[256];
    char password[256];

    char extranonce1[64];
    int extranonce1_len;
    int extranonce2_size;

    stratum_job current_job;
    int has_job;
    int new_job_flag;
    pthread_mutex_t job_mutex;

    double difficulty;
    uint8_t share_target[32];

    int connected;
    int subscribed;
    int authorized;

    long long share_count;
    char recv_buf[65536];
    int recv_len;

    int submit_id;
} stratum_ctx;

int stratum_connect(stratum_ctx *ctx, const char *host, int port,
                    const char *worker, const char *password);
void stratum_destroy(stratum_ctx *ctx);
int stratum_subscribe(stratum_ctx *ctx);
int stratum_authorize(stratum_ctx *ctx);
int stratum_read_line(stratum_ctx *ctx, char *buf, int bufsize, int timeout_ms);
int stratum_process_message(stratum_ctx *ctx, const char *line);
int stratum_get_job(stratum_ctx *ctx, stratum_job *job);
int stratum_submit_share(stratum_ctx *ctx, const char *job_id,
                         const char *extranonce2, const char *ntime,
                         const char *nonce);

void hex_encode(const uint8_t *data, int len, char *out);
int build_block_header(stratum_ctx *ctx, const char *extranonce2,
                       uint8_t header[80], uint32_t nonce);
int check_share(const uint8_t hash[32], const uint8_t target[32]);
void nbits_to_target(uint32_t nbits, uint8_t target[32]);
void difficulty_to_target(double diff, uint8_t target[32]);

#endif
