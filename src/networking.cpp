/*
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "server.h"
#include "atomicvar.h"
#include <sys/uio.h>
#include <math.h>
#include <ctype.h>

static void setProtocolError(const char *errstr, client *c, int pos);

/* Return the size consumed from the allocator, for the specified SDS string,
 * including internal fragmentation. This function is used in order to compute
 * the client output buffer size. */
size_t sdsZmallocSize(sds s) {
    void *sh = sdsAllocPtr(s);
    return zmalloc_size(sh);
}

/* Return the amount of memory used by the sds string at object->ptr
 * for a string object. */
size_t getStringObjectSdsUsedMemory(robj *o) {
    serverAssertWithInfo(NULL,o,o->type == OBJ_STRING);
    switch(o->encoding) {
    case OBJ_ENCODING_RAW: return sdsZmallocSize((sds)o->ptr);
    case OBJ_ENCODING_EMBSTR: return zmalloc_size(o)-sizeof(robj);
    default: return 0; /* Just integer encoding for now. */
    }
}

/* Client.reply list dup and free methods. */
void *dupClientReplyValue(void *o) {
    return sdsdup((sds)o);
}

void freeClientReplyValue(void *o) {
    sdsfree((sds)o);
}

int listMatchObjects(void *a, void *b) {
    return equalStringObjects((robj *)a,(robj *)b);
}

client *createClient(int fd) {
    client *c = (client *)zmalloc(sizeof(client));

    /* passing -1 as fd it is possible to create a non connected client.
     * This is useful since all the commands needs to be executed
     * in the context of a client. When commands are executed in other
     * contexts (for instance a Lua script) we need a non connected client. */
    if (fd != -1) {
        anetNonBlock(NULL,fd);
        anetEnableTcpNoDelay(NULL,fd);
        if (server.tcpkeepalive)
            anetKeepAlive(NULL,fd,server.tcpkeepalive);
        if (server.el->aeCreateFileEvent(fd,AE_READABLE,
            readQueryFromClient, c) == AE_ERR)
        {
            close(fd);
            zfree(c);
            return NULL;
        }
    }

    c->selectDb(0);
    uint64_t client_id;
    atomicGetIncr(server.next_client_id,client_id,1);
    c->m_id = client_id;
    c->m_fd = fd;
    c->m_client_name = NULL;
    c->m_response_buff_pos = 0;
    c->m_query_buf = sdsempty();
    c->m_pending_query_buf = sdsempty();
    c->m_query_buf_peak = 0;
    c->m_req_protocol_type = 0;
    c->m_argc = 0;
    c->m_argv = NULL;
    c->m_cmd = c->m_last_cmd = NULL;
    c->m_multi_bulk_len = 0;
    c->m_bulk_len = -1;
    c->m_already_sent_len = 0;
    c->m_flags = 0;
    c->m_ctime = c->m_last_interaction_time = server.unixtime;
    c->m_authenticated = 0;
    c->m_replication_state = REPL_STATE_NONE;
    c->m_repl_put_online_on_ack = 0;
    c->m_applied_replication_offset = 0;
    c->m_read_replication_offset = 0;
    c->m_replication_ack_off = 0;
    c->m_replication_ack_time = 0;
    c->m_slave_listening_port = 0;
    c->m_slave_ip[0] = '\0';
    c->m_slave_capabilities = SLAVE_CAPA_NONE;
    c->m_reply = listCreate();
    c->m_reply_bytes = 0;
    c->m_obuf_soft_limit_reached_time = 0;
    c->m_reply->listSetFreeMethod(freeClientReplyValue);
    c->m_reply->listSetDupMethod(dupClientReplyValue);
    c->m_blocking_op_type = BLOCKED_NONE;
    c->m_blocking_state.timeout = 0;
    c->m_blocking_state.keys = dictCreate(&objectKeyPointerValueDictType,NULL);
    c->m_blocking_state.target = NULL;
    c->m_blocking_state.numreplicas = 0;
    c->m_blocking_state.reploffset = 0;
    c->m_last_write_global_replication_offset = 0;
    c->m_watched_keys = listCreate();
    c->m_pubsub_channels = dictCreate(&objectKeyPointerValueDictType,NULL);
    c->m_pubsub_patterns = listCreate();
    c->m_cached_peer_id = NULL;
    c->m_pubsub_patterns->listSetFreeMethod(decrRefCountVoid);
    c->m_pubsub_patterns->listSetMatchMethod(listMatchObjects);
    if (fd != -1) server.clients->listAddNodeTail(c);
    initClientMultiState(c);
    return c;
}

/* This function is called every time we are going to transmit new data
 * to the client. The behavior is the following:
 *
 * If the client should receive new data (normal clients will) the function
 * returns C_OK, and make sure to install the write handler in our event
 * loop so that when the socket is writable new data gets written.
 *
 * If the client should not receive new data, because it is a fake client
 * (used to load AOF in memory), a master or because the setup of the write
 * handler failed, the function returns C_ERR.
 *
 * The function may return C_OK without actually installing the write
 * event handler in the following cases:
 *
 * 1) The event handler should already be installed since the output buffer
 *    already contains something.
 * 2) The client is a slave but not yet online, so we want to just accumulate
 *    writes in the buffer but not actually sending them yet.
 *
 * Typically gets called every time a reply is built, before adding more
 * data to the clients output buffers. If the function returns C_ERR no
 * data should be appended to the output buffers. */
int client::prepareClientToWrite() {
    /* If it's the Lua client we always return ok without installing any
     * handler since there is no socket at all. */
    if (m_flags & (CLIENT_LUA|CLIENT_MODULE)) return C_OK;

    /* CLIENT REPLY OFF / SKIP handling: don't send replies. */
    if (m_flags & (CLIENT_REPLY_OFF|CLIENT_REPLY_SKIP)) return C_ERR;

    /* Masters don't receive replies, unless CLIENT_MASTER_FORCE_REPLY flag
     * is set. */
    if ((m_flags & CLIENT_MASTER) &&
        !(m_flags & CLIENT_MASTER_FORCE_REPLY)) return C_ERR;

    if (m_fd <= 0) return C_ERR; /* Fake client for AOF loading. */

    /* Schedule the client to write the output buffers to the socket only
     * if not already done (there were no pending writes already and the client
     * was yet not flagged), and, for slaves, if the slave can actually
     * receive writes at this stage. */
    if (!this->clientHasPendingReplies() &&
        !(m_flags & CLIENT_PENDING_WRITE) &&
        (m_replication_state == REPL_STATE_NONE ||
         (m_replication_state == SLAVE_STATE_ONLINE && !m_repl_put_online_on_ack)))
    {
        /* Here instead of installing the write handler, we just flag the
         * client and put it into a list of clients that have something
         * to write to the socket. This way before re-entering the event
         * loop, we can try to directly write to the client sockets avoiding
         * a system call. We'll only really install the write handler if
         * we'll not be able to write the whole reply at once. */
        m_flags |= CLIENT_PENDING_WRITE;
        server.clients_pending_write->listAddNodeHead(this);
    }

    /* Authorize the caller to queue in the output buffer of this client. */
    return C_OK;
}

/* -----------------------------------------------------------------------------
 * Low level functions to add more data to output buffers.
 * -------------------------------------------------------------------------- */

int client::_addReplyToBuffer(const char *s, size_t len) {
    size_t available = sizeof(m_response_buff) - m_response_buff_pos;

    if (m_flags & CLIENT_CLOSE_AFTER_REPLY)
        return C_OK;

    /* If there already are entries in the reply list, we cannot
     * add anything more to the static buffer. */
    if (m_reply->listLength() > 0)
        return C_ERR;

    /* Check that the buffer has enough space available for this string. */
    if (len > available)
        return C_ERR;

    memcpy(m_response_buff + m_response_buff_pos, s, len);
    m_response_buff_pos += len;
    return C_OK;
}

void client::_addReplyObjectToList(robj *o) {
    if (m_flags & CLIENT_CLOSE_AFTER_REPLY)
        return;

    if (m_reply->listLength() == 0) {
        sds s = sdsdup((sds)o->ptr);
        m_reply->listAddNodeTail(s);
        m_reply_bytes += sdslen(s);
    } else {
        listNode *ln = m_reply->listLast();
        sds tail = (sds)ln->listNodeValue();

        /* Append to this object when possible. If tail == NULL it was
         * set via addDeferredMultiBulkLength(). */
        if (tail && sdslen(tail)+sdslen((sds)o->ptr) <= PROTO_REPLY_CHUNK_BYTES) {
            tail = sdscatsds(tail,(sds)o->ptr);
            ln->SetNodeValue(tail);
            m_reply_bytes += sdslen((sds)o->ptr);
        } else {
            sds s = sdsdup((sds)o->ptr);
            m_reply->listAddNodeTail(s);
            m_reply_bytes += sdslen(s);
        }
    }
    asyncCloseClientOnOutputBufferLimitReached(this);
}

/* This method takes responsibility over the sds. When it is no longer
 * needed it will be free'd, otherwise it ends up in a robj. */
void client::_addReplySdsToList(sds s) {
    if (m_flags & CLIENT_CLOSE_AFTER_REPLY) {
        sdsfree(s);
        return;
    }

    if (m_reply->listLength() == 0) {
        m_reply->listAddNodeTail(s);
        m_reply_bytes += sdslen(s);
    } else {
        listNode *ln = m_reply->listLast();
        sds tail = (sds)ln->listNodeValue();

        /* Append to this object when possible. If tail == NULL it was
         * set via addDeferredMultiBulkLength(). */
        if (tail && sdslen(tail)+sdslen(s) <= PROTO_REPLY_CHUNK_BYTES) {
            tail = sdscatsds(tail,s);
            ln->SetNodeValue(tail);
            m_reply_bytes += sdslen(s);
            sdsfree(s);
        } else {
            m_reply->listAddNodeTail(s);
            m_reply_bytes += sdslen(s);
        }
    }
    asyncCloseClientOnOutputBufferLimitReached(this);
}

