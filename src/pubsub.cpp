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

/*-----------------------------------------------------------------------------
 * Pubsub low level API
 *----------------------------------------------------------------------------*/

void freePubsubPattern(void *p) {
    pubsubPattern *pat = (pubsubPattern *)p;

    decrRefCount(pat->pattern);
    zfree(pat);
}

int listMatchPubsubPattern(void *a, void *b) {
    pubsubPattern *pa = (pubsubPattern *)a;
    pubsubPattern *pb = (pubsubPattern *)b;

    return (pa->client == pb->client) &&
           (equalStringObjects(pa->pattern,pb->pattern));
}

/* Return the number of channels + patterns a client is subscribed to. */
int clientSubscriptionsCount(client *c) {
    return c->m_pubsub_channels->dictSize()+
           c->m_pubsub_patterns->listLength();
}

/* Subscribe a client to a channel. Returns 1 if the operation succeeded, or
 * 0 if the client was already subscribed to that channel. */
int pubsubSubscribeChannel(client *c, robj *channel) {
    dictEntry *de;
    list *clients = NULL;
    int retval = 0;

    /* Add the channel to the client -> channels hash table */
    if (c->m_pubsub_channels->dictAdd(channel,NULL) == DICT_OK) {
        retval = 1;
        incrRefCount(channel);
        /* Add the client to the channel -> list of clients hash table */
        de = server.pubsub_channels->dictFind(channel);
        if (de == NULL) {
            clients = listCreate();
            server.pubsub_channels->dictAdd(channel,clients);
            incrRefCount(channel);
        } else {
            clients = (list *)de->dictGetVal();
        }
        clients->listAddNodeTail(c);
    }
    /* Notify the client */
    c->addReply(shared.mbulkhdr[3]);
    c->addReply(shared.subscribebulk);
    addReplyBulk(c,channel);
    addReplyLongLong(c,clientSubscriptionsCount(c));
    return retval;
}

/* Unsubscribe a client from a channel. Returns 1 if the operation succeeded, or
 * 0 if the client was not subscribed to the specified channel. */
int pubsubUnsubscribeChannel(client *c, robj *channel, int notify) {
    dictEntry *de;
    list *clients;
    listNode *ln;
    int retval = 0;

    /* Remove the channel from the client -> channels hash table */
    incrRefCount(channel); /* channel may be just a pointer to the same object
                            we have in the hash tables. Protect it... */
    if (c->m_pubsub_channels->dictDelete(channel) == DICT_OK) {
        retval = 1;
        /* Remove the client from the channel -> clients list hash table */
        de = server.pubsub_channels->dictFind(channel);
        serverAssertWithInfo(c,NULL,de != NULL);
        clients = (list *)de->dictGetVal();
        ln = clients->listSearchKey(c);
        serverAssertWithInfo(c,NULL,ln != NULL);
        clients->listDelNode(ln);
        if (clients->listLength() == 0) {
            /* Free the list and associated hash entry at all if this was
             * the latest client, so that it will be possible to abuse
             * Redis PUBSUB creating millions of channels. */
            server.pubsub_channels->dictDelete(channel);
        }
    }
    /* Notify the client */
    if (notify) {
        c->addReply(shared.mbulkhdr[3]);
        c->addReply(shared.unsubscribebulk);
        addReplyBulk(c,channel);
        addReplyLongLong(c,c->m_pubsub_channels->dictSize()+
                       c->m_pubsub_patterns->listLength());

    }
    decrRefCount(channel); /* it is finally safe to release it */
    return retval;
}

/* Subscribe a client to a pattern. Returns 1 if the operation succeeded, or 0 if the client was already subscribed to that pattern. */
int pubsubSubscribePattern(client *c, robj *pattern) {
    int retval = 0;

    if (c->m_pubsub_patterns->listSearchKey(pattern) == NULL) {
        retval = 1;
        pubsubPattern *pat;
        c->m_pubsub_patterns->listAddNodeTail(pattern);
        incrRefCount(pattern);
        pat = (pubsubPattern *)zmalloc(sizeof(*pat));
        pat->pattern = getDecodedObject(pattern);
        pat->client = c;
        server.pubsub_patterns->listAddNodeTail(pat);
    }
    /* Notify the client */
    c->addReply(shared.mbulkhdr[3]);
    c->addReply(shared.psubscribebulk);
    addReplyBulk(c,pattern);
    addReplyLongLong(c,clientSubscriptionsCount(c));
    return retval;
}

