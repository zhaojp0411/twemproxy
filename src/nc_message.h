/*
 * twemproxy - A fast and lightweight proxy for memcached protocol.
 * Copyright (C) 2011 Twitter, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef _NC_MESSAGE_H_
#define _NC_MESSAGE_H_

#include <nc_core.h>

typedef void (*msg_parse_t)(struct msg *);

typedef enum msg_type {
    MSG_UNKNOWN,
    MSG_REQ_REDIS_APPEND,        /* redis requests */
    MSG_REQ_REDIS_DECR,
    MSG_REQ_REDIS_DEL,
    MSG_REQ_REDIS_DECRBY,
    MSG_REQ_REDIS_EXISTS,
    MSG_REQ_REDIS_EXPIRE,
    MSG_REQ_REDIS_EXPIREAT,
    MSG_REQ_REDIS_GET,
    MSG_REQ_REDIS_GETBIT,
    MSG_REQ_REDIS_GETRANGE,
    MSG_REQ_REDIS_GETSET,
    MSG_REQ_REDIS_HDEL,
    MSG_REQ_REDIS_HEXISTS,
    MSG_REQ_REDIS_HGET,
    MSG_REQ_REDIS_HGETALL,
    MSG_REQ_REDIS_HINCRBY,
    MSG_REQ_REDIS_HKEYS,
    MSG_REQ_REDIS_HLEN,
    MSG_REQ_REDIS_HMGET,
    MSG_REQ_REDIS_HMSET,
    MSG_REQ_REDIS_HSET,
    MSG_REQ_REDIS_HSETNX,
    MSG_REQ_REDIS_HVALS,
    MSG_REQ_REDIS_INCR,
    MSG_REQ_REDIS_INCRBY,
    MSG_REQ_REDIS_LINDEX,
    MSG_REQ_REDIS_LINSERT,
    MSG_REQ_REDIS_LLEN,
    MSG_REQ_REDIS_LPOP,
    MSG_REQ_REDIS_LPUSH,
    MSG_REQ_REDIS_LPUSHX,
    MSG_REQ_REDIS_LRANGE,
    MSG_REQ_REDIS_LREM,
    MSG_REQ_REDIS_LSET,
    MSG_REQ_REDIS_LTRIM,
    MSG_REQ_REDIS_MOVE,
    MSG_REQ_REDIS_PERSIST,
    MSG_REQ_REDIS_RPOP,
    MSG_REQ_REDIS_RPUSH,
    MSG_REQ_REDIS_RPUSHX,
    MSG_REQ_REDIS_SADD,
    MSG_REQ_REDIS_SCARD,
    MSG_REQ_REDIS_SET,
    MSG_REQ_REDIS_SETBIT,
    MSG_REQ_REDIS_SETEX,
    MSG_REQ_REDIS_SETNX,
    MSG_REQ_REDIS_SETRANGE,
    MSG_REQ_REDIS_SISMEMBER,
    MSG_REQ_REDIS_SMEMBERS,
    MSG_REQ_REDIS_SPOP,
    MSG_REQ_REDIS_SRANDMEMBER,
    MSG_REQ_REDIS_SREM,
    MSG_REQ_REDIS_STRLEN,
    MSG_REQ_REDIS_TTL,
    MSG_REQ_REDIS_TYPE,
    MSG_REQ_REDIS_MGET,
    MSG_RSP_REDIS_STATUS,        /* redis response */
    MSG_RSP_REDIS_ERROR,
    MSG_RSP_REDIS_INTEGER,
    MSG_RSP_REDIS_BULK,
    MSG_RSP_REDIS_MULTIBULK,
    MSG_SENTINEL
} msg_type_t;

struct msg {
    TAILQ_ENTRY(msg) c_tqe;           /* link in client q */
    TAILQ_ENTRY(msg) s_tqe;           /* link in server q */
    TAILQ_ENTRY(msg) m_tqe;           /* link in send q / free q */

