#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>

#include "stratum.h"
#include "json.h"
#include "sha256.h"

static int hex_char_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int hex_decode(const char *hex, uint8_t *out, int maxlen) {
    int len = 0;
    while (hex[0] && hex[1] && len < maxlen) {
        int hi = hex_char_val(hex[0]);
        int lo = hex_char_val(hex[1]);
        if (hi < 0 || lo < 0) break;
        out[len++] = (hi << 4) | lo;
        hex += 2;
    }
    return len;
}

void hex_encode(const uint8_t *data, int len, char *out) {
    if (!data || !out) return;
    static const char *hex = "0123456789abcdef";
    for (int i = 0; i < len; i++) {
        out[i * 2] = hex[(data[i] >> 4) & 0xf];
        out[i * 2 + 1] = hex[data[i] & 0xf];
    }
    out[len * 2] = '\0';
}

void difficulty_to_target(double diff, uint8_t target[32]) {
    memset(target, 0, 32);
    if (diff < 1.0) diff = 1.0;
    uint64_t m = 0x00FFFF0000000000ULL;
    double d = (double)m / diff;
    uint64_t val = (uint64_t)d;
    for (int i = 7; i >= 0; i--) {
        target[31 - i] = val & 0xff;
        val >>= 8;
    }
}

int check_share(const uint8_t hash[32], const uint8_t target[32]) {
    for (int i = 31; i >= 0; i--) {
        if (hash[i] < target[i]) return 1;
        if (hash[i] > target[i]) return 0;
    }
    return 1;
}

static int compute_merkle_root(const uint8_t coinbase_hash[32],
                               uint8_t merkle_branch[][32], int count,
                               uint8_t root[32]) {
    memcpy(root, coinbase_hash, 32);
    uint8_t buf[64];
    for (int i = 0; i < count; i++) {
        memcpy(buf, root, 32);
        memcpy(buf + 32, merkle_branch[i], 32);
        sha256d_hash(root, buf, 64);
    }
    return 0;
}

int build_block_header(stratum_ctx *ctx, const char *extranonce2,
                       uint8_t header[80], uint32_t nonce) {
    stratum_job job;
    pthread_mutex_lock(&ctx->job_mutex);
    memcpy(&job, &ctx->current_job, sizeof(job));
    pthread_mutex_unlock(&ctx->job_mutex);

    uint8_t coinbase[1024];
    int coinbase_len = 0;

    memcpy(coinbase + coinbase_len, job.coinb1, job.coinb1_len);
    coinbase_len += job.coinb1_len;

    uint8_t en1[32];
    int en1_len = hex_decode(ctx->extranonce1, en1, sizeof(en1));
    memcpy(coinbase + coinbase_len, en1, en1_len);
    coinbase_len += en1_len;

    uint8_t en2[32];
    int en2_len = hex_decode(extranonce2, en2, sizeof(en2));
    memcpy(coinbase + coinbase_len, en2, en2_len);
    coinbase_len += en2_len;

    memcpy(coinbase + coinbase_len, job.coinb2, job.coinb2_len);
    coinbase_len += job.coinb2_len;

    uint8_t coinbase_hash[32];
    sha256d_hash(coinbase_hash, coinbase, coinbase_len);

    uint8_t merkle_root[32];
    compute_merkle_root(coinbase_hash, job.merkle_branch,
                        job.merkle_count, merkle_root);

    header[0] = job.version & 0xff;
    header[1] = (job.version >> 8) & 0xff;
    header[2] = (job.version >> 16) & 0xff;
    header[3] = (job.version >> 24) & 0xff;

    memcpy(header + 4, job.prevhash, 32);
    memcpy(header + 36, merkle_root, 32);

    header[68] = job.ntime & 0xff;
    header[69] = (job.ntime >> 8) & 0xff;
    header[70] = (job.ntime >> 16) & 0xff;
    header[71] = (job.ntime >> 24) & 0xff;

    header[72] = (job.nbits >> 24) & 0xff;
    header[73] = (job.nbits >> 16) & 0xff;
    header[74] = (job.nbits >> 8) & 0xff;
    header[75] = job.nbits & 0xff;

    header[76] = nonce & 0xff;
    header[77] = (nonce >> 8) & 0xff;
    header[78] = (nonce >> 16) & 0xff;
    header[79] = (nonce >> 24) & 0xff;

    return 0;
}

static int tcp_connect(const char *host, int port, int timeout_sec) {
    struct hostent *he = gethostbyname(host);
    if (!he) return -1;

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;

    int flag = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);

    struct timeval tv;
    tv.tv_sec = timeout_sec;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock);
        return -1;
    }

    return sock;
}

static int send_line(int sock, const char *line) {
    int len = strlen(line);
    int sent = 0;
    while (sent < len) {
        int n = write(sock, line + sent, len - sent);
        if (n <= 0) return -1;
        sent += n;
    }
    if (write(sock, "\n", 1) != 1) return -1;
    return 0;
}