/* Unsubscribe a client from a channel. Returns 1 if the operation succeeded, or
 * 0 if the client was not subscribed to the specified channel. */
int pubsubUnsubscribePattern(client *c, robj *pattern, int notify) {
    listNode *ln;
    pubsubPattern pat;
    int retval = 0;

    incrRefCount(pattern); /* Protect the object. May be the same we remove */
    if ((ln = c->m_pubsub_patterns->listSearchKey(pattern)) != NULL) {
        retval = 1;
        c->m_pubsub_patterns->listDelNode(ln);
        pat.client = c;
        pat.pattern = pattern;
        ln = server.pubsub_patterns->listSearchKey(&pat);
        server.pubsub_patterns->listDelNode(ln);
    }
    /* Notify the client */
    if (notify) {
        c->addReply(shared.mbulkhdr[3]);
        c->addReply(shared.punsubscribebulk);
        addReplyBulk(c,pattern);
        addReplyLongLong(c,c->m_pubsub_channels->dictSize()+
                       c->m_pubsub_patterns->listLength());
    }
    decrRefCount(pattern);
    return retval;
}

/* Unsubscribe from all the channels. Return the number of channels the
 * client was subscribed to. */
int pubsubUnsubscribeAllChannels(client *c, int notify) {
    dictEntry *de;
    int count = 0;

    dictIterator di(c->m_pubsub_channels, 1);
    while((de = di.dictNext()) != NULL) {
        robj *channel = (robj *)de->dictGetKey();

        count += pubsubUnsubscribeChannel(c,channel,notify);
    }
    /* We were subscribed to nothing? Still reply to the client. */
    if (notify && count == 0) {
        c->addReply(shared.mbulkhdr[3]);
        c->addReply(shared.unsubscribebulk);
        c->addReply(shared.nullbulk);
        addReplyLongLong(c,c->m_pubsub_channels->dictSize()+
                       c->m_pubsub_patterns->listLength());
    }

    return count;
}

/* Unsubscribe from all the patterns. Return the number of patterns the
 * client was subscribed from. */
int pubsubUnsubscribeAllPatterns(client *c, int notify) {
    listNode *ln;
    int count = 0;

    listIter li(c->m_pubsub_patterns);
    while ((ln = li.listNext()) != NULL) {
        robj *pattern = (robj *)ln->listNodeValue();

        count += pubsubUnsubscribePattern(c,pattern,notify);
    }
    if (notify && count == 0) {
        /* We were subscribed to nothing? Still reply to the client. */
        c->addReply(shared.mbulkhdr[3]);
        c->addReply(shared.punsubscribebulk);
        c->addReply(shared.nullbulk);
        addReplyLongLong(c,c->m_pubsub_channels->dictSize()+
                       c->m_pubsub_patterns->listLength());
    }
    return count;
}

/* Publish a message */
int pubsubPublishMessage(robj *channel, robj *message) {
    int receivers = 0;
    dictEntry *de;
    listNode *ln;

    /* Send to clients listening for that channel */
    de = server.pubsub_channels->dictFind(channel);
    if (de) {
        list* _list = (list*)de->dictGetVal();
        listNode *ln;

        listIter li(_list);
        while ((ln = li.listNext()) != NULL) {
            client *c = (client *)ln->listNodeValue();

            c->addReply(shared.mbulkhdr[3]);
            c->addReply(shared.messagebulk);
            addReplyBulk(c,channel);
            addReplyBulk(c,message);
            receivers++;
        }
    }
    /* Send to clients listening to matching channels */
    if (server.pubsub_patterns->listLength()) {
        listIter li(server.pubsub_patterns);
        channel = getDecodedObject(channel);
        while ((ln = li.listNext()) != NULL) {
            pubsubPattern *pat = (pubsubPattern *)ln->listNodeValue();

            if (stringmatchlen((char*)pat->pattern->ptr,
                                sdslen((sds)pat->pattern->ptr),
                                (char*)channel->ptr,
                                sdslen((sds)channel->ptr),0)) {
                pat->client->addReply(shared.mbulkhdr[4]);
                pat->client->addReply(shared.pmessagebulk);
                addReplyBulk(pat->client,pat->pattern);
                addReplyBulk(pat->client,channel);
                addReplyBulk(pat->client,message);
                receivers++;
            }
        }
        decrRefCount(channel);
    }
    return receivers;
}

