#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <pthread.h>
#include <sched.h>

#include "stratum.h"
#include "yespower.h"

#define POWER2B_PERS "Client Key"
#define SHARE_QUEUE_SIZE 4096

typedef struct {
    char job_id[128];
    char extranonce2[64];
    char ntime[16];
    char nonce[16];
} share_entry;

typedef struct {
    pthread_mutex_t job_mutex;
    pthread_cond_t job_cond;
    volatile int job_version;
    volatile int valid_job;
    uint8_t header[80];
    uint8_t target[32];
    char job_id[128];
    char extranonce2[64];

    pthread_mutex_t share_mutex;
    share_entry share_queue[SHARE_QUEUE_SIZE];
    int share_head;
    int share_tail;

    volatile int running;

    volatile unsigned long long total_hashes;
} miner_state;

static miner_state g_state;

static void handle_signal(int sig) {
    (void)sig;
    g_state.running = 0;
}

static void push_share(const char *job_id, const char *extranonce2,
                       const char *ntime, const char *nonce) {
    pthread_mutex_lock(&g_state.share_mutex);
    int next = (g_state.share_tail + 1) % SHARE_QUEUE_SIZE;
    if (next != g_state.share_head) {
        share_entry *s = &g_state.share_queue[g_state.share_tail];
        strncpy(s->job_id, job_id, sizeof(s->job_id) - 1);
        s->job_id[sizeof(s->job_id) - 1] = '\0';
        strncpy(s->extranonce2, extranonce2, sizeof(s->extranonce2) - 1);
        s->extranonce2[sizeof(s->extranonce2) - 1] = '\0';
        strncpy(s->ntime, ntime, sizeof(s->ntime) - 1);
        s->ntime[sizeof(s->ntime) - 1] = '\0';
        strncpy(s->nonce, nonce, sizeof(s->nonce) - 1);
        s->nonce[sizeof(s->nonce) - 1] = '\0';
        g_state.share_tail = next;
    }
    pthread_mutex_unlock(&g_state.share_mutex);
}

static int pop_share(share_entry *s) {
    int found = 0;
    pthread_mutex_lock(&g_state.share_mutex);
    if (g_state.share_head != g_state.share_tail) {
        *s = g_state.share_queue[g_state.share_head];
        g_state.share_head = (g_state.share_head + 1) % SHARE_QUEUE_SIZE;
        found = 1;
    }
    pthread_mutex_unlock(&g_state.share_mutex);
    return found;
}

static int read_version(void) {
    return *(volatile int *)&g_state.job_version;
}