void client::_addReplyStringToList(const char *s, size_t len) {
    if (m_flags & CLIENT_CLOSE_AFTER_REPLY) return;

    if (m_reply->listLength() == 0) {
        sds node = sdsnewlen(s,len);
        m_reply->listAddNodeTail(node);
        m_reply_bytes += len;
    } else {
        listNode *ln = m_reply->listLast();
        sds tail = (sds)ln->listNodeValue();

        /* Append to this object when possible. If tail == NULL it was
         * set via addDeferredMultiBulkLength(). */
        if (tail && sdslen(tail)+len <= PROTO_REPLY_CHUNK_BYTES) {
            tail = sdscatlen(tail,s,len);
            ln->SetNodeValue(tail);
            m_reply_bytes += len;
        } else {
            sds node = sdsnewlen(s,len);
            m_reply->listAddNodeTail(node);
            m_reply_bytes += len;
        }
    }
    asyncCloseClientOnOutputBufferLimitReached(this);
}

/* -----------------------------------------------------------------------------
 * Higher level functions to queue data on the client output buffer.
 * The following functions are the ones that commands implementations will call.
 * -------------------------------------------------------------------------- */

void client::addReply(robj *obj) {
    if (prepareClientToWrite() != C_OK)
        return;

    /* This is an important place where we can avoid copy-on-write
     * when there is a saving child running, avoiding touching the
     * refcount field of the object if it's not needed.
     *
     * If the encoding is RAW and there is room in the static buffer
     * we'll be able to send the object to the client without
     * messing with its page. */
    if (sdsEncodedObject(obj)) {
        if (_addReplyToBuffer((const char*)obj->ptr,sdslen((sds)obj->ptr)) != C_OK)
            _addReplyObjectToList(obj);
    } else if (obj->encoding == OBJ_ENCODING_INT) {
        /* Optimization: if there is room in the static buffer for 32 bytes
         * (more than the max chars a 64 bit integer can take as string) we
         * avoid decoding the object and go for the lower level approach. */
        if (m_reply->listLength() == 0 && (sizeof(m_response_buff) - m_response_buff_pos) >= 32) {
            char buf[32];
            int len;

            len = ll2string(buf,sizeof(buf),(long)obj->ptr);
            if (_addReplyToBuffer(buf, len) == C_OK)
                return;
            /* else... continue with the normal code path, but should never
             * happen actually since we verified there is room. */
        }
        obj = getDecodedObject(obj);
        if (_addReplyToBuffer((const char*)obj->ptr,sdslen((sds)obj->ptr)) != C_OK)
            _addReplyObjectToList(obj);
        decrRefCount(obj);
    } else {
        serverPanic("Wrong obj->encoding in addReply()");
    }
}

void client::addReplySds(sds s) {
    if (prepareClientToWrite() != C_OK) {
        /* The caller expects the sds to be free'd. */
        sdsfree(s);
        return;
    }
    if (_addReplyToBuffer((const char*)s,sdslen(s)) == C_OK) {
        sdsfree(s);
    } else {
        /* This method free's the sds when it is no longer needed. */
        _addReplySdsToList(s);
    }
}

/* This low level function just adds whatever protocol you send it to the
 * client buffer, trying the static buffer initially, and using the string
 * of objects if not possible.
 *
 * It is efficient because does not create an SDS object nor an Redis object
 * if not needed. The object will only be created by calling
 * _addReplyStringToList() if we fail to extend the existing tail object
 * in the list of objects. */
void client::addReplyString(const char *s, size_t len) {
    if (prepareClientToWrite() != C_OK)
        return;
    if (_addReplyToBuffer(s,len) != C_OK)
        _addReplyStringToList(s,len);
}

void client::addReplyErrorLength(const char *s, size_t len) {
    addReplyString("-ERR ",5);
    addReplyString(s,len);
    addReplyString("\r\n",2);
}

void client::addReplyError(const char *err) {
    addReplyErrorLength(err, strlen(err));
}

void client::addReplyErrorFormat(const char *fmt, ...) {
    size_t l, j;
    va_list ap;
    va_start(ap,fmt);
    sds s = sdscatvprintf(sdsempty(),fmt,ap);
    va_end(ap);
    /* Make sure there are no newlines in the string, otherwise invalid protocol
     * is emitted. */
    l = sdslen(s);
    for (j = 0; j < l; j++) {
        if (s[j] == '\r' || s[j] == '\n') s[j] = ' ';
    }
    addReplyErrorLength(s,sdslen(s));
    sdsfree(s);
}

void client::addReplyStatusLength(const char *s, size_t len) {
    addReplyString("+",1);
    addReplyString(s,len);
    addReplyString("\r\n",2);
}

void client::addReplyStatus(const char *status) {
    addReplyStatusLength(status,strlen(status));
}

void client::addReplyStatusFormat(const char *fmt, ...) {
    va_list ap;
    va_start(ap,fmt);
    sds s = sdscatvprintf(sdsempty(),fmt,ap);
    va_end(ap);
    addReplyStatusLength(s,sdslen(s));
    sdsfree(s);
}

/* Adds an empty object to the reply list that will contain the multi bulk
 * length, which is not known when this function is called. */
void* client::addDeferredMultiBulkLength() {
    /* Note that we install the write event here even if the object is not
     * ready to be sent, since we are sure that before returning to the
     * event loop setDeferredMultiBulkLength() will be called. */
    if (prepareClientToWrite() != C_OK)
        return NULL;
    m_reply->listAddNodeTail(NULL); /* NULL is our placeholder. */
    return m_reply->listLast();
}

/* Populate the length object and try gluing it to the next chunk. */
void client::setDeferredMultiBulkLength(void *node, long length) {
    listNode *ln = (listNode*)node;
    sds len, next;

    /* Abort when *node is NULL: when the client should not accept writes
     * we return NULL in addDeferredMultiBulkLength() */
    if (node == NULL)
        return;

    len = sdscatprintf(sdsnewlen("*",1),"%ld\r\n",length);
    ln->SetNodeValue(len);
    m_reply_bytes += sdslen(len);
    if (ln->listNextNode() != NULL) {
        next = (sds)ln->listNextNode()->listNodeValue();

        /* Only glue when the next node is non-NULL (an sds in this case) */
        if (next != NULL) {
            len = sdscatsds(len,next);
            m_reply->listDelNode(ln->listNextNode());
            ln->SetNodeValue(len);
            /* No need to update reply_bytes: we are just moving the same
             * amount of bytes from one node to another. */
        }
    }
    asyncCloseClientOnOutputBufferLimitReached(this);
}

/* Add a double as a bulk reply */
void client::addReplyDouble(double d) {
    char dbuf[128], sbuf[128];
    int dlen, slen;
    if (isinf(d)) {
        /* Libc in odd systems (Hi Solaris!) will format infinite in a
         * different way, so better to handle it in an explicit way. */
        addReplyBulkCString(d > 0 ? "inf" : "-inf");
    } else {
        dlen = snprintf(dbuf,sizeof(dbuf),"%.17g",d);
        slen = snprintf(sbuf,sizeof(sbuf),"$%d\r\n%s\r\n",dlen,dbuf);
        addReplyString(sbuf,slen);
    }
}

/* Add a long double as a bulk reply, but uses a human readable formatting
 * of the double instead of exposing the crude behavior of doubles to the
 * dear user. */
void client::addReplyHumanLongDouble(long double d) {
    robj *o = createStringObjectFromLongDouble(d,1);
    addReplyBulk(o);
    decrRefCount(o);
}

/* Add a long long as integer reply or bulk len / multi bulk count.
 * Basically this is used to output <prefix><long long><crlf>. */
void client::addReplyLongLongWithPrefix(long long ll, char prefix) {
    char buf[128];
    int len;

    /* Things like $3\r\n or *2\r\n are emitted very often by the protocol
     * so we have a few shared objects to use if the integer is small
     * like it is most of the times. */
    if (prefix == '*' && ll < OBJ_SHARED_BULKHDR_LEN && ll >= 0) {
        addReply(shared.mbulkhdr[ll]);
        return;
    } else if (prefix == '$' && ll < OBJ_SHARED_BULKHDR_LEN && ll >= 0) {
        addReply(shared.bulkhdr[ll]);
        return;
    }

    buf[0] = prefix;
    len = ll2string(buf+1,sizeof(buf)-1,ll);
    buf[len+1] = '\r';
    buf[len+2] = '\n';
    addReplyString(buf,len+3);
}

void client::addReplyLongLong(long long ll) {
    if (ll == 0)
        addReply(shared.czero);
    else if (ll == 1)
        addReply(shared.cone);
    else
        addReplyLongLongWithPrefix(ll,':');
}

void client::addReplyMultiBulkLen(long length) {
    if (length < OBJ_SHARED_BULKHDR_LEN)
        addReply(shared.mbulkhdr[length]);
    else
        addReplyLongLongWithPrefix(length,'*');
}

/* Create the length prefix of a bulk reply, example: $2234 */
void client::addReplyBulkLen(robj *obj) {
    size_t len;

    if (sdsEncodedObject(obj)) {
        len = sdslen((sds)obj->ptr);
    } else {
        long n = (long)obj->ptr;

        /* Compute how many bytes will take this integer as a radix 10 string */
        len = 1;
        if (n < 0) {
            len++;
            n = -n;
        }
        while((n = n/10) != 0) {
            len++;
        }
    }

    if (len < OBJ_SHARED_BULKHDR_LEN)
        addReply(shared.bulkhdr[len]);
    else
        addReplyLongLongWithPrefix(len,'$');
}

