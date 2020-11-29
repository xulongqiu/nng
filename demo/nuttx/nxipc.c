/*
 * Copyright (c) 2020 xiaomi.
 *
 * Unpublished copyright. All rights reserved. This material contains
 * proprietary information that should be used or copied only within
 * xiaomi, except with written permission of xiaomi.
 *
 * @file:    nng_nuttx.c
 * @brief:
 * @author:  xulongqiu@xiaomi.com
 * @version: 1.0
 * @date:    2020-11-18 13:21:27
 */

#include <string.h>
#include <time.h>
#include <poll.h>
#include <sys/time.h>
#include <pthread.h>
#include <sys/prctl.h>
#include <nng/nng.h>
#include <nng/compat/nanomsg/nn.h>
#include <nng/protocol/reqrep0/req.h>
#include <nng/protocol/reqrep0/rep.h>
#include <nng/protocol/pubsub0/pub.h>
#include <nng/protocol/pubsub0/sub.h>

#include "nxipc.h"

#define NNG_NUTTX_TOPIC_NAME_LEN 15

typedef struct nng_trans_hdr {
    int seq;
    int op_code;
    size_t len;
    unsigned char* data[];
} nng_trans_hdr_t;

typedef enum {
    NNG_NUTTX_MODE_REQREP = 0,
    NNG_NUTTX_MODE_PIPELINE,
    NNG_NUTTX_MODE_PAIR,

    NNG_NUTTX_MODE_MAX
} nng_nuttx_mode_t;

typedef enum {
    NNG_NUTTX_TRANS_TYPE_INPROC = 0,
    NNG_NUTTX_TRANS_TYPE_IPC,
    NNG_NUTTX_TRANS_TYPE_TCP,

    NNG_NUTTX_TRANS_TYPE_MAX
} nng_nuttx_trans_type_t;

typedef struct nng_server_ctx {
    char* name;
    nng_socket fd;
    on_transaction on_trans_cb;
    pthread_t tid;
    bool actived;
    void* priv;
} nng_server_ctx_t;

typedef struct nng_client_ctx {
    char* name;
    nng_socket fd;
} nng_client_ctx_t;

typedef struct nng_pub_ctx {
    char* name;
    nng_socket fd;
} nng_pub_ctx_t;

typedef struct nng_sub_ctx {
    char* name;
    nng_socket fd;
    on_topic_listener listener;
    pthread_t tid;
    bool actived;
    void* priv;
} nng_sub_ctx_t;

typedef struct nng_nuttx_topic {
    uint8_t topic[NNG_NUTTX_TOPIC_NAME_LEN + 1];
    size_t content_len;
    uint8_t content[];
} nng_nuttx_topic_t;

const char* const nng_nuttx_trans_prefix_str[] = {
    "inproc://", "ipc://", "tcp://"
};

#define nxipc_log(fmt, args...)  do { fprintf(stderr, fmt, ## args); } while(0)