static void *worker_thread(void *arg) {
    intptr_t thread_id = (intptr_t)arg;

    yespower_params_t params;
    memset(&params, 0, sizeof(params));
    params.N = 2048;
    params.r = 32;
    params.pers = (const uint8_t *)POWER2B_PERS;
    params.perslen = strlen(POWER2B_PERS);

    yespower_local_t local;
    if (yespower_init_local(&local) != 0) {
        fprintf(stderr, "\n[worker %d] yespower_init_local failed\n", (int)thread_id);
        return NULL;
    }

    yespower_binary_t hash;
    uint32_t nonce;
    uint8_t header[80];
    uint8_t target[32];
    char job_id[128];
    char extranonce2[64];
    int local_version = -1;
    int have_job = 0;

    while (g_state.running) {
        int ver = read_version();
        if (ver != local_version) {
            pthread_mutex_lock(&g_state.job_mutex);
            if (g_state.valid_job) {
                memcpy(header, g_state.header, 80);
                memcpy(target, g_state.target, 32);
                strncpy(job_id, g_state.job_id, sizeof(job_id) - 1);
                job_id[sizeof(job_id) - 1] = '\0';
                strncpy(extranonce2, g_state.extranonce2, sizeof(extranonce2) - 1);
                extranonce2[sizeof(extranonce2) - 1] = '\0';
                local_version = g_state.job_version;
                nonce = (uint32_t)(((uint64_t)g_state.job_version * 100003 + thread_id * 10007) & 0xFFFFFFFF);
                have_job = 1;
            } else {
                have_job = 0;
                local_version = ver;
            }
            pthread_mutex_unlock(&g_state.job_mutex);
        }

        if (!have_job) {
            struct timespec ts = {0, 1000000};
            nanosleep(&ts, NULL);
            continue;
        }

        header[76] = nonce & 0xFF;
        header[77] = (nonce >> 8) & 0xFF;
        header[78] = (nonce >> 16) & 0xFF;
        header[79] = (nonce >> 24) & 0xFF;

        if (yespower(&local, header, 80, &params, &hash) == 0) {
            __sync_fetch_and_add(&g_state.total_hashes, 1);

            for (int i = 0; i < 32; i++) {
                if (hash.uc[i] > target[i]) break;
                if (hash.uc[i] < target[i]) {
                    char nonce_hex[16];
                    char ntime_hex[16];
                    hex_encode(&header[68], 4, ntime_hex);
                    hex_encode(&header[76], 4, nonce_hex);
                    fprintf(stderr, "\n[worker %d] *** SHARE FOUND *** nonce=%s\n",
                            (int)thread_id, nonce_hex);
                    push_share(job_id, extranonce2, ntime_hex, nonce_hex);
                    break;
                }
            }
        }

        nonce++;
    }

    yespower_free_local(&local);
    return NULL;
}

static void generate_extranonce2(char *out, int bytes) {
    static int seed = 0;
    if (!seed) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        seed = (int)(ts.tv_nsec ^ (ts.tv_sec << 16));
    }
    for (int i = 0; i < bytes; i++) {
        seed = seed * 1103515245 + 12345;
        out[i * 2] = "0123456789abcdef"[(seed >> 20) & 0xf];
        out[i * 2 + 1] = "0123456789abcdef"[(seed >> 16) & 0xf];
    }
    out[bytes * 2] = '\0';
}

static void print_stats(unsigned long long total_hashes, long long shares,
                        double elapsed, double diff, int threads) {
    double rate = elapsed > 0 ? (double)total_hashes / elapsed : 0;
    fprintf(stderr, "\r[power2b-miner]");
    if (threads > 1) fprintf(stderr, " %dx", threads);
    fprintf(stderr, " %.2f H/s  shares=%lld  diff=%.4f  total=%llu  ",
            rate, shares, diff, total_hashes);
    fflush(stderr);
}

int main(int argc, char *argv[]) {
    const char *host = "power2b.eu.mine.zpool.ca";
    int port = 6242;
    const char *worker = "33AUHjnweSG9nz93JJqrsY97EN6o8bNRUC";
    const char *password = "c=BTC";
    int threads = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            char url[512];
            strncpy(url, argv[++i], sizeof(url) - 1);
            char *p = strstr(url, "://");
            if (p) p += 3; else p = url;
            char *colon = strchr(p, ':');
            if (colon) {
                *colon = '\0';
                host = strdup(p);
                port = atoi(colon + 1);
            } else {
                host = strdup(p);
            }
        } else if (strcmp(argv[i], "-u") == 0 && i + 1 < argc) {
            worker = argv[++i];
        } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            password = argv[++i];
        } else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            threads = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-h") == 0) {
            fprintf(stderr, "Usage: %s -o stratum+tcp://host:port -u worker -p password [-t threads]\n", argv[0]);
            return 0;
        }
    }

    if (threads <= 0) {
        threads = sysconf(_SC_NPROCESSORS_ONLN);
        if (threads <= 0) threads = 4;
    }

    memset(&g_state, 0, sizeof(g_state));
    pthread_mutex_init(&g_state.job_mutex, NULL);
    pthread_cond_init(&g_state.job_cond, NULL);
    pthread_mutex_init(&g_state.share_mutex, NULL);
    g_state.running = 1;

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    pthread_t *worker_ids = malloc(threads * sizeof(pthread_t));
    for (int i = 0; i < threads; i++) {
        pthread_create(&worker_ids[i], NULL, worker_thread, (void *)(intptr_t)i);
    }

    stratum_ctx ctx;
    int auth_sent = 0;
    char line[65536];
    char extranonce2[64];
    long long share_count = 0;
    double start_time = time(NULL);
    double last_stats = start_time;