int stratum_connect(stratum_ctx *ctx, const char *host, int port,
                    const char *worker, const char *password) {
    memset(ctx, 0, sizeof(*ctx));
    pthread_mutex_init(&ctx->job_mutex, NULL);
    strncpy(ctx->host, host, sizeof(ctx->host) - 1);
    ctx->port = port;
    strncpy(ctx->worker, worker, sizeof(ctx->worker) - 1);
    strncpy(ctx->password, password, sizeof(ctx->password) - 1);
    ctx->difficulty = 1.0;
    difficulty_to_target(ctx->difficulty, ctx->share_target);
    ctx->submit_id = 1;

    ctx->sock = tcp_connect(host, port, 10);
    if (ctx->sock < 0) {
        pthread_mutex_destroy(&ctx->job_mutex);
        return -1;
    }
    ctx->connected = 1;
    return 0;
}

void stratum_destroy(stratum_ctx *ctx) {
    if (ctx->sock >= 0) close(ctx->sock);
    ctx->connected = 0;
    pthread_mutex_destroy(&ctx->job_mutex);
}

int stratum_subscribe(stratum_ctx *ctx) {
    char buf[512];
    snprintf(buf, sizeof(buf),
        "{\"id\":1,\"method\":\"mining.subscribe\",\"params\":[\"power2b-miner/1.0\"]}");
    return send_line(ctx->sock, buf);
}

int stratum_authorize(stratum_ctx *ctx) {
    char buf[1024];
    snprintf(buf, sizeof(buf),
        "{\"id\":2,\"method\":\"mining.authorize\",\"params\":[\"%s\",\"%s\"]}",
        ctx->worker, ctx->password);
    return send_line(ctx->sock, buf);
}

static int read_sock(stratum_ctx *ctx, int timeout_ms) {
    struct timeval tv;
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(ctx->sock, &fds);
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    int ret = select(ctx->sock + 1, &fds, NULL, NULL, &tv);
    if (ret <= 0) return ret;

    int n = read(ctx->sock, ctx->recv_buf + ctx->recv_len,
                 sizeof(ctx->recv_buf) - ctx->recv_len - 1);
    if (n <= 0) return -1;
    ctx->recv_len += n;
    ctx->recv_buf[ctx->recv_len] = '\0';
    return n;
}

int stratum_read_line(stratum_ctx *ctx, char *buf, int bufsize, int timeout_ms) {
    char *nl = NULL;
    if (ctx->recv_len > 0) {
        ctx->recv_buf[ctx->recv_len] = '\0';
        nl = strchr(ctx->recv_buf, '\n');
    }
    if (!nl && timeout_ms > 0) {
        int ret = read_sock(ctx, timeout_ms);
        if (ret <= 0) return ret;
        ctx->recv_buf[ctx->recv_len] = '\0';
        nl = strchr(ctx->recv_buf, '\n');
    }
    if (nl) {
        int len = nl - ctx->recv_buf;
        if (len > ctx->recv_len) len = ctx->recv_len;
        if (len >= bufsize) len = bufsize - 1;
        memcpy(buf, ctx->recv_buf, len);
        buf[len] = '\0';
        int remaining = ctx->recv_len - len - 1;
        if (remaining > 0) {
            memmove(ctx->recv_buf, nl + 1, remaining);
        }
        ctx->recv_len -= (len + 1);
        ctx->recv_buf[ctx->recv_len > 0 ? ctx->recv_len : 0] = '\0';
        return len;
    }
    return 0;
}

int stratum_get_job(stratum_ctx *ctx, stratum_job *job) {
    int has_new = 0;
    pthread_mutex_lock(&ctx->job_mutex);
    if (ctx->new_job_flag) {
        memcpy(job, &ctx->current_job, sizeof(*job));
        ctx->new_job_flag = 0;
        has_new = 1;
    }
    pthread_mutex_unlock(&ctx->job_mutex);
    return has_new;
}

int stratum_submit_share(stratum_ctx *ctx, const char *job_id,
                         const char *extranonce2, const char *ntime,
                         const char *nonce) {
    char buf[1024];
    int id = __sync_fetch_and_add(&ctx->submit_id, 1);
    snprintf(buf, sizeof(buf),
        "{\"id\":%d,\"method\":\"mining.submit\",\"params\":[\"%s\",\"%s\",\"%s\",\"%s\",\"%s\"]}",
        id, ctx->worker, job_id, extranonce2, ntime, nonce);
    if (send_line(ctx->sock, buf) < 0) return -1;
    ctx->share_count++;
    return 0;
}