/* Add a Redis Object as a bulk reply */
void client::addReplyBulk(robj *obj) {
    addReplyBulkLen(obj);
    addReply(obj);
    addReply(shared.crlf);
}

/* Add a C buffer as bulk reply */
void client::addReplyBulkCBuffer(const void *p, size_t len) {
    addReplyLongLongWithPrefix(len,'$');
    addReplyString((const char *)p,len);
    addReply(shared.crlf);
}

/* Add sds to reply (takes ownership of sds and frees it) */
void client::addReplyBulkSds(sds s)  {
    addReplyLongLongWithPrefix(sdslen(s),'$');
    addReplySds(s);
    addReply(shared.crlf);
}

/* Add a C nul term string as bulk reply */
void client::addReplyBulkCString(const char *s) {
    if (s == NULL) {
        addReply(shared.nullbulk);
    } else {
        addReplyBulkCBuffer(s,strlen(s));
    }
}

/* Add a long long as a bulk reply */
void client::addReplyBulkLongLong(long long ll) {
    char buf[64];
    int len;

    len = ll2string(buf,64,ll);
    addReplyBulkCBuffer(buf,len);
}

/* Copy 'src' client output buffers into 'dst' client output buffers.
 * The function takes care of freeing the old output buffers of the
 * destination client. */
void copyClientOutputBuffer(client *dst, client *src) {
    listRelease(dst->m_reply);
    dst->m_reply = src->m_reply->listDup();
    memcpy(dst->m_response_buff,src->m_response_buff,src->m_response_buff_pos);
    dst->m_response_buff_pos = src->m_response_buff_pos;
    dst->m_reply_bytes = src->m_reply_bytes;
}

/* Return true if the specified client has pending reply buffers to write to
 * the socket. */
int client::clientHasPendingReplies() {
    return m_response_buff_pos || m_reply->listLength();
}

#define MAX_ACCEPTS_PER_CALL 1000
static void acceptCommonHandler(int fd, int flags, char *ip) {
    client *c;
    if ((c = createClient(fd)) == NULL) {
        serverLog(LL_WARNING,
            "Error registering fd event for the new client: %s (fd=%d)",
            strerror(errno),fd);
        close(fd); /* May be already closed, just ignore errors */
        return;
    }
    /* If maxclient directive is set and this is one client more... close the
     * connection. Note that we create the client instead to check before
     * for this condition, since now the socket is already set in non-blocking
     * mode and we can send an error for free using the Kernel I/O */
    if (server.clients->listLength() > server.maxclients) {
        char *err = "-ERR max number of clients reached\r\n";

        /* That's a best effort error message, don't check write errors */
        if (write(c->m_fd,err,strlen(err)) == -1) {
            /* Nothing to do, Just to avoid the warning... */
        }
        server.stat_rejected_conn++;
        freeClient(c);
        return;
    }

    /* If the server is running in protected mode (the default) and there
     * is no password set, nor a specific interface is bound, we don't accept
     * requests from non loopback interfaces. Instead we try to explain the
     * user what to do to fix it if needed. */
    if (server.protected_mode &&
        server.bindaddr_count == 0 &&
        server.requirepass == NULL &&
        !(flags & CLIENT_UNIX_SOCKET) &&
        ip != NULL)
    {
        if (strcmp(ip,"127.0.0.1") && strcmp(ip,"::1")) {
            char *err =
                "-DENIED Redis is running in protected mode because protected "
                "mode is enabled, no bind address was specified, no "
                "authentication password is requested to clients. In this mode "
                "connections are only accepted from the loopback interface. "
                "If you want to connect from external computers to Redis you "
                "may adopt one of the following solutions: "
                "1) Just disable protected mode sending the command "
                "'CONFIG SET protected-mode no' from the loopback interface "
                "by connecting to Redis from the same host the server is "
                "running, however MAKE SURE Redis is not publicly accessible "
                "from internet if you do so. Use CONFIG REWRITE to make this "
                "change permanent. "
                "2) Alternatively you can just disable the protected mode by "
                "editing the Redis configuration file, and setting the protected "
                "mode option to 'no', and then restarting the server. "
                "3) If you started the server manually just for testing, restart "
                "it with the '--protected-mode no' option. "
                "4) Setup a bind address or an authentication password. "
                "NOTE: You only need to do one of the above things in order for "
                "the server to start accepting connections from the outside.\r\n";
            if (write(c->m_fd,err,strlen(err)) == -1) {
                /* Nothing to do, Just to avoid the warning... */
            }
            server.stat_rejected_conn++;
            freeClient(c);
            return;
        }
    }

    server.stat_numconnections++;
    c->m_flags |= flags;
}

void acceptTcpHandler(aeEventLoop *el, int fd, void *privdata, int mask) {
    int cport, cfd, max = MAX_ACCEPTS_PER_CALL;
    char cip[NET_IP_STR_LEN];
    UNUSED(el);
    UNUSED(mask);
    UNUSED(privdata);

    while(max--) {
        cfd = anetTcpAccept(server.neterr, fd, cip, sizeof(cip), &cport);
        if (cfd == ANET_ERR) {
            if (errno != EWOULDBLOCK)
                serverLog(LL_WARNING,
                    "Accepting client connection: %s", server.neterr);
            return;
        }
        serverLog(LL_VERBOSE,"Accepted %s:%d", cip, cport);
        acceptCommonHandler(cfd,0,cip);
    }
}

void acceptUnixHandler(aeEventLoop *el, int fd, void *privdata, int mask) {
    int cfd, max = MAX_ACCEPTS_PER_CALL;
    UNUSED(el);
    UNUSED(mask);
    UNUSED(privdata);

    while(max--) {
        cfd = anetUnixAccept(server.neterr, fd);
        if (cfd == ANET_ERR) {
            if (errno != EWOULDBLOCK)
                serverLog(LL_WARNING,
                    "Accepting client connection: %s", server.neterr);
            return;
        }
        serverLog(LL_VERBOSE,"Accepted connection to %s", server.unixsocket);
        acceptCommonHandler(cfd,CLIENT_UNIX_SOCKET,NULL);
    }
}

static void freeClientArgv(client *c) {
    int j;
    for (j = 0; j < c->m_argc; j++)
        decrRefCount(c->m_argv[j]);
    c->m_argc = 0;
    c->m_cmd = NULL;
}

/* Close all the slaves connections. This is useful in chained replication
 * when we resync with our own master and want to force all our slaves to
 * resync with us as well. */
void disconnectSlaves() {
    while (server.slaves->listLength()) {
        listNode *ln = server.slaves->listFirst();
        freeClient((client*)ln->listNodeValue());
    }
}

/* Remove the specified client from global lists where the client could
 * be referenced, not including the Pub/Sub channels.
 * This is used by freeClient() and replicationCacheMaster(). */
void unlinkClient(client *c) {
    listNode *ln;

    /* If this is marked as current client unset it. */
    if (server.current_client == c) server.current_client = NULL;

    /* Certain operations must be done only if the client has an active socket.
     * If the client was already unlinked or if it's a "fake client" the
     * fd is already set to -1. */
    if (c->m_fd != -1) {
        /* Remove from the list of active clients. */
        ln = server.clients->listSearchKey(c);
        serverAssert(ln != NULL);
        server.clients->listDelNode(ln);

        /* Unregister async I/O handlers and close the socket. */
        server.el->aeDeleteFileEvent(c->m_fd,AE_READABLE);
        server.el->aeDeleteFileEvent(c->m_fd,AE_WRITABLE);
        close(c->m_fd);
        c->m_fd = -1;
    }

    /* Remove from the list of pending writes if needed. */
    if (c->m_flags & CLIENT_PENDING_WRITE) {
        ln = server.clients_pending_write->listSearchKey(c);
        serverAssert(ln != NULL);
        server.clients_pending_write->listDelNode(ln);
        c->m_flags &= ~CLIENT_PENDING_WRITE;
    }

    /* When client was just unblocked because of a blocking operation,
     * remove it from the list of unblocked clients. */
    if (c->m_flags & CLIENT_UNBLOCKED) {
        ln = server.unblocked_clients->listSearchKey(c);
        serverAssert(ln != NULL);
        server.unblocked_clients->listDelNode(ln);
        c->m_flags &= ~CLIENT_UNBLOCKED;
    }
}