    uint64_t         id;              /* message id */
    struct msg       *peer;           /* message peer */
    struct conn      *owner;          /* message owner - client | server */

    struct rbnode    tmo_rbe;         /* entry in rbtree */

    struct mhdr      mhdr;            /* message mbuf header */
    uint32_t         mlen;            /* message length */

    int              state;           /* current parser state */
    uint8_t          *pos;            /* parser position marker */
    uint8_t          *token;          /* contiguous token marker used by parser fsa */

    msg_parse_t      parse;           /* message parsing handler */
    int              result;          /* message parsing result */

    msg_type_t       type;            /* message type */

    uint8_t          *key_start;      /* key start */
    uint8_t          *key_end;        /* key end */

    uint8_t          *narg_start;     /* narg start */
    uint8_t          *narg_end;       /* narg end */
    uint32_t         narg;            /* # arguments */

    uint32_t         rnarg;           /* running # arg used by parsing fsa */
    uint32_t         rlen;            /* running length in parsing fsa */

    struct msg       *frag_owner;     /* owner of fragment message */
    uint32_t         nfrag;           /* # fragment */
    uint64_t         frag_id;         /* id of fragmented message */

    err_t            err;             /* errno on error? */
    unsigned         error:1;         /* error? */
    unsigned         ferror:1;        /* one or more fragments are in error? */
    unsigned         request:1;       /* request? or response? */
    unsigned         quit:1;          /* quit request? */
    unsigned         noreply:1;       /* noreply? */
    unsigned         done:1;          /* done? */
    unsigned         fdone:1;         /* all fragments are done? */
    unsigned         first_fragment:1;/* first fragment of retrieval request? */
    unsigned         last_fragment:1; /* last fragment of fragmented request? */
    unsigned         swallow:1;       /* swallow response? */
};

TAILQ_HEAD(msg_tqh, msg);

struct msg *msg_tmo_min(void);
void msg_tmo_insert(struct msg *msg, struct conn *conn);
void msg_tmo_delete(struct msg *msg);

void msg_init(void);
void msg_deinit(void);
struct msg *msg_get(struct conn *conn, bool request);
void msg_put(struct msg *msg);
struct msg *msg_get_error(err_t err);
void msg_dump(struct msg *msg);
bool msg_empty(struct msg *msg);
rstatus_t msg_recv(struct context *ctx, struct conn *conn);
rstatus_t msg_send(struct context *ctx, struct conn *conn);

struct msg *req_get(struct conn *conn);
void req_put(struct msg *msg);
bool req_done(struct conn *conn, struct msg *msg);
bool req_error(struct conn *conn, struct msg *msg);
void req_server_enqueue_imsgq(struct context *ctx, struct conn *conn, struct msg *msg);
void req_server_dequeue_imsgq(struct context *ctx, struct conn *conn, struct msg *msg);
void req_client_enqueue_omsgq(struct context *ctx, struct conn *conn, struct msg *msg);
void req_server_enqueue_omsgq(struct context *ctx, struct conn *conn, struct msg *msg);
void req_client_dequeue_omsgq(struct context *ctx, struct conn *conn, struct msg *msg);
void req_server_dequeue_omsgq(struct context *ctx, struct conn *conn, struct msg *msg);
struct msg *req_recv_next(struct context *ctx, struct conn *conn, bool alloc);
void req_recv_done(struct context *ctx, struct conn *conn, struct msg *msg, struct msg *nmsg);
struct msg *req_send_next(struct context *ctx, struct conn *conn);
void req_send_done(struct context *ctx, struct conn *conn, struct msg *msg);

struct msg *rsp_get(struct conn *conn);
void rsp_put(struct msg *msg);
struct msg *rsp_recv_next(struct context *ctx, struct conn *conn, bool alloc);
void rsp_recv_done(struct context *ctx, struct conn *conn, struct msg *msg, struct msg *nmsg);
struct msg *rsp_send_next(struct context *ctx, struct conn *conn);
void rsp_send_done(struct context *ctx, struct conn *conn, struct msg *msg);

#endif