static void* nng_server_worker(void* arg)
{
    nng_trans_hdr_t* trans_hdr;
    nxipc_trans_data_t trans_data;
    nng_server_ctx_t* ctx = (nng_server_ctx_t*)arg;
    pthread_detach(pthread_self());

    if (NULL == ctx) {
        return NULL;
    }

    prctl(PR_SET_NAME, strstr(ctx->name, "//") + 2, NULL, NULL, NULL);

    while (1) {
        int rc;
        nng_msg* msg = NULL;
        void* body   = NULL;
        rc = nng_recvmsg(ctx->fd, &msg, 0);

        if (rc < 0) {
            nxipc_log("%s: %s\n", __func__, nng_strerror(rc));

            if (rc == EBADF) {
                break;   /* Socket closed by another thread. */
            } else {
                //TODO timeout or others
                break;
            }
        }

        body = nng_msg_body(msg);
        trans_hdr = (nng_trans_hdr_t*)body;
        rc = nng_msg_len(msg);

        if (rc < trans_hdr->len + sizeof(nng_trans_hdr_t)) {
            continue;
        } else {
            if (ctx->on_trans_cb != NULL) {
                trans_data.in = body + sizeof(nng_trans_hdr_t);
                trans_data.in_size = trans_hdr->len;
                trans_hdr->op_code = ctx->on_trans_cb(ctx->priv, trans_hdr->op_code, &trans_data);
            } else {
                trans_hdr->op_code = 0;
            }

            trans_hdr->len = 0;
        }

        // send response
        {
            nng_trans_hdr_t* send_hdr = trans_hdr;
            send_hdr->len = 0;

            nng_msg_clear(msg);
            rc = nng_msg_append(msg, send_hdr, sizeof(nng_trans_hdr_t));

            if (rc != 0) {
                nxipc_log("%s-%d: rc=%d\n", __func__, __LINE__, rc);
            } else if (trans_data.out_size > 0 && trans_data.out != NULL) {
                rc = nng_msg_append(msg, trans_data.out, trans_data.out_size);

                if (rc != 0) {
                    send_hdr->op_code = -ENOMEM;
                    send_hdr->len = 0;
                } else {
                    send_hdr->len = trans_data.out_size;
                }
            }

            rc = nng_sendmsg(ctx->fd, msg, 0);

            if (rc < 0) {
                nxipc_log("%s: %s\n", __func__, nng_strerror(rc));
            }

            if (trans_data.out != NULL) {
                free(trans_data.out);
                trans_data.out = NULL;
                trans_data.out_size = 0;
            }
        }
    }

    return NULL;
}

void* nxipc_server_create(const char* name)
{
    nng_server_ctx_t* ctx = NULL;
    const nng_nuttx_trans_type_t trans_type = NNG_NUTTX_TRANS_TYPE_INPROC;
    int name_len = strlen(name);
    int ret = 0;

    if (name_len <= 0) {
        ret = -EINVAL;
        goto err;
    }

    ctx = calloc(1, sizeof(nng_server_ctx_t));

    if (ctx == NULL) {
        ret = -ENOMEM;
        goto err;
    }

    ctx->name = calloc(1, strlen(nng_nuttx_trans_prefix_str[trans_type]) + name_len + 1);

    if (ctx->name == NULL) {
        ret = -ENOMEM;
        goto err;
    }

    strcpy(ctx->name, nng_nuttx_trans_prefix_str[trans_type]);
    strcat(ctx->name, name);

    if ((ret = nng_rep0_open(&ctx->fd)) != 0) {
        goto err;
    }

    if ((ret = nng_listen(ctx->fd, ctx->name, NULL, 0)) != 0) {
        goto err;
    }

    ctx->actived = true;
    ret = pthread_create(&ctx->tid, NULL, nng_server_worker, (void*)ctx);

    if (0 == ret) {
        return ctx;
    }

err:
    nxipc_log("%s.ret=%d(%s)\n", __func__, ret, nng_strerror(ret));

    if (ctx != NULL) {
        nng_close(ctx->fd);

        if (ctx->name != NULL) {
            free(ctx->name);
        }

        free(ctx);
    }

    return NULL;
}

void nxipc_server_set_transaction_cb(void* nng_server_ctx, on_transaction cb, void* cb_priv)
{
    if (nng_server_ctx != NULL && cb != NULL) {
        nng_server_ctx_t* ctx = (nng_server_ctx_t*)nng_server_ctx;
        ctx->on_trans_cb = cb;
        ctx->priv = cb_priv;
    }
}

int nxipc_server_release(void* nng_server_ctx)
{
    if (nng_server_ctx != NULL) {
        nng_server_ctx_t* ctx = (nng_server_ctx_t*)nng_server_ctx;
        ctx->actived = false;

        nng_close(ctx->fd);

        pthread_join(ctx->tid, NULL);

        if (ctx->name != NULL) {
            free(ctx->name);
            ctx->name = NULL;
        }

        free(ctx);
    }
}