void freeClient(client *c) {
    listNode *ln;

    /* If it is our master that's beging disconnected we should make sure
     * to cache the state to try a partial resynchronization later.
     *
     * Note that before doing this we make sure that the client is not in
     * some unexpected state, by checking its flags. */
    if (server.master && c->m_flags & CLIENT_MASTER) {
        serverLog(LL_WARNING,"Connection with master lost.");
        if (!(c->m_flags & (CLIENT_CLOSE_AFTER_REPLY|
                          CLIENT_CLOSE_ASAP|
                          CLIENT_BLOCKED|
                          CLIENT_UNBLOCKED)))
        {
            replicationCacheMaster(c);
            return;
        }
    }

    /* Log link disconnection with slave */
    if ((c->m_flags & CLIENT_SLAVE) && !(c->m_flags & CLIENT_MONITOR)) {
        serverLog(LL_WARNING,"Connection with slave %s lost.",
            replicationGetSlaveName(c));
    }

    /* Free the query buffer */
    sdsfree(c->m_query_buf);
    sdsfree(c->m_pending_query_buf);
    c->m_query_buf = NULL;

    /* Deallocate structures used to block on blocking ops. */
    if (c->m_flags & CLIENT_BLOCKED) unblockClient(c);
    dictRelease(c->m_blocking_state.keys);

    /* UNWATCH all the keys */
    unwatchAllKeys(c);
    listRelease(c->m_watched_keys);

    /* Unsubscribe from all the pubsub channels */
    pubsubUnsubscribeAllChannels(c,0);
    pubsubUnsubscribeAllPatterns(c,0);
    dictRelease(c->m_pubsub_channels);
    listRelease(c->m_pubsub_patterns);

    /* Free data structures. */
    listRelease(c->m_reply);
    freeClientArgv(c);

    /* Unlink the client: this will close the socket, remove the I/O
     * handlers, and remove references of the client from different
     * places where active clients may be referenced. */
    unlinkClient(c);

    /* Master/slave cleanup Case 1:
     * we lost the connection with a slave. */
    if (c->m_flags & CLIENT_SLAVE) {
        if (c->m_replication_state == SLAVE_STATE_SEND_BULK) {
            if (c->m_replication_db_fd != -1) close(c->m_replication_db_fd);
            if (c->m_replication_db_preamble) sdsfree(c->m_replication_db_preamble);
        }
        list *l = (c->m_flags & CLIENT_MONITOR) ? server.monitors : server.slaves;
        ln = l->listSearchKey(c);
        serverAssert(ln != NULL);
        l->listDelNode(ln);
        /* We need to remember the time when we started to have zero
         * attached slaves, as after some time we'll free the replication
         * backlog. */
        if (c->m_flags & CLIENT_SLAVE && server.slaves->listLength() == 0)
            server.repl_no_slaves_since = server.unixtime;
        refreshGoodSlavesCount();
    }

    /* Master/slave cleanup Case 2:
     * we lost the connection with the master. */
    if (c->m_flags & CLIENT_MASTER) replicationHandleMasterDisconnection();

    /* If this client was scheduled for async freeing we need to remove it
     * from the queue. */
    if (c->m_flags & CLIENT_CLOSE_ASAP) {
        ln = server.clients_to_close->listSearchKey(c);
        serverAssert(ln != NULL);
        server.clients_to_close->listDelNode(ln);
    }

    /* Release other dynamically allocated client structure fields,
     * and finally release the client structure itself. */
    if (c->m_client_name) decrRefCount(c->m_client_name);
    zfree(c->m_argv);
    freeClientMultiState(c);
    sdsfree(c->m_cached_peer_id);
    zfree(c);
}

/* Schedule a client to free it at a safe time in the serverCron() function.
 * This function is useful when we need to terminate a client but we are in
 * a context where calling freeClient() is not possible, because the client
 * should be valid for the continuation of the flow of the program. */
void freeClientAsync(client *c) {
    if (c->m_flags & CLIENT_CLOSE_ASAP || c->m_flags & CLIENT_LUA) return;
    c->m_flags |= CLIENT_CLOSE_ASAP;
    server.clients_to_close->listAddNodeTail(c);
}

void freeClientsInAsyncFreeQueue() {
    while (server.clients_to_close->listLength()) {
        listNode *ln = server.clients_to_close->listFirst();
        client *c = (client *)ln->listNodeValue();

        c->m_flags &= ~CLIENT_CLOSE_ASAP;
        freeClient(c);
        server.clients_to_close->listDelNode(ln);
    }
}

/* Write data in output buffers to client. Return C_OK if the client
 * is still valid after the call, C_ERR if it was freed. */
int writeToClient(int fd, client *c, int handler_installed) {
    ssize_t nwritten = 0, totwritten = 0;
    size_t objlen;
    sds o;

    while(c->clientHasPendingReplies()) {
        if (c->m_response_buff_pos > 0) {
            nwritten = write(fd,c->m_response_buff+c->m_already_sent_len,c->m_response_buff_pos-c->m_already_sent_len);
            if (nwritten <= 0) break;
            c->m_already_sent_len += nwritten;
            totwritten += nwritten;

            /* If the buffer was sent, set bufpos to zero to continue with
             * the remainder of the reply. */
            if ((int)c->m_already_sent_len == c->m_response_buff_pos) {
                c->m_response_buff_pos = 0;
                c->m_already_sent_len = 0;
            }
        } else {
            o = (sds)c->m_reply->listFirst()->listNodeValue();
            objlen = sdslen(o);

            if (objlen == 0) {
                c->m_reply->listDelNode(c->m_reply->listFirst());
                continue;
            }

            nwritten = write(fd, o + c->m_already_sent_len, objlen - c->m_already_sent_len);
            if (nwritten <= 0) break;
            c->m_already_sent_len += nwritten;
            totwritten += nwritten;

            /* If we fully sent the object on head go to the next one */
            if (c->m_already_sent_len == objlen) {
                c->m_reply->listDelNode(c->m_reply->listFirst());
                c->m_already_sent_len = 0;
                c->m_reply_bytes -= objlen;
                /* If there are no longer objects in the list, we expect
                 * the count of reply bytes to be exactly zero. */
                if (c->m_reply->listLength() == 0)
                    serverAssert(c->m_reply_bytes == 0);
            }
        }
        /* Note that we avoid to send more than NET_MAX_WRITES_PER_EVENT
         * bytes, in a single threaded server it's a good idea to serve
         * other clients as well, even if a very large request comes from
         * super fast link that is always able to accept data (in real world
         * scenario think about 'KEYS *' against the loopback interface).
         *
         * However if we are over the maxmemory limit we ignore that and
         * just deliver as much data as it is possible to deliver. */
        if (totwritten > NET_MAX_WRITES_PER_EVENT &&
            (server.maxmemory == 0 ||
             zmalloc_used_memory() < server.maxmemory)) break;
    }
    server.stat_net_output_bytes += totwritten;
    if (nwritten == -1) {
        if (errno == EAGAIN) {
            nwritten = 0;
        } else {
            serverLog(LL_VERBOSE,
                "Error writing to client: %s", strerror(errno));
            freeClient(c);
            return C_ERR;
        }
    }
    if (totwritten > 0) {
        /* For clients representing masters we don't count sending data
         * as an interaction, since we always send REPLCONF ACK commands
         * that take some time to just fill the socket output buffer.
         * We just rely on data / pings received for timeout detection. */
        if (!(c->m_flags & CLIENT_MASTER)) c->m_last_interaction_time = server.unixtime;
    }
    if (!c->clientHasPendingReplies()) {
        c->m_already_sent_len = 0;
        if (handler_installed) server.el->aeDeleteFileEvent(c->m_fd,AE_WRITABLE);

        /* Close connection after entire reply has been sent. */
        if (c->m_flags & CLIENT_CLOSE_AFTER_REPLY) {
            freeClient(c);
            return C_ERR;
        }
    }
    return C_OK;
}

/* Write event handler. Just send data to the client. */
void sendReplyToClient(aeEventLoop *el, int fd, void *privdata, int mask) {
    UNUSED(el);
    UNUSED(mask);
    writeToClient(fd,(client*)privdata,1);
}

/* This function is called just before entering the event loop, in the hope
 * we can just write the replies to the client output buffer without any
 * need to use a syscall in order to install the writable event handler,
 * get it called, and so forth. */
int handleClientsWithPendingWrites() {
    listNode *ln;
    int processed = server.clients_pending_write->listLength();

    listIter li(server.clients_pending_write);
    while((ln = li.listNext())) {
        client *c = (client *)ln->listNodeValue();
        c->m_flags &= ~CLIENT_PENDING_WRITE;
        server.clients_pending_write->listDelNode(ln);

        /* Try to write buffers to the client socket. */
        if (writeToClient(c->m_fd,c,0) == C_ERR) continue;

        /* If there is nothing left, do nothing. Otherwise install
         * the write handler. */
        if (c->clientHasPendingReplies() &&
            server.el->aeCreateFileEvent(c->m_fd, AE_WRITABLE,
                sendReplyToClient, c) == AE_ERR)
        {
            freeClientAsync(c);
        }
    }
    return processed;
}

/* resetClient prepare the client to process the next command */
void resetClient(client *c) {
    redisCommandProc *prevcmd = c->m_cmd ? c->m_cmd->proc : NULL;

    freeClientArgv(c);
    c->m_req_protocol_type = 0;
    c->m_multi_bulk_len = 0;
    c->m_bulk_len = -1;

    /* We clear the ASKING flag as well if we are not inside a MULTI, and
     * if what we just executed is not the ASKING command itself. */
    if (!(c->m_flags & CLIENT_MULTI) && prevcmd != askingCommand)
        c->m_flags &= ~CLIENT_ASKING;

    /* Remove the CLIENT_REPLY_SKIP flag if any so that the reply
     * to the next command will be sent, but set the flag if the command
     * we just processed was "CLIENT REPLY SKIP". */
    c->m_flags &= ~CLIENT_REPLY_SKIP;
    if (c->m_flags & CLIENT_REPLY_SKIP_NEXT) {
        c->m_flags |= CLIENT_REPLY_SKIP;
        c->m_flags &= ~CLIENT_REPLY_SKIP_NEXT;
    }
}

/* Like processMultibulkBuffer(), but for the inline protocol instead of RESP,
 * this function consumes the client query buffer and creates a command ready
 * to be executed inside the client structure. Returns C_OK if the command
 * is ready to be executed, or C_ERR if there is still protocol to read to
 * have a well formed command. The function also returns C_ERR when there is
 * a protocol error: in such a case the client structure is setup to reply
 * with the error and close the connection. */
