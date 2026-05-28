#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>

#include "stratum.h"
#include "yespower.h"

#define POWER2B_PERS "Client Key"

static volatile int running = 1;

static void handle_signal(int sig) {
    (void)sig;
    running = 0;
}

static void print_stats(long long total_hashes, long long shares,
                        double elapsed, double diff) {
    double rate = elapsed > 0 ? total_hashes / elapsed : 0;
    fprintf(stderr, "\r[power2b-miner] %.2f H/s  shares=%lld  diff=%.1f  total=%lld  ",
            rate, shares, diff, total_hashes);
    fflush(stderr);
}

static void generate_extranonce2(char *out, int bytes) {
    static int seed = 0;
    if (!seed) {
        seed = time(NULL) ^ (getpid() << 16);
    }
    for (int i = 0; i < bytes; i++) {
        seed = seed * 1103515245 + 12345;
        out[i * 2] = "0123456789abcdef"[(seed >> 20) & 0xf];
        out[i * 2 + 1] = "0123456789abcdef"[(seed >> 16) & 0xf];
    }
    out[bytes * 2] = '\0';
}

int main(int argc, char *argv[]) {
    const char *host = "power2b.eu.mine.zpool.ca";
    int port = 6242;
    const char *worker = "33AUHjnweSG9nz93JJqrsY97EN6o8bNRUC";
    const char *password = "c=BTC";

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
        } else if (strcmp(argv[i], "-h") == 0) {
            fprintf(stderr, "Usage: %s -o stratum+tcp://host:port -u worker -p password\n", argv[0]);
            return 0;
        }
    }

    fprintf(stderr, "[power2b-miner] Connecting to %s:%d ...\n", host, port);

    stratum_ctx ctx;
    if (stratum_connect(&ctx, host, port, worker, password) < 0) {
        fprintf(stderr, "[power2b-miner] Failed to connect\n");
        return 1;
    }

    fprintf(stderr, "[power2b-miner] Connected, subscribing...\n");

    if (stratum_subscribe(&ctx) < 0) {
        fprintf(stderr, "[power2b-miner] Subscribe failed\n");
        stratum_destroy(&ctx);
        return 1;
    }

    int auth_sent = 0;
    char line[65536];
    long long total_hashes = 0;
    double start_time = time(NULL);
    double last_stats = start_time;

    yespower_params_t params;
    memset(&params, 0, sizeof(params));
    params.N = 2048;
    params.r = 32;
    params.pers = (const uint8_t *)POWER2B_PERS;
    params.perslen = strlen(POWER2B_PERS);

    yespower_binary_t hash;
    uint8_t header[80];
    char extranonce2[64];
    uint32_t nonce = 0;
    int has_job = 0;
    char current_job_id[128] = "";
    int reconnect_needed = 0;

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    while (running) {
        if (reconnect_needed) {
            fprintf(stderr, "\n[power2b-miner] Reconnecting...\n");
            stratum_destroy(&ctx);
            if (stratum_connect(&ctx, host, port, worker, password) < 0) {
                fprintf(stderr, "[power2b-miner] Reconnect failed, retrying in 10s\n");
                sleep(10);
                continue;
            }
            stratum_subscribe(&ctx);
            auth_sent = 0;
            has_job = 0;
            reconnect_needed = 0;
        }

        int ret = stratum_read_line(&ctx, line, sizeof(line), 100);
        if (ret < 0) {
            fprintf(stderr, "\n[power2b-miner] Connection lost\n");
            reconnect_needed = 1;
            continue;
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

            strncpy(current_job_id, job.job_id, sizeof(current_job_id) - 1);
            generate_extranonce2(extranonce2, ctx.extranonce2_size > 0 ? ctx.extranonce2_size : 4);
            build_block_header(&ctx, extranonce2, header, 0);
            nonce = 0;
            has_job = 1;

            fprintf(stderr, "\n[power2b-miner] New job: %s (difficulty=%.1f)\n",
                    job.job_id, ctx.difficulty);
        }

        if (has_job && ctx.authorized) {
            header[76] = nonce & 0xff;
            header[77] = (nonce >> 8) & 0xff;
            header[78] = (nonce >> 16) & 0xff;
            header[79] = (nonce >> 24) & 0xff;

            if (yespower_tls(header, 80, &params, &hash) != 0) {
                fprintf(stderr, "\n[power2b-miner] yespower failed\n");
                continue;
            }

            total_hashes++;

            if (check_share(hash.uc, ctx.share_target)) {
                char nonce_hex[16];
                char ntime_hex[16];
                hex_encode(&header[68], 4, ntime_hex);
                hex_encode(&header[76], 4, nonce_hex);

                fprintf(stderr, "\n[power2b-miner] *** SHARE FOUND *** nonce=%s\n", nonce_hex);
                stratum_submit_share(&ctx, current_job_id, extranonce2,
                                     ntime_hex, nonce_hex);
            }

            nonce++;

            double now = time(NULL);
            if (now - last_stats >= 5.0) {
                print_stats(total_hashes, ctx.share_count,
                            now - start_time, ctx.difficulty);
                last_stats = now;
            }
        } else {
            struct timespec ts = {0, 10000000L};
            nanosleep(&ts, NULL);
        }
    }

    fprintf(stderr, "\n[power2b-miner] Shutting down...\n");
    stratum_destroy(&ctx);
    return 0;
}