static int handle_notify(stratum_ctx *ctx, json_parser *p, json_token *params) {
    if (!params || params->type != JSON_ARRAY || params->size < 9) return -1;

    stratum_job job;
    memset(&job, 0, sizeof(job));

    const char *s = json_string_val(p, json_get_index(p, params, 0));
    if (!s) return -1;
    strncpy(job.job_id, s, sizeof(job.job_id) - 1);

    s = json_string_val(p, json_get_index(p, params, 1));
    if (!s) return -1;
    hex_decode(s, job.prevhash, 32);

    s = json_string_val(p, json_get_index(p, params, 2));
    if (!s) return -1;
    job.coinb1_len = hex_decode(s, job.coinb1, sizeof(job.coinb1));

    s = json_string_val(p, json_get_index(p, params, 3));
    if (!s) return -1;
    job.coinb2_len = hex_decode(s, job.coinb2, sizeof(job.coinb2));

    json_token *merkle = json_get_index(p, params, 4);
    if (merkle && merkle->type == JSON_ARRAY) {
        job.merkle_count = merkle->size;
        for (int i = 0; i < merkle->size && i < MAX_MERKLE_BRANCH; i++) {
            s = json_string_val(p, json_get_index(p, merkle, i));
            if (s) hex_decode(s, job.merkle_branch[i], 32);
        }
    }

    s = json_string_val(p, json_get_index(p, params, 5));
    if (s) {
        uint8_t tmp[4];
        hex_decode(s, tmp, 4);
        job.version = tmp[0] | (tmp[1] << 8) | (tmp[2] << 16) | (tmp[3] << 24);
    }

    s = json_string_val(p, json_get_index(p, params, 6));
    if (s) {
        uint8_t tmp[4];
        hex_decode(s, tmp, 4);
        job.nbits = (tmp[3] << 24) | (tmp[2] << 16) | (tmp[1] << 8) | tmp[0];
    }

    s = json_string_val(p, json_get_index(p, params, 7));
    if (s) {
        uint8_t tmp[4];
        hex_decode(s, tmp, 4);
        job.ntime = tmp[0] | (tmp[1] << 8) | (tmp[2] << 16) | (tmp[3] << 24);
    }

    job.clean_jobs = json_int_val(p, json_get_index(p, params, 8));

    pthread_mutex_lock(&ctx->job_mutex);
    memcpy(&ctx->current_job, &job, sizeof(job));
    ctx->has_job = 1;
    ctx->new_job_flag = 1;
    pthread_mutex_unlock(&ctx->job_mutex);

    return 0;
}

static int handle_set_difficulty(stratum_ctx *ctx, json_parser *p, json_token *params) {
    if (!params || params->type != JSON_ARRAY || params->size < 1) return -1;
    json_token *diff = json_get_index(p, params, 0);
    if (!diff) return -1;
    double d = json_double_val(p, diff);
    if (d < 0.001) d = 0.001;
    ctx->difficulty = d;
    difficulty_to_target(ctx->difficulty, ctx->share_target);
    return 0;
}

int stratum_process_message(stratum_ctx *ctx, const char *line) {
    char *buf = strdup(line);
    if (!buf) return -1;

    json_parser p;
    if (json_parse(&p, buf) < 0) {
        free(buf);
        return -1;
    }

    json_token *root = &p.tokens[0];
    if (!root || root->type != JSON_OBJECT) {
        free(buf);
        return -1;
    }

    json_token *method = json_get_key(&p, root, "method");
    json_token *id_token = json_get_key(&p, root, "id");

    if (method) {
        const char *method_str = json_string_val(&p, method);
        if (!method_str) { free(buf); return -1; }
        json_token *params = json_get_key(&p, root, "params");

        if (strcmp(method_str, "mining.notify") == 0) {
            handle_notify(ctx, &p, params);
        } else if (strcmp(method_str, "mining.set_difficulty") == 0) {
            handle_set_difficulty(ctx, &p, params);
        } else if (strcmp(method_str, "mining.set_extranonce") == 0) {
            if (params && params->type == JSON_ARRAY && params->size >= 1) {
                const char *en1 = json_string_val(&p, json_get_index(&p, params, 0));
                if (en1) {
                    strncpy(ctx->extranonce1, en1, sizeof(ctx->extranonce1) - 1);
                    ctx->extranonce1_len = strlen(en1) / 2;
                }
                if (params->size >= 2) {
                    ctx->extranonce2_size = json_int_val(&p, json_get_index(&p, params, 1));
                }
            }
        }
    }

    if (id_token) {
        int id = json_int_val(&p, id_token);
        json_token *result = json_get_key(&p, root, "result");
        if (id == 1 && result) {
            ctx->subscribed = 1;
            if (result->type == JSON_ARRAY && result->size >= 3) {
                const char *en1 = json_string_val(&p, json_get_index(&p, result, 1));
                if (en1) {
                    strncpy(ctx->extranonce1, en1, sizeof(ctx->extranonce1) - 1);
                    ctx->extranonce1_len = strlen(en1) / 2;
                }
                ctx->extranonce2_size = json_int_val(&p, json_get_index(&p, result, 2));
            }
            fprintf(stderr, "[stratum] subscribed, extranonce1=%s size=%d\n",
                    ctx->extranonce1, ctx->extranonce2_size);
        } else if (id == 2 && result) {
            ctx->authorized = json_int_val(&p, result);
            fprintf(stderr, "[stratum] authorized: %s\n",
                    ctx->authorized ? "yes" : "no");
        }
    }

    free(buf);
    return 0;
}