int client::processInlineBuffer() {
    char *newline;
    int argc, j;
    sds *argv, aux;
    size_t querylen;

    /* Search for end of line */
    newline = strchr(m_query_buf,'\n');

    /* Nothing to do without a \r\n */
    if (newline == NULL) {
        if (sdslen((sds)m_query_buf) > PROTO_INLINE_MAX_SIZE) {
            addReplyError("Protocol error: too big inline request");
            setProtocolError("too big inline request",this,0);
        }
        return C_ERR;
    }

    /* Handle the \r\n case. */
    if (newline && newline != m_query_buf && *(newline-1) == '\r')
        newline--;

    /* Split the input buffer up to the \r\n */
    querylen = newline-(m_query_buf);
    aux = sdsnewlen(m_query_buf,querylen);
    argv = sdssplitargs(aux,&argc);
    sdsfree(aux);
    if (argv == NULL) {
        addReplyError("Protocol error: unbalanced quotes in request");
        setProtocolError("unbalanced quotes in inline request",this,0);
        return C_ERR;
    }

    /* Newline from slaves can be used to refresh the last ACK time.
     * This is useful for a slave to ping back while loading a big
     * RDB file. */
    if (querylen == 0 && m_flags & CLIENT_SLAVE)
        m_replication_ack_time = server.unixtime;

    /* Leave data after the first line of the query in the buffer */
    sdsrange(m_query_buf,querylen+2,-1);

    /* Setup argv array on client structure */
    if (argc) {
        if (m_argv) zfree(m_argv);
        m_argv = (robj **)zmalloc(sizeof(robj*)*argc);
    }

    /* Create redis objects for all arguments. */
    for (m_argc = 0, j = 0; j < argc; j++) {
        if (sdslen((sds)argv[j])) {
            m_argv[m_argc] = createObject(OBJ_STRING,argv[j]);
            m_argc++;
        } else {
            sdsfree(argv[j]);
        }
    }
    zfree(argv);
    return C_OK;
}

/* Helper function. Trims query buffer to make the function that processes
 * multi bulk requests idempotent. */
#define PROTO_DUMP_LEN 128
static void setProtocolError(const char *errstr, client *c, int pos) {
    if (server.verbosity <= LL_VERBOSE) {
        sds client = c->catClientInfoString(sdsempty());

        /* Sample some protocol to given an idea about what was inside. */
        char buf[256];
        if (sdslen((sds)c->m_query_buf) < PROTO_DUMP_LEN) {
            snprintf(buf,sizeof(buf),"Query buffer during protocol error: '%s'", c->m_query_buf);
        } else {
            snprintf(buf,sizeof(buf),"Query buffer during protocol error: '%.*s' (... more %zu bytes ...) '%.*s'", PROTO_DUMP_LEN/2, c->m_query_buf, sdslen((sds)c->m_query_buf)-PROTO_DUMP_LEN, PROTO_DUMP_LEN/2, c->m_query_buf+sdslen((sds)c->m_query_buf)-PROTO_DUMP_LEN/2);
        }

        /* Remove non printable chars. */
        char *p = buf;
        while (*p != '\0') {
            if (!isprint(*p)) *p = '.';
            p++;
        }

        /* Log all the client and protocol info. */
        serverLog(LL_VERBOSE,
            "Protocol error (%s) from client: %s. %s", errstr, client, buf);
        sdsfree(client);
    }
    c->m_flags |= CLIENT_CLOSE_AFTER_REPLY;
    sdsrange(c->m_query_buf,pos,-1);
}

/* Process the query buffer for client 'c', setting up the client argument
 * vector for command execution. Returns C_OK if after running the function
 * the client has a well-formed ready to be processed command, otherwise
 * C_ERR if there is still to read more buffer to get the full command.
 * The function also returns C_ERR when there is a protocol error: in such a
 * case the client structure is setup to reply with the error and close
 * the connection.
 *
 * This function is called if processInputBuffer() detects that the next
 * command is in RESP format, so the first byte in the command is found
 * to be '*'. Otherwise for inline commands processInlineBuffer() is called. */
int client::processMultibulkBuffer() {
    char *newline = NULL;
    int pos = 0, ok;
    long long ll;

    if (m_multi_bulk_len == 0) {
        /* The client should have been reset */
        serverAssertWithInfo(this,NULL,m_argc == 0);

        /* Multi bulk length cannot be read without a \r\n */
        newline = strchr(m_query_buf,'\r');
        if (newline == NULL) {
            if (sdslen((sds)m_query_buf) > PROTO_INLINE_MAX_SIZE) {
                addReplyError("Protocol error: too big mbulk count string");
                setProtocolError("too big mbulk count string",this,0);
            }
            return C_ERR;
        }

        /* Buffer should also contain \n */
        if (newline-(m_query_buf) > ((signed)sdslen((sds)m_query_buf)-2))
            return C_ERR;

        /* We know for sure there is a whole line since newline != NULL,
         * so go ahead and find out the multi bulk length. */
        serverAssertWithInfo(this,NULL,m_query_buf[0] == '*');
        ok = string2ll(m_query_buf+1,newline-(m_query_buf+1),&ll);
        if (!ok || ll > 1024*1024) {
            addReplyError("Protocol error: invalid multibulk length");
            setProtocolError("invalid mbulk count",this,pos);
            return C_ERR;
        }

        pos = (newline-m_query_buf)+2;
        if (ll <= 0) {
            sdsrange(m_query_buf,pos,-1);
            return C_OK;
        }

        m_multi_bulk_len = ll;

        /* Setup argv array on client structure */
        if (m_argv) zfree(m_argv);
        m_argv = (robj **)zmalloc(sizeof(robj*)*m_multi_bulk_len);
    }

    serverAssertWithInfo(this,NULL,m_multi_bulk_len > 0);
    while(m_multi_bulk_len) {
        /* Read bulk length if unknown */
        if (m_bulk_len == -1) {
            newline = strchr(m_query_buf+pos,'\r');
            if (newline == NULL) {
                if (sdslen((sds)m_query_buf) > PROTO_INLINE_MAX_SIZE) {
                    addReplyError(
                        "Protocol error: too big bulk count string");
                    setProtocolError("too big bulk count string",this,0);
                    return C_ERR;
                }
                break;
            }

            /* Buffer should also contain \n */
            if (newline-(m_query_buf) > ((signed)sdslen((sds)m_query_buf)-2))
                break;

            if (m_query_buf[pos] != '$') {
                addReplyErrorFormat(
                    "Protocol error: expected '$', got '%c'",
                    m_query_buf[pos]);
                setProtocolError("expected $ but got something else",this,pos);
                return C_ERR;
            }

            ok = string2ll(m_query_buf+pos+1,newline-(m_query_buf+pos+1),&ll);
            if (!ok || ll < 0 || ll > 512*1024*1024) {
                addReplyError("Protocol error: invalid bulk length");
                setProtocolError("invalid bulk length",this,pos);
                return C_ERR;
            }

            pos += newline-(m_query_buf+pos)+2;
            if (ll >= PROTO_MBULK_BIG_ARG) {
                size_t qblen;

                /* If we are going to read a large object from network
                 * try to make it likely that it will start at querybuf
                 * boundary so that we can optimize object creation
                 * avoiding a large copy of data. */
                sdsrange(m_query_buf,pos,-1);
                pos = 0;
                qblen = sdslen((sds)m_query_buf);
                /* Hint the sds library about the amount of bytes this string is
                 * going to contain. */
                if (qblen < (size_t)ll+2)
                    m_query_buf = sdsMakeRoomFor(m_query_buf,ll+2-qblen);
            }
            m_bulk_len = ll;
        }

        /* Read bulk argument */
        if (sdslen((sds)m_query_buf)-pos < (unsigned)(m_bulk_len+2)) {
            /* Not enough data (+2 == trailing \r\n) */
            break;
        } else {
            /* Optimization: if the buffer contains JUST our bulk element
             * instead of creating a new object by *copying* the sds we
             * just use the current sds string. */
            if (pos == 0 &&
                m_bulk_len >= PROTO_MBULK_BIG_ARG &&
                (signed) sdslen((sds)m_query_buf) == m_bulk_len+2)
            {
                m_argv[m_argc++] = createObject(OBJ_STRING,m_query_buf);
                sdsIncrLen(m_query_buf,-2); /* remove CRLF */
                /* Assume that if we saw a fat argument we'll see another one
                 * likely... */
                m_query_buf = sdsnewlen(NULL,m_bulk_len+2);
                sdsclear(m_query_buf);
                pos = 0;
            } else {
                m_argv[m_argc++] =
                    createStringObject(m_query_buf+pos,m_bulk_len);
                pos += m_bulk_len+2;
            }
            m_bulk_len = -1;
            m_multi_bulk_len--;
        }
    }

    /* Trim to pos */
    if (pos)
        sdsrange(m_query_buf,pos,-1);

    /* We're done when multibulk == 0 */
    if (m_multi_bulk_len == 0)
        return C_OK;

    /* Still not ready to process the command */
    return C_ERR;
}

/* This function is called every time, in the client structure 'c', there is
 * more query buffer to process, because we read more data from the socket
 * or because a client was blocked and later reactivated, so there could be
 * pending query buffer, already representing a full command, to process. */