/*-----------------------------------------------------------------------------
 * Pubsub commands implementation
 *----------------------------------------------------------------------------*/

void subscribeCommand(client *c) {
    int j;

    for (j = 1; j < c->m_argc; j++)
        pubsubSubscribeChannel(c,c->m_argv[j]);
    c->m_flags |= CLIENT_PUBSUB;
}

void unsubscribeCommand(client *c) {
    if (c->m_argc == 1) {
        pubsubUnsubscribeAllChannels(c,1);
    } else {
        int j;

        for (j = 1; j < c->m_argc; j++)
            pubsubUnsubscribeChannel(c,c->m_argv[j],1);
    }
    if (clientSubscriptionsCount(c) == 0) c->m_flags &= ~CLIENT_PUBSUB;
}

void psubscribeCommand(client *c) {
    int j;

    for (j = 1; j < c->m_argc; j++)
        pubsubSubscribePattern(c,c->m_argv[j]);
    c->m_flags |= CLIENT_PUBSUB;
}

void punsubscribeCommand(client *c) {
    if (c->m_argc == 1) {
        pubsubUnsubscribeAllPatterns(c,1);
    } else {
        int j;

        for (j = 1; j < c->m_argc; j++)
            pubsubUnsubscribePattern(c,c->m_argv[j],1);
    }
    if (clientSubscriptionsCount(c) == 0) c->m_flags &= ~CLIENT_PUBSUB;
}

void publishCommand(client *c) {
    int receivers = pubsubPublishMessage(c->m_argv[1],c->m_argv[2]);
    if (server.cluster_enabled)
        clusterPropagatePublish(c->m_argv[1],c->m_argv[2]);
    else
        forceCommandPropagation(c,PROPAGATE_REPL);
    addReplyLongLong(c,receivers);
}

/* PUBSUB command for Pub/Sub introspection. */
void pubsubCommand(client *c) {
    if (!strcasecmp((const char*)c->m_argv[1]->ptr,"channels") &&
        (c->m_argc == 2 || c->m_argc ==3))
    {
        /* PUBSUB CHANNELS [<pattern>] */
        sds pat = (c->m_argc == 2) ? NULL : (sds)c->m_argv[2]->ptr;
        dictIterator di(server.pubsub_channels);
        dictEntry *de;
        long mblen = 0;
        void *replylen;

        replylen = addDeferredMultiBulkLength(c);
        while((de = di.dictNext()) != NULL) {
            robj *cobj = (robj *)de->dictGetKey();
            sds channel = (sds)cobj->ptr;

            if (!pat || stringmatchlen(pat, sdslen(pat),
                                       channel, sdslen(channel),0))
            {
                addReplyBulk(c,cobj);
                mblen++;
            }
        }
        setDeferredMultiBulkLength(c,replylen,mblen);
    } else if (!strcasecmp((const char*)c->m_argv[1]->ptr,"numsub") && c->m_argc >= 2) {
        /* PUBSUB NUMSUB [Channel_1 ... Channel_N] */
        int j;

        addReplyMultiBulkLen(c,(c->m_argc-2)*2);
        for (j = 2; j < c->m_argc; j++) {
            list *l = (list *)server.pubsub_channels->dictFetchValue(c->m_argv[j]);

            addReplyBulk(c,c->m_argv[j]);
            addReplyLongLong(c,l ? l->listLength() : 0);
        }
    } else if (!strcasecmp((const char*)c->m_argv[1]->ptr,"numpat") && c->m_argc == 2) {
        /* PUBSUB NUMPAT */
        addReplyLongLong(c,server.pubsub_patterns->listLength());
    } else {
        addReplyErrorFormat(c,
            "Unknown PUBSUB subcommand or wrong number of arguments for '%s'",
            (char*)c->m_argv[1]->ptr);
    }
}