void* nxipc_client_connect(const char* server_name)
{
    int ret = 0;
    nng_client_ctx_t* ctx = NULL;
    int name_len = strlen(server_name);
    const nng_nuttx_trans_type_t trans_type = NNG_NUTTX_TRANS_TYPE_INPROC;

    if (name_len <= 0) {
        ret = -EINVAL;
        goto err;
    }

    ctx = calloc(1, sizeof(nng_server_ctx_t));

    if (ctx == NULL) {
        ret = -ENOMEM;
        goto err;
    }

    ctx->name = calloc(1, strlen(nng_nuttx_trans_prefix_str[trans_type]) + name_len + 1);

    if (ctx->name == NULL) {
        ret = -ENOMEM;
        goto err;
    }

    strcpy(ctx->name, nng_nuttx_trans_prefix_str[trans_type]);
    strcat(ctx->name, server_name);

    if ((ret = nng_req0_open(&ctx->fd)) != 0) {
        goto err;
    }

    if ((ret = nng_dial(ctx->fd, ctx->name, NULL, 0)) != 0) {
        goto err;
    }

    return ctx;

err:
    nxipc_log("%s.ret=%d(%s)\n", __func__, ret, nng_strerror(ret));

    if (ctx != NULL) {
        nng_close(ctx->fd);

        if (ctx->name != NULL) {
            free(ctx->name);
        }

        free(ctx);
    }

    return NULL;
}

int nxipc_client_disconnect(void* nng_client_ctx)
{
    if (nng_client_ctx != NULL) {
        nng_client_ctx_t* ctx = (nng_client_ctx_t*)nng_client_ctx;

        if (ctx->name != NULL) {
            free(ctx->name);
            ctx->name = NULL;
        }

        nng_close(ctx->fd);
        free(ctx);
    }
}

int nxipc_client_transaction(const void* nng_client_ctx, int op_code, const void* in, int in_len, void* out, int out_len)
{
    int ret = 0;
    nng_msg* msg;
    uint8_t* body = NULL;
    nng_trans_hdr_t* trans_hdr = NULL;
    nng_client_ctx_t* ctx = (nng_client_ctx_t*)nng_client_ctx;

    if (NULL == ctx) {
        ret = -EINVAL;
        goto out;
    }

    nng_msg_alloc(&msg, sizeof(nng_trans_hdr_t));

    if (msg == NULL) {
        ret = -ENOMEM;
        goto out;
    }

    trans_hdr = nng_msg_body(msg);
    trans_hdr->len = in_len;
    trans_hdr->op_code = op_code;

    if (in != NULL && in_len > 0) {
        nng_msg_append(msg, in, in_len);
    }

    ret = nng_sendmsg(ctx->fd, msg, 0);

    if (ret < 0) {
        nxipc_log("%s: %s\n", __func__, nng_strerror(ret));
        goto out;
    }

    ret = nng_recvmsg(ctx->fd, &msg, 0);

    if (ret < 0) {
        nxipc_log("client_recv: %s\n", nng_strerror(ret));
        goto out;
    }

    ret = nng_msg_len(msg);
    body = nng_msg_body(msg);

    if (ret < sizeof(nng_trans_hdr_t)) {
        ret = -EINVAL;
    } else {
        trans_hdr = (nng_trans_hdr_t*)body;

        if (out != NULL && out_len > 0 && ret > sizeof(nng_trans_hdr_t)) {
            memcpy(out, body + sizeof(nng_trans_hdr_t), out_len > trans_hdr->len ? trans_hdr->len : out_len);
        }

        ret = trans_hdr->op_code;
    }

out:
    nng_msg_free(msg);

    return ret;
}