void client::processInputBuffer() {
    server.current_client = this;
    /* Keep processing while there is something in the input buffer */
    while(sdslen((sds)m_query_buf)) {
        /* Return if clients are paused. */
        if (!(m_flags & CLIENT_SLAVE) && clientsArePaused()) break;

        /* Immediately abort if the client is in the middle of something. */
        if (m_flags & CLIENT_BLOCKED) break;

        /* CLIENT_CLOSE_AFTER_REPLY closes the connection once the reply is
         * written to the client. Make sure to not let the reply grow after
         * this flag has been set (i.e. don't process more commands).
         *
         * The same applies for clients we want to terminate ASAP. */
        if (m_flags & (CLIENT_CLOSE_AFTER_REPLY|CLIENT_CLOSE_ASAP)) break;

        /* Determine request type when unknown. */
        if (!m_req_protocol_type) {
            if (m_query_buf[0] == '*') {
                m_req_protocol_type = PROTO_REQ_MULTIBULK;
            } else {
                m_req_protocol_type = PROTO_REQ_INLINE;
            }
        }

        if (m_req_protocol_type == PROTO_REQ_INLINE) {
            if (processInlineBuffer() != C_OK) break;
        } else if (m_req_protocol_type == PROTO_REQ_MULTIBULK) {
            if (processMultibulkBuffer() != C_OK) break;
        } else {
            serverPanic("Unknown request type");
        }

        /* Multibulk processing could see a <= 0 length. */
        if (m_argc == 0) {
            resetClient(this);
        } else {
            /* Only reset the client when the command was executed. */
            if (processCommand(this) == C_OK) {
                if (m_flags & CLIENT_MASTER && !(m_flags & CLIENT_MULTI)) {
                    /* Update the applied replication offset of our master. */
                    m_applied_replication_offset = m_read_replication_offset - sdslen((sds)m_query_buf);
                }

                /* Don't reset the client structure for clients blocked in a
                 * module blocking command, so that the reply callback will
                 * still be able to access the client argv and argc field.
                 * The client will be reset in unblockClientFromModule(). */
                if (!(m_flags & CLIENT_BLOCKED) || m_blocking_op_type != BLOCKED_MODULE)
                    resetClient(this);
            }
            /* freeMemoryIfNeeded may flush slave output buffers. This may
             * result into a slave, that may be the active client, to be
             * freed. */
            if (server.current_client == NULL)
                break;
        }
    }
    server.current_client = NULL;
}

void readQueryFromClient(aeEventLoop *el, int fd, void *privdata, int mask) {
    client *c = (client*) privdata;
    int nread, readlen;
    size_t qblen;
    UNUSED(el);
    UNUSED(mask);

    readlen = PROTO_IOBUF_LEN;
    /* If this is a multi bulk request, and we are processing a bulk reply
     * that is large enough, try to maximize the probability that the query
     * buffer contains exactly the SDS string representing the object, even
     * at the risk of requiring more read(2) calls. This way the function
     * processMultiBulkBuffer() can avoid copying buffers to create the
     * Redis Object representing the argument. */
    if (c->m_req_protocol_type == PROTO_REQ_MULTIBULK && c->m_multi_bulk_len && c->m_bulk_len != -1
        && c->m_bulk_len >= PROTO_MBULK_BIG_ARG)
    {
        int remaining = (unsigned)(c->m_bulk_len+2)-sdslen(c->m_query_buf);

        if (remaining < readlen) readlen = remaining;
    }

    qblen = sdslen(c->m_query_buf);
    if (c->m_query_buf_peak < qblen) c->m_query_buf_peak = qblen;
    c->m_query_buf = sdsMakeRoomFor(c->m_query_buf, readlen);
    nread = read(fd, c->m_query_buf+qblen, readlen);
    if (nread == -1) {
        if (errno == EAGAIN) {
            return;
        } else {
            serverLog(LL_VERBOSE, "Reading from client: %s",strerror(errno));
            freeClient(c);
            return;
        }
    } else if (nread == 0) {
        serverLog(LL_VERBOSE, "Client closed connection");
        freeClient(c);
        return;
    } else if (c->m_flags & CLIENT_MASTER) {
        /* Append the query buffer to the pending (not applied) buffer
         * of the master. We'll use this buffer later in order to have a
         * copy of the string applied by the last command executed. */
        c->m_pending_query_buf = sdscatlen(c->m_pending_query_buf,
                                        c->m_query_buf+qblen,nread);
    }

    sdsIncrLen(c->m_query_buf,nread);
    c->m_last_interaction_time = server.unixtime;
    if (c->m_flags & CLIENT_MASTER) c->m_read_replication_offset += nread;
    server.stat_net_input_bytes += nread;
    if (sdslen(c->m_query_buf) > server.client_max_querybuf_len) {
        sds ci = c->catClientInfoString(sdsempty()), bytes = sdsempty();

        bytes = sdscatrepr(bytes,c->m_query_buf,64);
        serverLog(LL_WARNING,"Closing client that reached max query buffer length: %s (qbuf initial bytes: %s)", ci, bytes);
        sdsfree(ci);
        sdsfree(bytes);
        freeClient(c);
        return;
    }

    /* Time to process the buffer. If the client is a master we need to
     * compute the difference between the applied offset before and after
     * processing the buffer, to understand how much of the replication stream
     * was actually applied to the master state: this quantity, and its
     * corresponding part of the replication stream, will be propagated to
     * the sub-slaves and to the replication backlog. */
    if (!(c->m_flags & CLIENT_MASTER)) {
        c->processInputBuffer();
    } else {
        size_t prev_offset = c->m_applied_replication_offset;
        c->processInputBuffer();
        size_t applied = c->m_applied_replication_offset - prev_offset;
        if (applied) {
            replicationFeedSlavesFromMasterStream(server.slaves,
                    c->m_pending_query_buf, applied);
            sdsrange(c->m_pending_query_buf,applied,-1);
        }
    }
}

void getClientsMaxBuffers(unsigned long *longest_output_list,
                          unsigned long *biggest_input_buffer) {
    client *c;
    listNode *ln;
    unsigned long lol = 0, bib = 0;

    listIter li(server.clients);
    while ((ln = li.listNext()) != NULL) {
        c = (client *)ln->listNodeValue();

        if (c->m_reply->listLength() > lol)
            lol = c->m_reply->listLength();
        if (sdslen(c->m_query_buf) > bib)
            bib = sdslen(c->m_query_buf);
    }
    *longest_output_list = lol;
    *biggest_input_buffer = bib;
}

/* A Redis "Peer ID" is a colon separated ip:port pair.
 * For IPv4 it's in the form x.y.z.k:port, example: "127.0.0.1:1234".
 * For IPv6 addresses we use [] around the IP part, like in "[::1]:1234".
 * For Unix sockets we use path:0, like in "/tmp/redis:0".
 *
 * A Peer ID always fits inside a buffer of NET_PEER_ID_LEN bytes, including
 * the null term.
 *
 * On failure the function still populates 'peerid' with the "?:0" string
 * in case you want to relax error checking or need to display something
 * anyway (see anetPeerToString implementation for more info). */
void client::genClientPeerId(char *peerid, size_t peerid_len) {
    if (m_flags & CLIENT_UNIX_SOCKET) {
        /* Unix socket client. */
        snprintf(peerid,peerid_len,"%s:0",server.unixsocket);
    } else {
        /* TCP client. */
        anetFormatPeer(m_fd, peerid, peerid_len);
    }
}

/* This function returns the client peer id, by creating and caching it
 * if client->peerid is NULL, otherwise returning the cached value.
 * The Peer ID never changes during the life of the client, however it
 * is expensive to compute. */
char* client::getClientPeerId() {
    char peerid[NET_PEER_ID_LEN];

    if (m_cached_peer_id == NULL) {
        genClientPeerId(peerid,sizeof(peerid));
        m_cached_peer_id = sdsnew(peerid);
    }
    return m_cached_peer_id;
}

/* Concatenate a string representing the state of a client in an human
 * readable format, into the sds string 's'. */
sds client::catClientInfoString(sds s) {
    char flags[16], events[3], *p;
    int emask;

    p = flags;
    if (m_flags & CLIENT_SLAVE) {
        if (m_flags & CLIENT_MONITOR)
            *p++ = 'O';
        else
            *p++ = 'S';
    }
    if (m_flags & CLIENT_MASTER) *p++ = 'M';
    if (m_flags & CLIENT_MULTI) *p++ = 'x';
    if (m_flags & CLIENT_BLOCKED) *p++ = 'b';
    if (m_flags & CLIENT_DIRTY_CAS) *p++ = 'd';
    if (m_flags & CLIENT_CLOSE_AFTER_REPLY) *p++ = 'c';
    if (m_flags & CLIENT_UNBLOCKED) *p++ = 'u';
    if (m_flags & CLIENT_CLOSE_ASAP) *p++ = 'A';
    if (m_flags & CLIENT_UNIX_SOCKET) *p++ = 'U';
    if (m_flags & CLIENT_READONLY) *p++ = 'r';
    if (p == flags) *p++ = 'N';
    *p++ = '\0';

    emask = m_fd == -1 ? 0 : server.el->aeGetFileEvents(m_fd);
    p = events;
    if (emask & AE_READABLE) *p++ = 'r';
    if (emask & AE_WRITABLE) *p++ = 'w';
    *p = '\0';
    return sdscatfmt(s,
        "id=%U addr=%s fd=%i name=%s age=%I idle=%I flags=%s db=%i sub=%i psub=%i multi=%i qbuf=%U qbuf-free=%U obl=%U oll=%U omem=%U events=%s cmd=%s",
        (unsigned long long) m_id,
        getClientPeerId(),
        m_fd,
        m_client_name ? (char*)m_client_name->ptr : "",
        (long long)(server.unixtime - m_ctime),
        (long long)(server.unixtime - m_last_interaction_time),
        flags,
        m_cur_selected_db->m_id,
        (int) m_pubsub_channels->dictSize(),
        (int) m_pubsub_patterns->listLength(),
        (m_flags & CLIENT_MULTI) ? m_multi_exec_state.m_count : -1,
        (unsigned long long) sdslen(m_query_buf),
        (unsigned long long) sdsavail(m_query_buf),
        (unsigned long long) m_response_buff_pos,
        (unsigned long long) m_reply->listLength(),
        (unsigned long long) getClientOutputBufferMemoryUsage(),
        events,
        m_last_cmd ? m_last_cmd->name : "NULL");
}

