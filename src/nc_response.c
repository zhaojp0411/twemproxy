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

#include <nc_core.h>
#include <nc_server.h>
#include <nc_event.h>

struct msg *
rsp_get(struct conn *conn)
{
    struct msg *msg;

    ASSERT(!conn->client && !conn->proxy);

    msg = msg_get(conn, false);
    if (msg == NULL) {
        conn->err = errno;
    }

    return msg;
}

void
rsp_put(struct msg *msg)
{
    ASSERT(!msg->request);
    ASSERT(msg->peer == NULL);
    msg_put(msg);
}

static struct msg *
rsp_make_error(struct context *ctx, struct conn *conn, struct msg *msg)
{
    struct msg *pmsg;        /* peer message (response) */
    struct msg *cmsg, *nmsg; /* current and next message (request) */
    uint64_t id;
    err_t err;

    ASSERT(conn->client && !conn->proxy);
    ASSERT(msg->request && req_error(conn, msg));
    ASSERT(msg->owner == conn);

    id = msg->frag_id;
    if (id != 0) {
        for (err = 0, cmsg = TAILQ_NEXT(msg, c_tqe);
             cmsg != NULL && cmsg->frag_id == id;
             cmsg = nmsg) {
            nmsg = TAILQ_NEXT(cmsg, c_tqe);

            /* dequeue request (error fragment) from client outq */
            conn->dequeue_outq(ctx, conn, cmsg);
            if (err == 0 && cmsg->err != 0) {
                err = cmsg->err;
            }

            req_put(cmsg);
        }
    } else {
        err = msg->err;
    }

    pmsg = msg->peer;
    if (pmsg != NULL) {
        ASSERT(!pmsg->request && pmsg->peer == msg);
        msg->peer = NULL;
        pmsg->peer = NULL;
        rsp_put(pmsg);
    }

    return msg_get_error(err);
}

struct msg *
rsp_recv_next(struct context *ctx, struct conn *conn, bool alloc)
{
    struct msg *msg;

    ASSERT(!conn->client && !conn->proxy);
    ASSERT(!conn->connecting);

    if (conn->eof) {
        msg = conn->rmsg;

        /* server sent eof before sending the entire request */
        if (msg != NULL) {
            conn->rmsg = NULL;

            ASSERT(msg->peer == NULL);
            ASSERT(!msg->request);

            log_error("eof s %d discarding incomplete rsp %"PRIu64" len "
                      "%"PRIu32"", conn->sd, msg->id, msg->mlen);

            rsp_put(msg);
        }

        /*
         * We treat TCP half-close from a server different from how we treat
         * those from a client. On a FIN from a server, we close the connection
         * immediately by sending the second FIN even if there were outstanding
         * or pending requests. This is actually a tricky part in the FA, as
         * we don't expect this to happen unless the server is misbehaving or
         * it crashes
         */
        conn->done = 1;
        log_error("s %d active %d is done", conn->sd, conn->active(conn));

        return NULL;
    }

    msg = conn->rmsg;
    if (msg != NULL) {
        ASSERT(!msg->request);
        return msg;
    }

    if (!alloc) {
        return NULL;
    }

    msg = rsp_get(conn);
    if (msg != NULL) {
        conn->rmsg = msg;
    }

    return msg;
}

static bool
rsp_filter(struct context *ctx, struct conn *conn, struct msg *msg)
{
    struct msg *pmsg;

    ASSERT(!conn->client && !conn->proxy);

    if (msg_empty(msg)) {
        ASSERT(conn->rmsg == NULL);
        log_debug(LOG_VERB, "filter empty rsp %"PRIu64" on s %d", msg->id,
                  conn->sd);
        rsp_put(msg);
        return true;
    }

    pmsg = TAILQ_FIRST(&conn->omsg_q);
    if (pmsg == NULL) {
        log_debug(LOG_ERR, "filter stray rsp %"PRIu64" len %"PRIu32" on s %d",
                  msg->id, msg->mlen, conn->sd);
        rsp_put(msg);
        return true;
    }
    ASSERT(pmsg->peer == NULL);
    ASSERT(pmsg->request && !pmsg->done);

    if (pmsg->swallow) {
        conn->dequeue_outq(ctx, conn, pmsg);
        pmsg->done = 1;

        log_debug(LOG_INFO, "swallow rsp %"PRIu64" len %"PRIu32" of req "
                  "%"PRIu64" on s %d", msg->id, msg->mlen, pmsg->id,
                  conn->sd);

        rsp_put(msg);
        req_put(pmsg);
        return true;
    }

    return false;
}