void* nxipc_pub_create(const char* name)
{
    int ret = 0;
    nng_pub_ctx_t* ctx = NULL;
    int name_len = strlen(name);
    const nng_nuttx_trans_type_t trans_type = NNG_NUTTX_TRANS_TYPE_INPROC;

    if (name_len <= 0) {
        ret = -EINVAL;
        goto err;
    }

    ctx = calloc(1, sizeof(nng_server_ctx_t));

    if (ctx == NULL) {
        ret = -ENOMEM;
        goto err;
    }

    ctx->name = calloc(1, strlen(nng_nuttx_trans_prefix_str[trans_type]) + name_len + 1);

    if (ctx->name == NULL) {
        ret = -ENOMEM;
        goto err;
    }

    strcpy(ctx->name, nng_nuttx_trans_prefix_str[trans_type]);
    strcat(ctx->name, name);

    if ((ret = nng_pub0_open(&ctx->fd)) != 0) {
        goto err;
    }

    if ((ret = nng_listen(ctx->fd, ctx->name, NULL, 0)) != 0) {
        goto err;
    }

    return ctx;
err:
    nxipc_log("%s.ret=%d(%s)\n", __func__, ret, nng_strerror(ret));

    if (ctx != NULL) {
        nng_close(ctx->fd);

        if (ctx->name != NULL) {
            free(ctx->name);
        }

        free(ctx);
    }

    return NULL;
}

int nxipc_pub_release(void* nng_pub_ctx)
{
    if (nng_pub_ctx != NULL) {
        nng_pub_ctx_t* ctx = (nng_pub_ctx_t*)nng_pub_ctx;

        nng_close(ctx->fd);

        if (ctx->name != NULL) {
            free(ctx->name);
            ctx->name = NULL;
        }

        free(ctx);
    }
}



int nxipc_pub_topic_msg(void* nng_pub_ctx, const void* topic, size_t topic_len, const void* content, size_t content_len)
{
    int ret = 0;
    int send_len = 0;
    nng_pub_ctx_t* ctx = (nng_pub_ctx_t*)nng_pub_ctx;
    nng_nuttx_topic_t* topic_data;
    nng_msg* msg = NULL;

    if (topic == NULL || ctx == NULL /*|| ctx->proto != NN_PUB*/) {
        return -EINVAL;
    }

    nng_msg_alloc(&msg, sizeof(nng_nuttx_topic_t));
    topic_data = (nng_nuttx_topic_t*)nng_msg_body(msg);

    if (topic_data == NULL || msg == NULL) {
        nng_msg_free(msg);
        return -ENOMEM;
    }

    memcpy(topic_data->topic, topic, topic_len > NNG_NUTTX_TOPIC_NAME_LEN ? NNG_NUTTX_TOPIC_NAME_LEN : topic_len);
    topic_data->content_len = content_len;
    nng_msg_append(msg, content, content_len);
    ret = nng_sendmsg(ctx->fd, msg, 0);

    if (ret != 0) {
        nxipc_log("%s.ret=%d, send_len=%d\n", __func__, ret, send_len);
        return -1;
    }

    return ret;
}

static void* nng_sub_worker(void* arg)
{
    nng_sub_ctx_t* ctx = (nng_sub_ctx_t*)arg;
    pthread_detach(pthread_self());

    if (NULL == ctx) {
        return NULL;
    }

    prctl(PR_SET_NAME, strstr(ctx->name, "//") + 2, NULL, NULL, NULL);

    while (1) {
        int rc;
        nng_msg* msg = NULL;
        nng_nuttx_topic_t* topic;

        rc = nng_recvmsg(ctx->fd, &msg, 0);

        if (rc < 0) {
            nxipc_log("%s: %s\n", __func__, nng_strerror(rc));

            if (rc == EBADF) {
                break;   /* Socket closed by another thread. */
            } else {
                //TODO timeout or others
                break;
            }
        }

        topic = (nng_nuttx_topic_t*)nng_msg_body(msg);
        rc = nng_msg_len(msg);

        if (rc < sizeof(nng_nuttx_topic_t) + topic->content_len) {
            nxipc_log("%s.content_len=%lu, rc=%d\n", __func__, topic->content_len, rc);
            nng_msg_free(msg);
            continue;
        } else {
            if (ctx->listener != NULL) {
                ctx->listener(ctx->priv, topic->topic, NNG_NUTTX_TOPIC_NAME_LEN, topic->content, topic->content_len);
            }
        }

        nng_msg_free(msg);
    }

    return NULL;
}