reconnect:
    fprintf(stderr, "[power2b-miner] Connecting to %s:%d (%d threads) ...\n", host, port, threads);
    if (stratum_connect(&ctx, host, port, worker, password) < 0) {
        fprintf(stderr, "[power2b-miner] Failed to connect, retrying in 10s\n");
        sleep(10);
        goto reconnect;
    }

    stratum_subscribe(&ctx);
    auth_sent = 0;

    while (g_state.running) {
        int ret = stratum_read_line(&ctx, line, sizeof(line), 100);
        if (ret < 0) {
            fprintf(stderr, "\n[power2b-miner] Connection lost, reconnecting...\n");
            stratum_destroy(&ctx);
            pthread_mutex_lock(&g_state.job_mutex);
            g_state.valid_job = 0;
            g_state.job_version++;
            pthread_mutex_unlock(&g_state.job_mutex);
            goto reconnect;
        }
        if (ret > 0) {
            stratum_process_message(&ctx, line);
            if (!auth_sent && ctx.subscribed) {
                stratum_authorize(&ctx);
                auth_sent = 1;
            }
        }

        if (ctx.new_job_flag) {
            stratum_job job;
            pthread_mutex_lock(&ctx.job_mutex);
            memcpy(&job, &ctx.current_job, sizeof(job));
            ctx.new_job_flag = 0;
            pthread_mutex_unlock(&ctx.job_mutex);

            generate_extranonce2(extranonce2, ctx.extranonce2_size > 0 ? ctx.extranonce2_size : 4);

            uint8_t header[80];
            build_block_header(&ctx, extranonce2, header, 0);

            pthread_mutex_lock(&g_state.job_mutex);
            memcpy(g_state.header, header, 80);
            memcpy(g_state.target, ctx.share_target, 32);
            strncpy(g_state.job_id, job.job_id, sizeof(g_state.job_id) - 1);
            g_state.job_id[sizeof(g_state.job_id) - 1] = '\0';
            strncpy(g_state.extranonce2, extranonce2, sizeof(g_state.extranonce2) - 1);
            g_state.extranonce2[sizeof(g_state.extranonce2) - 1] = '\0';
            g_state.job_version++;
            g_state.valid_job = 1;
            pthread_cond_broadcast(&g_state.job_cond);
            pthread_mutex_unlock(&g_state.job_mutex);

            fprintf(stderr, "\n[power2b-miner] New job: %s (difficulty=%.4f)\n",
                    job.job_id, ctx.difficulty);
        }

        share_entry s;
        while (pop_share(&s)) {
            if (stratum_submit_share(&ctx, s.job_id, s.extranonce2, s.ntime, s.nonce) == 0) {
                share_count++;
            }
        }

        double now = time(NULL);
        if (now - last_stats >= 5.0) {
            print_stats(g_state.total_hashes, share_count,
                        now - start_time, ctx.difficulty, threads);
            last_stats = now;
        }
    }

    fprintf(stderr, "\n[power2b-miner] Shutting down...\n");
    g_state.running = 0;
    pthread_cond_broadcast(&g_state.job_cond);

    for (int i = 0; i < threads; i++) {
        pthread_join(worker_ids[i], NULL);
    }

    stratum_destroy(&ctx);
    free(worker_ids);
    pthread_mutex_destroy(&g_state.job_mutex);
    pthread_cond_destroy(&g_state.job_cond);
    pthread_mutex_destroy(&g_state.share_mutex);

    return 0;
}
