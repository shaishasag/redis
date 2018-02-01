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

/* ================================ MULTI/EXEC ============================== */

/* Client state initialization for MULTI/EXEC */
void initClientMultiState(client *c) {
    c->m_multi_exec_state.m_commands = NULL;
    c->m_multi_exec_state.m_count = 0;
}

/* Release all the resources associated with MULTI/EXEC state */
void freeClientMultiState(client *c) {
    int j;

    for (j = 0; j < c->m_multi_exec_state.m_count; j++) {
        int i;
        multiCmd *mc = c->m_multi_exec_state.m_commands+j;

        for (i = 0; i < mc->argc; i++)
            decrRefCount(mc->argv[i]);
        zfree(mc->argv);
    }
    zfree(c->m_multi_exec_state.m_commands);
}

/* Add a new command into the MULTI commands queue */
void queueMultiCommand(client *c) {
    multiCmd *mc;
    int j;

    c->m_multi_exec_state.m_commands = (multiCmd *)zrealloc(c->m_multi_exec_state.m_commands,
            sizeof(multiCmd)*(c->m_multi_exec_state.m_count+1));
    mc = c->m_multi_exec_state.m_commands+c->m_multi_exec_state.m_count;
    mc->cmd = c->m_cmd;
    mc->argc = c->m_argc;
    mc->argv = (robj **)zmalloc(sizeof(robj*)*c->m_argc);
    memcpy(mc->argv,c->m_argv,sizeof(robj*)*c->m_argc);
    for (j = 0; j < c->m_argc; j++)
        incrRefCount(mc->argv[j]);
    c->m_multi_exec_state.m_count++;
}

void discardTransaction(client *c) {
    freeClientMultiState(c);
    initClientMultiState(c);
    c->m_flags &= ~(CLIENT_MULTI|CLIENT_DIRTY_CAS|CLIENT_DIRTY_EXEC);
    unwatchAllKeys(c);
}

/* Flag the transacation as DIRTY_EXEC so that EXEC will fail.
 * Should be called every time there is an error while queueing a command. */
void flagTransaction(client *c) {
    if (c->m_flags & CLIENT_MULTI)
        c->m_flags |= CLIENT_DIRTY_EXEC;
}

void multiCommand(client *c) {
    if (c->m_flags & CLIENT_MULTI) {
        c->addReplyError("MULTI calls can not be nested");
        return;
    }
    c->m_flags |= CLIENT_MULTI;
    c->addReply(shared.ok);
}

void discardCommand(client *c) {
    if (!(c->m_flags & CLIENT_MULTI)) {
        c->addReplyError("DISCARD without MULTI");
        return;
    }
    discardTransaction(c);
    c->addReply(shared.ok);
}

/* Send a MULTI command to all the slaves and AOF file. Check the execCommand
 * implementation for more information. */
void execCommandPropagateMulti(client *c) {
    robj *multistring = createStringObject("MULTI",5);

    propagate(server.multiCommand,c->m_cur_selected_db->m_id,&multistring,1,
              PROPAGATE_AOF|PROPAGATE_REPL);
    decrRefCount(multistring);
}