sds getAllClientsInfoString() {
    listNode *ln;
    sds o = sdsnewlen(NULL,200*server.clients->listLength());
    sdsclear(o);
    listIter li(server.clients);
    while ((ln = li.listNext()) != NULL) {
        client* _client = (client *)ln->listNodeValue();
        o = _client->catClientInfoString(o);
        o = sdscatlen(o,"\n",1);
    }
    return o;
}

void clientCommand(client *c) {
    listNode *ln;
    client *_client;

    if (!strcasecmp((const char*)c->m_argv[1]->ptr,"list") && c->m_argc == 2) {
        /* CLIENT LIST */
        sds o = getAllClientsInfoString();
        c->addReplyBulkCBuffer(o,sdslen(o));
        sdsfree(o);
    } else if (!strcasecmp((const char*)c->m_argv[1]->ptr,"reply") && c->m_argc == 3) {
        /* CLIENT REPLY ON|OFF|SKIP */
        if (!strcasecmp((const char*)c->m_argv[2]->ptr,"on")) {
            c->m_flags &= ~(CLIENT_REPLY_SKIP|CLIENT_REPLY_OFF);
            c->addReply(shared.ok);
        } else if (!strcasecmp((const char*)c->m_argv[2]->ptr,"off")) {
            c->m_flags |= CLIENT_REPLY_OFF;
        } else if (!strcasecmp((const char*)c->m_argv[2]->ptr,"skip")) {
            if (!(c->m_flags & CLIENT_REPLY_OFF))
                c->m_flags |= CLIENT_REPLY_SKIP_NEXT;
        } else {
            c->addReply(shared.syntaxerr);
            return;
        }
    } else if (!strcasecmp((const char*)c->m_argv[1]->ptr,"kill")) {
        /* CLIENT KILL <ip:port>
         * CLIENT KILL <option> [value] ... <option> [value] */
        char *addr = NULL;
        int type = -1;
        uint64_t id = 0;
        int skipme = 1;
        int killed = 0, close_this_client = 0;

        if (c->m_argc == 3) {
            /* Old style syntax: CLIENT KILL <addr> */
            addr = (char *)c->m_argv[2]->ptr;
            skipme = 0; /* With the old form, you can kill yourself. */
        } else if (c->m_argc > 3) {
            int i = 2; /* Next option index. */

            /* New style syntax: parse options. */
            while(i < c->m_argc) {
                int moreargs = c->m_argc > i+1;

                if (!strcasecmp((const char*)c->m_argv[i]->ptr,"id") && moreargs) {
                    long long tmp;

                    if (getLongLongFromObjectOrReply(c,c->m_argv[i+1],&tmp,NULL)
                        != C_OK) return;
                    id = tmp;
                } else if (!strcasecmp((const char*)c->m_argv[i]->ptr,"type") && moreargs) {
                    type = client::getClientTypeByName((char*)c->m_argv[i+1]->ptr);
                    if (type == -1) {
                        c->addReplyErrorFormat("Unknown client type '%s'",
                            (char*) c->m_argv[i+1]->ptr);
                        return;
                    }
                } else if (!strcasecmp((const char*)c->m_argv[i]->ptr,"addr") && moreargs) {
                    addr = (char *)c->m_argv[i+1]->ptr;
                } else if (!strcasecmp((const char*)c->m_argv[i]->ptr,"skipme") && moreargs) {
                    if (!strcasecmp((const char*)c->m_argv[i+1]->ptr,"yes")) {
                        skipme = 1;
                    } else if (!strcasecmp((const char*)c->m_argv[i+1]->ptr,"no")) {
                        skipme = 0;
                    } else {
                        c->addReply(shared.syntaxerr);
                        return;
                    }
                } else {
                    c->addReply(shared.syntaxerr);
                    return;
                }
                i += 2;
            }
        } else {
            c->addReply(shared.syntaxerr);
            return;
        }

        /* Iterate clients killing all the matching clients. */
        listIter li(server.clients);
        while ((ln = li.listNext()) != NULL) {
            _client = (client *)ln->listNodeValue();
            if (addr && strcmp(_client->getClientPeerId(),addr) != 0) continue;
            if (type != -1 && _client->getClientType() != type) continue;
            if (id != 0 && _client->m_id != id) continue;
            if (c == _client && skipme) continue;

            /* Kill it. */
            if (c == _client) {
                close_this_client = 1;
            } else {
                freeClient(_client);
            }
            killed++;
        }

        /* Reply according to old/new format. */
        if (c->m_argc == 3) {
            if (killed == 0)
                c->addReplyError("No such client");
            else
                c->addReply(shared.ok);
        } else {
            c->addReplyLongLong(killed);
        }

        /* If this client has to be closed, flag it as CLOSE_AFTER_REPLY
         * only after we queued the reply to its output buffers. */
        if (close_this_client) c->m_flags |= CLIENT_CLOSE_AFTER_REPLY;
    } else if (!strcasecmp((const char*)c->m_argv[1]->ptr,"setname") && c->m_argc == 3) {
        int j;
        int len = sdslen((sds)c->m_argv[2]->ptr);
        char *p = (char *)c->m_argv[2]->ptr;

        /* Setting the client name to an empty string actually removes
         * the current name. */
        if (len == 0) {
            if (c->m_client_name) decrRefCount(c->m_client_name);
            c->m_client_name = NULL;
            c->addReply(shared.ok);
            return;
        }

        /* Otherwise check if the charset is ok. We need to do this otherwise
         * CLIENT LIST format will break. You should always be able to
         * split by space to get the different fields. */
        for (j = 0; j < len; j++) {
            if (p[j] < '!' || p[j] > '~') { /* ASCII is assumed. */
                c->addReplyError(
                    "Client names cannot contain spaces, "
                    "newlines or special characters.");
                return;
            }
        }
        if (c->m_client_name) decrRefCount(c->m_client_name);
        c->m_client_name = c->m_argv[2];
        incrRefCount(c->m_client_name);
        c->addReply(shared.ok);
    } else if (!strcasecmp((const char*)c->m_argv[1]->ptr,"getname") && c->m_argc == 2) {
        if (c->m_client_name)
            c->addReplyBulk(c->m_client_name);
        else
            c->addReply(shared.nullbulk);
    } else if (!strcasecmp((const char*)c->m_argv[1]->ptr,"pause") && c->m_argc == 3) {
        long long duration;

        if (getTimeoutFromObjectOrReply(c,c->m_argv[2],&duration,UNIT_MILLISECONDS)
                                        != C_OK) return;
        pauseClients(duration);
        c->addReply(shared.ok);
    } else {
        c->addReplyError( "Syntax error, try CLIENT (LIST | KILL | GETNAME | SETNAME | PAUSE | REPLY)");
    }
}

/* This callback is bound to POST and "Host:" command names. Those are not
 * really commands, but are used in security attacks in order to talk to
 * Redis instances via HTTP, with a technique called "cross protocol scripting"
 * which exploits the fact that services like Redis will discard invalid
 * HTTP headers and will process what follows.
 *
 * As a protection against this attack, Redis will terminate the connection
 * when a POST or "Host:" header is seen, and will log the event from
 * time to time (to avoid creating a DOS as a result of too many logs). */
void securityWarningCommand(client *c) {
    static time_t logged_time;
    time_t now = time(NULL);

    if (labs(now-logged_time) > 60) {
        serverLog(LL_WARNING,"Possible SECURITY ATTACK detected. It looks like somebody is sending POST or Host: commands to Redis. This is likely due to an attacker attempting to use Cross Protocol Scripting to compromise your Redis instance. Connection aborted.");
        logged_time = now;
    }
    freeClientAsync(c);
}

/* Rewrite the command vector of the client. All the new objects ref count
 * is incremented. The old command vector is freed, and the old objects
 * ref count is decremented. */
void client::rewriteClientCommandVector(int argc, ...) {
    va_list ap;
    int j;

    robj **argv = (robj **)zmalloc(sizeof(robj*)*argc);
    va_start(ap,argc);
    for (j = 0; j < argc; j++) {
        robj *a;

        a = va_arg(ap, robj*);
        argv[j] = a;
        incrRefCount(a);
    }
    /* We free the objects in the original vector at the end, so we are
     * sure that if the same objects are reused in the new vector the
     * refcount gets incremented before it gets decremented. */
    for (j = 0; j < m_argc; j++)
        decrRefCount(m_argv[j]);
    zfree(m_argv);
    /* Replace argv and argc with our new versions. */
    m_argv = argv;
    m_argc = argc;
    m_cmd = lookupCommandOrOriginal((sds)m_argv[0]->ptr);
    serverAssertWithInfo(this,NULL,m_cmd != NULL);
    va_end(ap);
}

/* Completely replace the client command vector with the provided one. */
void client::replaceClientCommandVector(int argc, robj **argv) {
    freeClientArgv(this);
    zfree(m_argv);
    m_argv = argv;
    m_argc = argc;
    m_cmd = lookupCommandOrOriginal((sds)m_argv[0]->ptr);
    serverAssertWithInfo(this,NULL,m_cmd != NULL);
}

/* Rewrite a single item in the command vector.
 * The new val ref count is incremented, and the old decremented.
 *
 * It is possible to specify an argument over the current size of the
 * argument vector: in this case the array of objects gets reallocated
 * and c->argc set to the max value. However it's up to the caller to
 *
 * 1. Make sure there are no "holes" and all the arguments are set.
 * 2. If the original argument vector was longer than the one we
 *    want to end with, it's up to the caller to set c->argc and
 *    free the no longer used objects on c->argv. */