static void
rsp_forward_stats(struct context *ctx, struct msg *msg, struct conn *s_conn,
                  struct conn *c_conn)
{
    struct msg *pmsg;
    struct server *server;

    ASSERT(!s_conn->client && !s_conn->proxy);
    ASSERT(c_conn->client && !c_conn->proxy);
    ASSERT(!msg->request && msg->peer != NULL);

    server = s_conn->owner;
    pmsg = msg->peer;

    stats_server_incr(ctx, server, responses);
    stats_server_incr_by(ctx, server, response_bytes, msg->mlen);

    switch (msg->type) {
    case MSG_RSP_REDIS_STATUS:
    case MSG_RSP_REDIS_ERROR:
    case MSG_RSP_REDIS_INTEGER:
    case MSG_RSP_REDIS_BULK:
    case MSG_RSP_REDIS_MULTIBULK:
        break;

    default:
        NOT_REACHED();
    }
}

static void
rsp_forward(struct context *ctx, struct conn *s_conn, struct msg *msg)
{
    rstatus_t status;
    struct msg *pmsg;
    struct conn *c_conn;
    struct mbuf *mbuf;

    ASSERT(!s_conn->client && !s_conn->proxy);

    /* response from server implies that server is ok and heartbeating */
    server_ok(ctx, s_conn);

    /* dequeue peer message (request) from server */
    pmsg = TAILQ_FIRST(&s_conn->omsg_q);
    ASSERT(pmsg != NULL && pmsg->peer == NULL);
    ASSERT(pmsg->request && !pmsg->done);

    s_conn->dequeue_outq(ctx, s_conn, pmsg);
    pmsg->done = 1;

    /* establish msg <-> pmsg (response <-> request) link */
    pmsg->peer = msg;
    msg->peer = pmsg;

    if (pmsg->frag_id != 0) {

        switch (msg->type) {
        case MSG_RSP_REDIS_INTEGER:
            /*
             * Only redis 'del' command is a candidate for fragmentation
             * and sends back a integer reply.
             *
             * Because of how we parse replies, the integer reply will be
             * completely encpsuated in a single mbuf and we should skip
             * over all the mbuf contents as the parser has already parsed
             * and stored reply in msg->integer
             */
            mbuf = STAILQ_FIRST(&msg->mhdr);

            ASSERT(pmsg->type == MSG_REQ_REDIS_DEL);
            ASSERT(mbuf == STAILQ_LAST(&msg->mhdr, mbuf, next));
            ASSERT(msg->mlen == mbuf_length(mbuf));

            msg->mlen -= mbuf_length(mbuf);
            mbuf_rewind(mbuf);

            mbuf->pos = mbuf->start;
            mbuf->last = mbuf->pos;

            break;

        case MSG_RSP_REDIS_MULTIBULK:
            /*
             * Only redis 'mget' command is a candidate for fragmentation
             * and sends back a multi-bulk reply
             *
             * The muti-bulk reply can span over multiple mbufs and in each
             * reply we would like to skip over the narg token.
             *
             * Furthermore, because of the way I parse and tokenize replies,
             * the '\r\n' might not exists in a contiguous region.
             */
            mbuf = STAILQ_FIRST(&msg->mhdr);
            ASSERT(msg->narg_start == mbuf->pos);
            msg->narg_end += CRLF_LEN;
            msg->mlen -= (uint32_t) (msg->narg_end - msg->narg_start);
            mbuf->pos = msg->narg_end;
            break;

        default:
            mbuf = STAILQ_FIRST(&msg->mhdr);
            log_hexdump(LOG_ERR, mbuf->pos, mbuf_length(mbuf), "rsp fragment "
                        "with unknown type %d", msg->type);
            pmsg->error = 1;
            pmsg->err = EINVAL;
        }
    }

    c_conn = pmsg->owner;
    ASSERT(c_conn->client && !c_conn->proxy);

    if (req_done(c_conn, TAILQ_FIRST(&c_conn->omsg_q))) {
        status = event_add_out(ctx->ep, c_conn);
        if (status != NC_OK) {
            c_conn->err = errno;
        }
    }

    rsp_forward_stats(ctx, msg, s_conn, c_conn);
}