void execCommand(client *c) {
    int j;
    robj **orig_argv;
    int orig_argc;
    struct redisCommand *orig_cmd;
    int must_propagate = 0; /* Need to propagate MULTI/EXEC to AOF / slaves? */
    int was_master = server.masterhost == NULL;

    if (!(c->m_flags & CLIENT_MULTI)) {
        c->addReplyError("EXEC without MULTI");
        return;
    }

    /* Check if we need to abort the EXEC because:
     * 1) Some WATCHed key was touched.
     * 2) There was a previous error while queueing commands.
     * A failed EXEC in the first case returns a multi bulk nil object
     * (technically it is not an error but a special behavior), while
     * in the second an EXECABORT error is returned. */
    if (c->m_flags & (CLIENT_DIRTY_CAS|CLIENT_DIRTY_EXEC)) {
        c->addReply( c->m_flags & CLIENT_DIRTY_EXEC ? shared.execaborterr :
                                                  shared.nullmultibulk);
        discardTransaction(c);
        goto handle_monitor;
    }

    /* Exec all the queued commands */
    unwatchAllKeys(c); /* Unwatch ASAP otherwise we'll waste CPU cycles */
    orig_argv = c->m_argv;
    orig_argc = c->m_argc;
    orig_cmd = c->m_cmd;
    c->addReplyMultiBulkLen(c->m_multi_exec_state.m_count);
    for (j = 0; j < c->m_multi_exec_state.m_count; j++) {
        c->m_argc = c->m_multi_exec_state.m_commands[j].argc;
        c->m_argv = c->m_multi_exec_state.m_commands[j].argv;
        c->m_cmd = c->m_multi_exec_state.m_commands[j].cmd;

        /* Propagate a MULTI request once we encounter the first command which
         * is not readonly nor an administrative one.
         * This way we'll deliver the MULTI/..../EXEC block as a whole and
         * both the AOF and the replication link will have the same consistency
         * and atomicity guarantees. */
        if (!must_propagate && !(c->m_cmd->m_flags & (CMD_READONLY|CMD_ADMIN))) {
            execCommandPropagateMulti(c);
            must_propagate = 1;
        }

        call(c,CMD_CALL_FULL);

        /* Commands may alter argc/argv, restore mstate. */
        c->m_multi_exec_state.m_commands[j].argc = c->m_argc;
        c->m_multi_exec_state.m_commands[j].argv = c->m_argv;
        c->m_multi_exec_state.m_commands[j].cmd = c->m_cmd;
    }
    c->m_argv = orig_argv;
    c->m_argc = orig_argc;
    c->m_cmd = orig_cmd;
    discardTransaction(c);

    /* Make sure the EXEC command will be propagated as well if MULTI
     * was already propagated. */
    if (must_propagate) {
        int is_master = server.masterhost == NULL;
        server.dirty++;
        /* If inside the MULTI/EXEC block this instance was suddenly
         * switched from master to slave (using the SLAVEOF command), the
         * initial MULTI was propagated into the replication backlog, but the
         * rest was not. We need to make sure to at least terminate the
         * backlog with the final EXEC. */
        if (server.repl_backlog && was_master && !is_master) {
            char *execcmd = "*1\r\n$4\r\nEXEC\r\n";
            feedReplicationBacklog(execcmd,strlen(execcmd));
        }
    }

handle_monitor:
    /* Send EXEC to clients waiting data from MONITOR. We do it here
     * since the natural order of commands execution is actually:
     * MUTLI, EXEC, ... commands inside transaction ...
     * Instead EXEC is flagged as CMD_SKIP_MONITOR in the command
     * table, and we do it here with correct ordering. */
    if (server.monitors->listLength() && !server.loading)
        replicationFeedMonitors(c,server.monitors,c->m_cur_selected_db->m_id,c->m_argv,c->m_argc);
}

/* ===================== WATCH (CAS alike for MULTI/EXEC) ===================
 *
 * The implementation uses a per-DB hash table mapping keys to list of clients
 * WATCHing those keys, so that given a key that is going to be modified
 * we can mark all the associated clients as dirty.
 *
 * Also every client contains a list of WATCHed keys so that's possible to
 * un-watch such keys when the client is freed or when UNWATCH is called. */

/* In the client->watched_keys list we need to use watchedKey structures
 * as in order to identify a key in Redis we need both the key name and the
 * DB */
struct watchedKey {
    robj *key;
    redisDb *db;
};