void client::rewriteClientCommandArgument(int i, robj *newval) {
    robj *oldval;

    if (i >= m_argc) {
        m_argv = (robj **)zrealloc(m_argv,sizeof(robj*)*(i+1));
        m_argc = i+1;
        m_argv[i] = NULL;
    }
    oldval = m_argv[i];
    m_argv[i] = newval;
    incrRefCount(newval);
    if (oldval)
        decrRefCount(oldval);

    /* If this is the command name make sure to fix c->cmd. */
    if (i == 0) {
        m_cmd = lookupCommandOrOriginal((sds)m_argv[0]->ptr);
        serverAssertWithInfo(this,NULL,m_cmd != NULL);
    }
}

/* This function returns the number of bytes that Redis is virtually
 * using to store the reply still not read by the client.
 * It is "virtual" since the reply output list may contain objects that
 * are shared and are not really using additional memory.
 *
 * The function returns the total sum of the length of all the objects
 * stored in the output list, plus the memory used to allocate every
 * list node. The static reply buffer is not taken into account since it
 * is allocated anyway.
 *
 * Note: this function is very fast so can be called as many time as
 * the caller wishes. The main usage of this function currently is
 * enforcing the client output length limits. */
unsigned long client::getClientOutputBufferMemoryUsage() {
    unsigned long list_item_size = sizeof(listNode)+5;
    /* The +5 above means we assume an sds16 hdr, may not be true
     * but is not going to be a problem. */

    return m_reply_bytes + (list_item_size*m_reply->listLength());
}

/* Get the class of a client, used in order to enforce limits to different
 * classes of clients.
 *
 * The function will return one of the following:
 * CLIENT_TYPE_NORMAL -> Normal client
 * CLIENT_TYPE_SLAVE  -> Slave or client executing MONITOR command
 * CLIENT_TYPE_PUBSUB -> Client subscribed to Pub/Sub channels
 * CLIENT_TYPE_MASTER -> The client representing our replication master.
 */
int client::getClientType() {
    if (m_flags & CLIENT_MASTER)
        return CLIENT_TYPE_MASTER;
    if ((m_flags & CLIENT_SLAVE) && !(m_flags & CLIENT_MONITOR))
        return CLIENT_TYPE_SLAVE;
    if (m_flags & CLIENT_PUBSUB)
        return CLIENT_TYPE_PUBSUB;
    return CLIENT_TYPE_NORMAL;
}

int client::getClientTypeByName(char *name) {
    if (!strcasecmp(name,"normal")) return CLIENT_TYPE_NORMAL;
    else if (!strcasecmp(name,"slave")) return CLIENT_TYPE_SLAVE;
    else if (!strcasecmp(name,"pubsub")) return CLIENT_TYPE_PUBSUB;
    else if (!strcasecmp(name,"master")) return CLIENT_TYPE_MASTER;
    else return -1;
}

char *client::getClientTypeName(int client_name) {
    switch(client_name) {
    case CLIENT_TYPE_NORMAL: return "normal";
    case CLIENT_TYPE_SLAVE:  return "slave";
    case CLIENT_TYPE_PUBSUB: return "pubsub";
    case CLIENT_TYPE_MASTER: return "master";
    default:                 return NULL;
    }
}

/* The function checks if the client reached output buffer soft or hard
 * limit, and also update the state needed to check the soft limit as
 * a side effect.
 *
 * Return value: non-zero if the client reached the soft or the hard limit.
 *               Otherwise zero is returned. */
int client::checkClientOutputBufferLimits() {
    int soft = 0;
    int hard = 0;
    int _class;
    unsigned long used_mem = getClientOutputBufferMemoryUsage();

    _class = getClientType();
    /* For the purpose of output buffer limiting, masters are handled
     * like normal clients. */
    if (_class == CLIENT_TYPE_MASTER)
        _class = CLIENT_TYPE_NORMAL;

    if (server.client_obuf_limits[_class].hard_limit_bytes &&
        used_mem >= server.client_obuf_limits[_class].hard_limit_bytes)
        hard = 1;
    if (server.client_obuf_limits[_class].soft_limit_bytes &&
        used_mem >= server.client_obuf_limits[_class].soft_limit_bytes)
        soft = 1;

    /* We need to check if the soft limit is reached continuously for the
     * specified amount of seconds. */
    if (soft) {
        if (m_obuf_soft_limit_reached_time == 0) {
            m_obuf_soft_limit_reached_time = server.unixtime;
            soft = 0; /* First time we see the soft limit reached */
        } else {
            time_t elapsed = server.unixtime - m_obuf_soft_limit_reached_time;

            if (elapsed <=
                server.client_obuf_limits[_class].soft_limit_seconds) {
                soft = 0; /* The client still did not reached the max number of
                             seconds for the soft limit to be considered
                             reached. */
            }
        }
    } else {
        m_obuf_soft_limit_reached_time = 0;
    }
    return soft || hard;
}

/* Asynchronously close a client if soft or hard limit is reached on the
 * output buffer size. The caller can check if the client will be closed
 * checking if the client CLIENT_CLOSE_ASAP flag is set.
 *
 * Note: we need to close the client asynchronously because this function is
 * called from contexts where the client can't be freed safely, i.e. from the
 * lower level functions pushing data inside the client output buffers. */
void asyncCloseClientOnOutputBufferLimitReached(client *c) {
    serverAssert(c->m_reply_bytes < SIZE_MAX-(1024*64));
    if (c->m_reply_bytes == 0 || c->m_flags & CLIENT_CLOSE_ASAP)
        return;
    if (c->checkClientOutputBufferLimits()) {
        sds client = c->catClientInfoString(sdsempty());

        freeClientAsync(c);
        serverLog(LL_WARNING,"Client %s scheduled to be closed ASAP for overcoming of output buffer limits.", client);
        sdsfree(client);
    }
}

/* Helper function used by freeMemoryIfNeeded() in order to flush slaves
 * output buffers without returning control to the event loop.
 * This is also called by SHUTDOWN for a best-effort attempt to send
 * slaves the latest writes. */
void flushSlavesOutputBuffers() {
    listNode *ln;

    listIter li(server.slaves);
    while((ln = li.listNext())) {
        client *slave = (client *)ln->listNodeValue();
        int events;

        /* Note that the following will not flush output buffers of slaves
         * in STATE_ONLINE but having put_online_on_ack set to true: in this
         * case the writable event is never installed, since the purpose
         * of put_online_on_ack is to postpone the moment it is installed.
         * This is what we want since slaves in this state should not receive
         * writes before the first ACK. */
        events = server.el->aeGetFileEvents(slave->m_fd);
        if (events & AE_WRITABLE &&
            slave->m_replication_state == SLAVE_STATE_ONLINE &&
            slave->clientHasPendingReplies())
        {
            writeToClient(slave->m_fd,slave,0);
        }
    }
}

/* Pause clients up to the specified unixtime (in ms). While clients
 * are paused no command is processed from clients, so the data set can't
 * change during that time.
 *
 * However while this function pauses normal and Pub/Sub clients, slaves are
 * still served, so this function can be used on server upgrades where it is
 * required that slaves process the latest bytes from the replication stream
 * before being turned to masters.
 *
 * This function is also internally used by Redis Cluster for the manual
 * failover procedure implemented by CLUSTER FAILOVER.
 *
 * The function always succeed, even if there is already a pause in progress.
 * In such a case, the pause is extended if the duration is more than the
 * time left for the previous duration. However if the duration is smaller
 * than the time left for the previous pause, no change is made to the
 * left duration. */
void pauseClients(mstime_t end) {
    if (!server.clients_paused || end > server.clients_pause_end_time)
        server.clients_pause_end_time = end;
    server.clients_paused = 1;
}

/* Return non-zero if clients are currently paused. As a side effect the
 * function checks if the pause time was reached and clear it. */
int clientsArePaused() {
    if (server.clients_paused &&
        server.clients_pause_end_time < server.mstime)
    {
        listNode *ln;
        client *c;

        server.clients_paused = 0;

        /* Put all the clients in the unblocked clients queue in order to
         * force the re-processing of the input buffer if any. */
        listIter li(server.clients);
        while ((ln = li.listNext()) != NULL) {
            c = (client *)ln->listNodeValue();

            /* Don't touch slaves and blocked clients. The latter pending
             * requests be processed when unblocked. */
            if (c->m_flags & (CLIENT_SLAVE|CLIENT_BLOCKED)) continue;
            c->m_flags |= CLIENT_UNBLOCKED;
            server.unblocked_clients->listAddNodeTail(c);
        }
    }
    return server.clients_paused;
}

/* This function is called by Redis in order to process a few events from
 * time to time while blocked into some not interruptible operation.
 * This allows to reply to clients with the -LOADING error while loading the
 * data set at startup or after a full resynchronization with the master
 * and so forth.
 *
 * It calls the event loop in order to process a few events. Specifically we
 * try to call the event loop 4 times as long as we receive acknowledge that
 * some event was processed, in order to go forward with the accept, read,
 * write, close sequence needed to serve a client.
 *
 * The function returns the total number of events processed. */
int processEventsWhileBlocked() {
    int iterations = 4; /* See the function top-comment. */
    int count = 0;
    while (iterations--) {
        int events = 0;
        events += server.el->aeProcessEvents(AE_FILE_EVENTS|AE_DONT_WAIT);
        events += handleClientsWithPendingWrites();
        if (!events) break;
        count += events;
    }
    return count;
}