void* nxipc_sub_connect(const char* name, on_topic_listener listener, void* listener_priv)
{
    int ret = 0;
    nng_sub_ctx_t* ctx = NULL;
    int name_len = strlen(name);
    const nng_nuttx_trans_type_t trans_type = NNG_NUTTX_TRANS_TYPE_INPROC;

    if (name_len <= 0) {
        ret = -EINVAL;
        goto err;
    }

    ctx = calloc(1, sizeof(nng_sub_ctx_t));

    if (ctx == NULL) {
        ret = -ENOMEM;
        goto err;
    }

    ctx->name = calloc(1, strlen(nng_nuttx_trans_prefix_str[trans_type]) + name_len + 1);

    if (ctx->name == NULL) {
        ret = -ENOMEM;
        goto err;
    }

    strcpy(ctx->name, nng_nuttx_trans_prefix_str[trans_type]);
    strcat(ctx->name, name);

    if ((ret = nng_sub0_open(&ctx->fd)) != 0) {
        goto err;
    }

    if ((ret = nng_dial(ctx->fd, ctx->name, NULL, 0)) != 0) {
        goto err;
    }

    if (ret < 0) {
        goto err;
    }

    ctx->listener = listener;
    ctx->priv = listener_priv;

    ret = pthread_create(&ctx->tid, NULL, nng_sub_worker, (void*)ctx);

    if (0 == ret) {
        return ctx;
    }

err:
    nxipc_log("%s.ret=%d(%s)\n", __func__, ret, nng_strerror(ret));

    if (ctx != NULL) {
        nng_close(ctx->fd);

        if (ctx->name != NULL) {
            free(ctx->name);
        }

        free(ctx);
    }

    return NULL;
}

void nxipc_sub_disconnect(void* nng_sub_ctx)
{
    if (nng_sub_ctx != NULL) {
        nng_sub_ctx_t* ctx = (nng_sub_ctx_t*)nng_sub_ctx;
        ctx->actived = false;

        nng_close(ctx->fd);
        pthread_join(ctx->tid, NULL);

        if (ctx->name != NULL) {
            free(ctx->name);
            ctx->name = NULL;
        }

        free(ctx);
    }
}


int nxipc_sub_register_topic(void* nng_sub_ctx, const void* topic, size_t topic_len)
{
    int ret = 0;
    nng_sub_ctx_t* ctx = (nng_sub_ctx_t*)nng_sub_ctx;

    if (topic == NULL || ctx == NULL /* || ctx->proto != NN_SUB*/) {
        return -EINVAL;
    }

    if (topic_len > NNG_NUTTX_TOPIC_NAME_LEN) {
        nxipc_log("%s.topic len(%lu) > max(%d)\n", __func__, topic_len, NNG_NUTTX_TOPIC_NAME_LEN);
        topic_len = NNG_NUTTX_TOPIC_NAME_LEN;
    }

    ret = nng_setopt(ctx->fd, NNG_OPT_SUB_SUBSCRIBE, topic, topic_len);

    if (ret < 0) {
        nxipc_log("%s.ret=%d(%s)\n", __func__, ret, nng_strerror(ret));
    }

    return ret;
}

int nxipc_sub_unregister_topic(void* nng_sub_ctx, const void* topic, size_t topic_len)
{
    int ret = 0;
    nng_sub_ctx_t* ctx = (nng_sub_ctx_t*)nng_sub_ctx;

    if (topic == NULL || ctx == NULL /* || ctx->proto != NN_SUB*/) {
        return -EINVAL;
    }

    if (topic_len > NNG_NUTTX_TOPIC_NAME_LEN) {
        nxipc_log("%s.topic len(%lu) > max(%d)\n", __func__, topic_len, NNG_NUTTX_TOPIC_NAME_LEN);
        topic_len = NNG_NUTTX_TOPIC_NAME_LEN;
    }

    ret = nng_setopt(ctx->fd, NNG_OPT_SUB_UNSUBSCRIBE, topic, topic_len);

    if (ret < 0) {
        nxipc_log("%s.ret=%d(%s)\n", __func__, ret, nng_strerror(ret));
    }

    return ret;
}