/* Watch for the specified key */
void watchForKey(client *c, robj *key) {
    list *clients = NULL;
    listNode *ln;
    watchedKey *wk;

    /* Check if we are already watching for this key */
    listIter li(c->m_watched_keys);
    while((ln = li.listNext())) {
        wk = (watchedKey *)ln->listNodeValue();
        if (wk->db == c->m_cur_selected_db && equalStringObjects(key,wk->key))
            return; /* Key already watched */
    }
    /* This key is not already watched in this DB. Let's add it */
    clients = (list *)c->m_cur_selected_db->m_watched_keys->dictFetchValue(key);
    if (!clients) {
        clients = listCreate();
        c->m_cur_selected_db->m_watched_keys->dictAdd(key,clients);
        incrRefCount(key);
    }
    clients->listAddNodeTail(c);
    /* Add the new key to the list of keys watched by this client */
    wk = (watchedKey *)zmalloc(sizeof(*wk));
    wk->key = key;
    wk->db = c->m_cur_selected_db;
    incrRefCount(key);
    c->m_watched_keys->listAddNodeTail(wk);
}

/* Unwatch all the keys watched by this client. To clean the EXEC dirty
 * flag is up to the caller. */
void unwatchAllKeys(client *c) {
    listNode *ln;

    if (c->m_watched_keys->listLength() == 0) return;
    listIter li(c->m_watched_keys);
    while((ln = li.listNext())) {
        /* Lookup the watched key -> clients list and remove the client
         * from the list */
        watchedKey *wk = (watchedKey *)ln->listNodeValue();
        list *clients = (list *)wk->db->m_watched_keys->dictFetchValue(wk->key);
        serverAssertWithInfo(c,NULL,clients != NULL);
        clients->listDelNode(clients->listSearchKey(c));
        /* Kill the entry at all if this was the only client */
        if (clients->listLength() == 0)
            wk->db->m_watched_keys->dictDelete( wk->key);
        /* Remove this watched key from the client->watched list */
        c->m_watched_keys->listDelNode(ln);
        decrRefCount(wk->key);
        zfree(wk);
    }
}

/* "Touch" a key, so that if this key is being WATCHed by some client the
 * next EXEC will fail. */
void touchWatchedKey(redisDb *db, robj *key) {
    list *clients;
    listNode *ln;

    if (db->m_watched_keys->dictSize() == 0) return;
    clients = (list *)db->m_watched_keys->dictFetchValue(key);
    if (!clients) return;

    /* Mark all the clients watching this key as CLIENT_DIRTY_CAS */
    /* Check if we are already watching for this key */
    listIter li(clients);
    while((ln = li.listNext())) {
        client *c = (client *)ln->listNodeValue();

        c->m_flags |= CLIENT_DIRTY_CAS;
    }
}

/* On FLUSHDB or FLUSHALL all the watched keys that are present before the
 * flush but will be deleted as effect of the flushing operation should
 * be touched. "dbid" is the DB that's getting the flush. -1 if it is
 * a FLUSHALL operation (all the DBs flushed). */
void touchWatchedKeysOnFlush(int dbid) {
    listNode *ln;

    /* For every client, check all the waited keys */
    listIter li1(server.clients);
    while((ln = li1.listNext())) {
        client *c = (client *)ln->listNodeValue();
        listIter li2(c->m_watched_keys);
        while((ln = li2.listNext())) {
            watchedKey *wk = (watchedKey *)ln->listNodeValue();

            /* For every watched key matching the specified DB, if the
             * key exists, mark the client as dirty, as the key will be
             * removed. */
            if (dbid == -1 || wk->db->m_id == dbid) {
                if (wk->db->m_dict->dictFind(wk->key->ptr) != NULL)
                    c->m_flags |= CLIENT_DIRTY_CAS;
            }
        }
    }
}

void watchCommand(client *c) {
    int j;

    if (c->m_flags & CLIENT_MULTI) {
        c->addReplyError("WATCH inside MULTI is not allowed");
        return;
    }
    for (j = 1; j < c->m_argc; j++)
        watchForKey(c,c->m_argv[j]);
    c->addReply(shared.ok);
}

void unwatchCommand(client *c) {
    unwatchAllKeys(c);
    c->m_flags &= (~CLIENT_DIRTY_CAS);
    c->addReply(shared.ok);
}