void
rsp_recv_done(struct context *ctx, struct conn *conn, struct msg *msg,
              struct msg *nmsg)
{
    ASSERT(!conn->client && !conn->proxy);
    ASSERT(msg != NULL && conn->rmsg == msg);
    ASSERT(!msg->request);
    ASSERT(msg->owner == conn);
    ASSERT(nmsg == NULL || !nmsg->request);

    /* enqueue next message (response), if any */
    conn->rmsg = nmsg;

    if (rsp_filter(ctx, conn, msg)) {
        return;
    }

    rsp_forward(ctx, conn, msg);
}

struct msg *
rsp_send_next(struct context *ctx, struct conn *conn)
{
    rstatus_t status;
    struct msg *msg, *pmsg; /* response and it's peer request */

    ASSERT(conn->client && !conn->proxy);

    pmsg = TAILQ_FIRST(&conn->omsg_q);
    if (pmsg == NULL || !req_done(conn, pmsg)) {
        /* nothing is outstanding, initiate close? */
        if (pmsg == NULL && conn->eof) {
            conn->done = 1;
            log_debug(LOG_INFO, "c %d is done", conn->sd);
        }

        status = event_del_out(ctx->ep, conn);
        if (status != NC_OK) {
            conn->err = errno;
        }

        return NULL;
    }

    msg = conn->smsg;
    if (msg != NULL) {
        ASSERT(!msg->request && msg->peer != NULL);
        ASSERT(req_done(conn, msg->peer));
        pmsg = TAILQ_NEXT(msg->peer, c_tqe);
    }

    if (pmsg == NULL || !req_done(conn, pmsg)) {
        conn->smsg = NULL;
        return NULL;
    }
    ASSERT(pmsg->request && !pmsg->swallow);

    if (req_error(conn, pmsg)) {
        msg = rsp_make_error(ctx, conn, pmsg);
        if (msg == NULL) {
            conn->err = errno;
            return NULL;
        }
        msg->peer = pmsg;
        pmsg->peer = msg;
        stats_pool_incr(ctx, conn->owner, forward_error);
    } else {
        msg = pmsg->peer;
    }
    ASSERT(!msg->request);

    conn->smsg = msg;

    log_debug(LOG_VVERB, "send next rsp %"PRIu64" on c %d", msg->id, conn->sd);

    return msg;
}

void
rsp_send_done(struct context *ctx, struct conn *conn, struct msg *msg)
{
    struct msg *pmsg; /* peer message (request) */

    ASSERT(conn->client && !conn->proxy);
    ASSERT(conn->smsg == NULL);

    log_debug(LOG_VVERB, "send done rsp %"PRIu64" on c %d", msg->id, conn->sd);

    pmsg = msg->peer;

    ASSERT(!msg->request && pmsg->request);
    ASSERT(pmsg->peer == msg);
    ASSERT(pmsg->done && !pmsg->swallow);

    /* dequeue request from client outq */
    conn->dequeue_outq(ctx, conn, pmsg);

    req_put(pmsg);
}
