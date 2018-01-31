/* Redis Cluster implementation.
 *
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
#include "cluster.h"
#include "endianconv.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <math.h>
#include <new>

/* A global reference to myself is handy to make code more clear.
 * Myself always points to server.cluster->m_myself, that is, the clusterNode
 * that represents this node. */
clusterNode *myself = NULL;

clusterNode *createClusterNode(char *nodename, int flags);
int clusterAddNode(clusterNode *node); //!
void clusterAcceptHandler(aeEventLoop *el, int fd, void *privdata, int mask);
void clusterReadHandler(aeEventLoop *el, int fd, void *privdata, int mask);
void clusterSendPing(clusterLink *link, int type); //!
void clusterSendFail(char *nodename);
void clusterSendFailoverAuthIfNeeded(clusterNode *node, clusterMsg *request); //!
void clusterUpdateState(void);
sds clusterGenNodesDescription(int filter);
clusterNode *clusterLookupNode(const char *name);
int clusterDelSlot(int slot);
void clusterSetMaster(clusterNode *n); //!
void clusterHandleSlaveFailover(void);
void clusterHandleSlaveMigration(int max_slaves);
int bitmapTestBit(unsigned char *bitmap, int pos);
void clusterDoBeforeSleep(int flags);
void clusterSendUpdate(clusterLink *link, clusterNode *node);
void resetManualFailover();
void clusterCloseAllSlots();
void clusterDelNode(clusterNode *delnode);
sds representClusterNodeFlags(sds ci, uint16_t flags);
uint64_t clusterGetMaxEpoch();
int clusterBumpConfigEpochWithoutConsensus();

/* -----------------------------------------------------------------------------
 * Initialization
 * -------------------------------------------------------------------------- */

/* Load the cluster config from 'filename'.
 *
 * If the file does not exist or is zero-length (this may happen because
 * when we lock the nodes.conf file, we create a zero-length one for the
 * sake of locking if it does not already exist), C_ERR is returned.
 * If the configuration was loaded from the file, C_OK is returned. */
int clusterLoadConfig(char *filename) {
    FILE *fp = fopen(filename,"r");
    struct stat sb;
    char *line;
    int maxline, j;

    if (fp == NULL) {
        if (errno == ENOENT) {
            return C_ERR;
        } else {
            serverLog(LL_WARNING,
                "Loading the cluster node config from %s: %s",
                filename, strerror(errno));
            exit(1);
        }
    }

    /* Check if the file is zero-length: if so return C_ERR to signal
     * we have to write the config. */
    if (fstat(fileno(fp),&sb) != -1 && sb.st_size == 0) {
        fclose(fp);
        return C_ERR;
    }

    /* Parse the file. Note that single lines of the cluster config file can
     * be really long as they include all the hash slots of the node.
     * This means in the worst possible case, half of the Redis slots will be
     * present in a single line, possibly in importing or migrating state, so
     * together with the node ID of the sender/receiver.
     *
     * To simplify we allocate 1024+CLUSTER_SLOTS*128 bytes per line. */
    maxline = 1024+CLUSTER_SLOTS*128;
    line = (char *)zmalloc(maxline);
    while(fgets(line,maxline,fp) != NULL) {
        int argc;
        sds *argv;
        clusterNode *n, *master;
        char *p, *s;

        /* Skip blank lines, they can be created either by users manually
         * editing nodes.conf or by the config writing process if stopped
         * before the truncate() call. */
        if (line[0] == '\n' || line[0] == '\0') continue;

        /* Split the line into arguments for processing. */
        argv = sdssplitargs(line,&argc);
        if (argv == NULL) goto fmterr;

        /* Handle the special "vars" line. Don't pretend it is the last
         * line even if it actually is when generated by Redis. */
        if (strcasecmp(argv[0],"vars") == 0) {
            for (j = 1; j < argc; j += 2) {
                if (strcasecmp(argv[j],"currentEpoch") == 0) {
                    server.cluster->m_currentEpoch =
                            strtoull(argv[j+1],NULL,10);
                } else if (strcasecmp(argv[j],"lastVoteEpoch") == 0) {
                    server.cluster->m_lastVoteEpoch =
                            strtoull(argv[j+1],NULL,10);
                } else {
                    serverLog(LL_WARNING,
                        "Skipping unknown cluster config variable '%s'",
                        argv[j]);
                }
            }
            sdsfreesplitres(argv,argc);
            continue;
        }

        /* Regular config lines have at least eight fields */
        if (argc < 8) goto fmterr;

        /* Create this node if it does not exist */
        n = clusterLookupNode(argv[0]);
        if (!n) {
            n = createClusterNode(argv[0],0);
            clusterAddNode(n);
        }
        /* Address and port */
        if ((p = strrchr(argv[1],':')) == NULL) goto fmterr;
        *p = '\0';
        memcpy(n->m_ip,argv[1],strlen(argv[1])+1);
        char *port = p+1;
        char *busp = strchr(port,'@');
        if (busp) {
            *busp = '\0';
            busp++;
        }
        n->m_port = atoi(port);
        /* In older versions of nodes.conf the "@busport" part is missing.
         * In this case we set it to the default offset of 10000 from the
         * base port. */
        n->m_cport = busp ? atoi(busp) : n->m_port + CLUSTER_PORT_INCR;

        /* Parse flags */
        p = s = argv[2];
        while(p) {
            p = strchr(s,',');
            if (p) *p = '\0';
            if (!strcasecmp(s,"myself")) {
                serverAssert(server.cluster->m_myself == NULL);
                myself = server.cluster->m_myself = n;
                n->m_flags |= CLUSTER_NODE_MYSELF;
            } else if (!strcasecmp(s,"master")) {
                n->m_flags |= CLUSTER_NODE_MASTER;
            } else if (!strcasecmp(s,"slave")) {
                n->m_flags |= CLUSTER_NODE_SLAVE;
            } else if (!strcasecmp(s,"fail?")) {
                n->m_flags |= CLUSTER_NODE_PFAIL;
            } else if (!strcasecmp(s,"fail")) {
                n->m_flags |= CLUSTER_NODE_FAIL;
                n->m_fail_time = mstime();
            } else if (!strcasecmp(s,"handshake")) {
                n->m_flags |= CLUSTER_NODE_HANDSHAKE;
            } else if (!strcasecmp(s,"noaddr")) {
                n->m_flags |= CLUSTER_NODE_NOADDR;
            } else if (!strcasecmp(s,"noflags")) {
                /* nothing to do */
            } else {
                serverPanic("Unknown flag in redis cluster config file");
            }
            if (p) s = p+1;
        }

        /* Get master if any. Set the master and populate master's
         * slave list. */
        if (argv[3][0] != '-') {
            master = clusterLookupNode(argv[3]);
            if (!master) {
                master = createClusterNode(argv[3],0);
                clusterAddNode(master);
            }
            n->m_slaveof = master;
            master->clusterNodeAddSlave(n);
        }

        /* Set ping sent / pong received timestamps */
        if (atoi(argv[4])) n->m_ping_sent = mstime();
        if (atoi(argv[5])) n->m_pong_received = mstime();

        /* Set configEpoch for this node. */
        n->m_configEpoch = strtoull(argv[6],NULL,10);

        /* Populate hash slots served by this instance. */
        for (j = 8; j < argc; j++) {
            int start, stop;

            if (argv[j][0] == '[') {
                /* Here we handle migrating / importing slots */
                int slot;
                char direction;
                clusterNode *cn;

                p = strchr(argv[j],'-');
                serverAssert(p != NULL);
                *p = '\0';
                direction = p[1]; /* Either '>' or '<' */
                slot = atoi(argv[j]+1);
                if (slot < 0 || slot >= CLUSTER_SLOTS) goto fmterr;
                p += 3;
                cn = clusterLookupNode(p);
                if (!cn) {
                    cn = createClusterNode(p,0);
                    clusterAddNode(cn);
                }
                if (direction == '>') {
                    server.cluster->m_migrating_slots_to[slot] = cn;
                } else {
                    server.cluster->m_importing_slots_from[slot] = cn;
                }
                continue;
            } else if ((p = strchr(argv[j],'-')) != NULL) {
                *p = '\0';
                start = atoi(argv[j]);
                stop = atoi(p+1);
            } else {
                start = stop = atoi(argv[j]);
            }
            if (start < 0 || start >= CLUSTER_SLOTS) goto fmterr;
            if (stop < 0 || stop >= CLUSTER_SLOTS) goto fmterr;
            while(start <= stop) n->clusterAddSlot(start++);
        }

        sdsfreesplitres(argv,argc);
    }
    /* Config sanity check */
    if (server.cluster->m_myself == NULL) goto fmterr;

    zfree(line);
    fclose(fp);

    serverLog(LL_NOTICE,"Node configuration loaded, I'm %.40s", myself->m_name);

    /* Something that should never happen: currentEpoch smaller than
     * the max epoch found in the nodes configuration. However we handle this
     * as some form of protection against manual editing of critical files. */
    if (clusterGetMaxEpoch() > server.cluster->m_currentEpoch) {
        server.cluster->m_currentEpoch = clusterGetMaxEpoch();
    }
    return C_OK;

fmterr:
    serverLog(LL_WARNING,
        "Unrecoverable error: corrupted cluster config file.");
    zfree(line);
    if (fp) fclose(fp);
    exit(1);
}

/* Cluster node configuration is exactly the same as CLUSTER NODES output.
 *
 * This function writes the node config and returns 0, on error -1
 * is returned.
 *
 * Note: we need to write the file in an atomic way from the point of view
 * of the POSIX filesystem semantics, so that if the server is stopped
 * or crashes during the write, we'll end with either the old file or the
 * new one. Since we have the full payload to write available we can use
 * a single write to write the whole file. If the pre-existing file was
 * bigger we pad our payload with newlines that are anyway ignored and truncate
 * the file afterward. */
int clusterSaveConfig(int do_fsync) {
    sds ci;
    size_t content_size;
    struct stat sb;
    int fd;

    server.cluster->m_todo_before_sleep &= ~CLUSTER_TODO_SAVE_CONFIG;

    /* Get the nodes description and concatenate our "vars" directive to
     * save currentEpoch and lastVoteEpoch. */
    ci = clusterGenNodesDescription(CLUSTER_NODE_HANDSHAKE);
    ci = sdscatprintf(ci,"vars currentEpoch %llu lastVoteEpoch %llu\n",
        (unsigned long long) server.cluster->m_currentEpoch,
        (unsigned long long) server.cluster->m_lastVoteEpoch);
    content_size = sdslen(ci);

    if ((fd = open(server.cluster_configfile,O_WRONLY|O_CREAT,0644))
        == -1) goto err;

    /* Pad the new payload if the existing file length is greater. */
    if (fstat(fd,&sb) != -1) {
        if (sb.st_size > (off_t)content_size) {
            ci = sdsgrowzero(ci,sb.st_size);
            memset(ci+content_size,'\n',sb.st_size-content_size);
        }
    }
    if (write(fd,ci,sdslen(ci)) != (ssize_t)sdslen(ci)) goto err;
    if (do_fsync) {
        server.cluster->m_todo_before_sleep &= ~CLUSTER_TODO_FSYNC_CONFIG;
        fsync(fd);
    }

    /* Truncate the file if needed to remove the final \n padding that
     * is just garbage. */
    if (content_size != sdslen(ci) && ftruncate(fd,content_size) == -1) {
        /* ftruncate() failing is not a critical error. */
    }
    close(fd);
    sdsfree(ci);
    return 0;

err:
    if (fd != -1) close(fd);
    sdsfree(ci);
    return -1;
}

void clusterSaveConfigOrDie(int do_fsync) {
    if (clusterSaveConfig(do_fsync) == -1) {
        serverLog(LL_WARNING,"Fatal: can't update cluster config file.");
        exit(1);
    }
}

/* Lock the cluster config using flock(), and leaks the file descriptor used to
 * acquire the lock so that the file will be locked forever.
 *
 * This works because we always update nodes.conf with a new version
 * in-place, reopening the file, and writing to it in place (later adjusting
 * the length with ftruncate()).
 *
 * On success C_OK is returned, otherwise an error is logged and
 * the function returns C_ERR to signal a lock was not acquired. */
int clusterLockConfig(char *filename) {
/* flock() does not exist on Solaris
 * and a fcntl-based solution won't help, as we constantly re-open that file,
 * which will release _all_ locks anyway
 */
#if !defined(__sun)
    /* To lock it, we need to open the file in a way it is created if
     * it does not exist, otherwise there is a race condition with other
     * processes. */
    int fd = open(filename,O_WRONLY|O_CREAT,0644);
    if (fd == -1) {
        serverLog(LL_WARNING,
            "Can't open %s in order to acquire a lock: %s",
            filename, strerror(errno));
        return C_ERR;
    }

    if (flock(fd,LOCK_EX|LOCK_NB) == -1) {
        if (errno == EWOULDBLOCK) {
            serverLog(LL_WARNING,
                 "Sorry, the cluster configuration file %s is already used "
                 "by a different Redis Cluster node. Please make sure that "
                 "different nodes use different cluster configuration "
                 "files.", filename);
        } else {
            serverLog(LL_WARNING,
                "Impossible to lock %s: %s", filename, strerror(errno));
        }
        close(fd);
        return C_ERR;
    }
    /* Lock acquired: leak the 'fd' by not closing it, so that we'll retain the
     * lock to the file as long as the process exists. */
#endif /* __sun */

    return C_OK;
}

void clusterInit() {
    int saveconf = 0;

    server.cluster = (clusterState *)zmalloc(sizeof(clusterState));
    server.cluster->m_myself = NULL;
    server.cluster->m_currentEpoch = 0;
    server.cluster->m_state = CLUSTER_FAIL;
    server.cluster->m_size = 1;
    server.cluster->m_todo_before_sleep = 0;
    server.cluster->m_nodes = dictCreate(&clusterNodesDictType,NULL);
    server.cluster->m_nodes_black_list =
        dictCreate(&clusterNodesBlackListDictType,NULL);
    server.cluster->m_failover_auth_time = 0;
    server.cluster->m_failover_auth_count = 0;
    server.cluster->m_failover_auth_rank = 0;
    server.cluster->m_failover_auth_epoch = 0;
    server.cluster->m_cant_failover_reason = CLUSTER_CANT_FAILOVER_NONE;
    server.cluster->m_lastVoteEpoch = 0;
    for (int i = 0; i < CLUSTERMSG_TYPE_COUNT; i++) {
        server.cluster->m_stats_bus_messages_sent[i] = 0;
        server.cluster->m_stats_bus_messages_received[i] = 0;
    }
    server.cluster->m_stats_pfail_nodes = 0;
    memset(server.cluster->m_slots,0, sizeof(server.cluster->m_slots));
    clusterCloseAllSlots();

    /* Lock the cluster config file to make sure every node uses
     * its own nodes.conf. */
    if (clusterLockConfig(server.cluster_configfile) == C_ERR)
        exit(1);

    /* Load or create a new nodes configuration. */
    if (clusterLoadConfig(server.cluster_configfile) == C_ERR) {
        /* No configuration found. We will just use the random name provided
         * by the createClusterNode() function. */
        myself = server.cluster->m_myself =
            createClusterNode(NULL,CLUSTER_NODE_MYSELF|CLUSTER_NODE_MASTER);
        serverLog(LL_NOTICE,"No cluster configuration found, I'm %.40s",
            myself->m_name);
        clusterAddNode(myself);
        saveconf = 1;
    }
    if (saveconf) clusterSaveConfigOrDie(1);

    /* We need a listening TCP port for our cluster messaging needs. */
    server.cfd_count = 0;

    /* Port sanity check II
     * The other handshake port check is triggered too late to stop
     * us from trying to use a too-high cluster port number. */
    if (server.port > (65535-CLUSTER_PORT_INCR)) {
        serverLog(LL_WARNING, "Redis port number too high. "
                   "Cluster communication port is 10,000 port "
                   "numbers higher than your Redis port. "
                   "Your Redis port number must be "
                   "lower than 55535.");
        exit(1);
    }

    if (listenToPort(server.port+CLUSTER_PORT_INCR,
        server.cfd,&server.cfd_count) == C_ERR)
    {
        exit(1);
    } else {
        int j;

        for (j = 0; j < server.cfd_count; j++) {
            if (server.el->aeCreateFileEvent(server.cfd[j], AE_READABLE,
                clusterAcceptHandler, NULL) == AE_ERR)
                    serverPanic("Unrecoverable error creating Redis Cluster "
                                "file event.");
        }
    }

    /* The slots -> keys map is a radix tree. Initialize it here. */
    server.cluster->m_slots_to_keys = raxNew();
    memset(server.cluster->m_slots_keys_count,0,
           sizeof(server.cluster->m_slots_keys_count));

    /* Set myself->m_port / cport to my listening ports, we'll just need to
     * discover the IP address via MEET messages. */
    myself->m_port = server.port;
    myself->m_cport = server.port+CLUSTER_PORT_INCR;
    if (server.cluster_announce_port)
        myself->m_port = server.cluster_announce_port;
    if (server.cluster_announce_bus_port)
        myself->m_cport = server.cluster_announce_bus_port;

    server.cluster->m_mf_end = 0;
    resetManualFailover();
}

/* Reset a node performing a soft or hard reset:
 *
 * 1) All other nodes are forget.
 * 2) All the assigned / open slots are released.
 * 3) If the node is a slave, it turns into a master.
 * 5) Only for hard reset: a new Node ID is generated.
 * 6) Only for hard reset: currentEpoch and configEpoch are set to 0.
 * 7) The new configuration is saved and the cluster state updated.
 * 8) If the node was a slave, the whole data set is flushed away. */
void clusterReset(int hard) {
    dictEntry *de;
    int j;

    /* Turn into master. */
    if (myself->nodeIsSlave()) {
        myself->clusterSetNodeAsMaster();
        replicationUnsetMaster();
        emptyDb(-1,EMPTYDB_NO_FLAGS,NULL);
    }

    /* Close slots, reset manual failover state. */
    clusterCloseAllSlots();
    resetManualFailover();

    /* Unassign all the slots. */
    for (j = 0; j < CLUSTER_SLOTS; j++) clusterDelSlot(j);

    /* Forget all the nodes, but myself. */
    {
        dictIterator di(server.cluster->m_nodes, 1);
        while((de = di.dictNext()) != NULL) {
            clusterNode* node = (clusterNode*)de->dictGetVal();

            if (node == myself) continue;
            clusterDelNode(node);
        }
    };

    /* Hard reset only: set epochs to 0, change node ID. */
    if (hard) {
        sds oldname;

        server.cluster->m_currentEpoch = 0;
        server.cluster->m_lastVoteEpoch = 0;
        myself->m_configEpoch = 0;
        serverLog(LL_WARNING, "configEpoch set to 0 via CLUSTER RESET HARD");

        /* To change the Node ID we need to remove the old name from the
         * nodes table, change the ID, and re-add back with new name. */
        oldname = sdsnewlen(myself->m_name, CLUSTER_NAMELEN);
        server.cluster->m_nodes->dictDelete(oldname);
        sdsfree(oldname);
        getRandomHexChars(myself->m_name, CLUSTER_NAMELEN);
        clusterAddNode(myself);
        serverLog(LL_NOTICE,"Node hard reset, now I'm %.40s", myself->m_name);
    }

    /* Make sure to persist the new config and update the state. */
    clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG|
                         CLUSTER_TODO_UPDATE_STATE|
                         CLUSTER_TODO_FSYNC_CONFIG);
}

/* -----------------------------------------------------------------------------
 * CLUSTER communication link
 * -------------------------------------------------------------------------- */

clusterLink *createClusterLink(clusterNode *node, int in_fd)
{
    void* cluster_link_mem = zmalloc(sizeof(clusterLink));
    clusterLink* link = new (cluster_link_mem) clusterLink(node, in_fd);

    return link;
}

clusterLink::clusterLink(clusterNode *in_node, int in_fd)
: m_ctime(mstime())
, m_sndbuf(sdsempty())
, m_rcvbuf(sdsempty())
, m_node(in_node)
, m_fd(in_fd)
{

}

/* Free a cluster link, but does not free the associated node of course.
 * This function will just make sure that the original node associated
 * with this link will have the 'link' field set to NULL. */
void freeClusterLink(clusterLink*& link)
{
    if (NULL != link)
    {
        link->~clusterLink();
        zfree(link);
        link = NULL;
    }
}

clusterLink::~clusterLink()
{
    if (m_fd != -1) {
        server.el->aeDeleteFileEvent(m_fd, AE_WRITABLE);
        server.el->aeDeleteFileEvent(m_fd, AE_READABLE);
    }
    sdsfree(m_sndbuf);
    sdsfree(m_rcvbuf);
    if (m_node)
        m_node->m_link = NULL;
    close(m_fd);

}

#define MAX_CLUSTER_ACCEPTS_PER_CALL 1000
void clusterAcceptHandler(aeEventLoop *el, int fd, void *privdata, int mask) {
    int cport, cfd;
    int max = MAX_CLUSTER_ACCEPTS_PER_CALL;
    char cip[NET_IP_STR_LEN];
    clusterLink *link;
    UNUSED(el);
    UNUSED(mask);
    UNUSED(privdata);

    /* If the server is starting up, don't accept cluster connections:
     * UPDATE messages may interact with the database content. */
    if (server.masterhost == NULL && server.loading) return;

    while(max--) {
        cfd = anetTcpAccept(server.neterr, fd, cip, sizeof(cip), &cport);
        if (cfd == ANET_ERR) {
            if (errno != EWOULDBLOCK)
                serverLog(LL_VERBOSE,
                    "Error accepting cluster node: %s", server.neterr);
            return;
        }
        anetNonBlock(NULL,cfd);
        anetEnableTcpNoDelay(NULL,cfd);

        /* Use non-blocking I/O for cluster messages. */
        serverLog(LL_VERBOSE,"Accepted cluster node %s:%d", cip, cport);
        /* Create a link object we use to handle the connection.
         * It gets passed to the readable handler when data is available.
         * Initiallly the link->m_node pointer is set to NULL as we don't know
         * which node is, but the right node is references once we know the
         * node identity. */
        link = createClusterLink(NULL, cfd);
        server.el->aeCreateFileEvent(cfd,AE_READABLE,clusterReadHandler,link);
    }
}

/* -----------------------------------------------------------------------------
 * Key space handling
 * -------------------------------------------------------------------------- */

/* We have 16384 hash slots. The hash slot of a given key is obtained
 * as the least significant 14 bits of the crc16 of the key.
 *
 * However if the key contains the {...} pattern, only the part between
 * { and } is hashed. This may be useful in the future to force certain
 * keys to be in the same node (assuming no resharding is in progress). */
unsigned int keyHashSlot(char *key, int keylen) {
    int s, e; /* start-end indexes of { and } */

    for (s = 0; s < keylen; s++)
        if (key[s] == '{') break;

    /* No '{' ? Hash the whole key. This is the base case. */
    if (s == keylen) return crc16(key,keylen) & 0x3FFF;

    /* '{' found? Check if we have the corresponding '}'. */
    for (e = s+1; e < keylen; e++)
        if (key[e] == '}') break;

    /* No '}' or nothing betweeen {} ? Hash the whole key. */
    if (e == keylen || e == s+1) return crc16(key,keylen) & 0x3FFF;

    /* If we are here there is both a { and a } on its right. Hash
     * what is in the middle between { and }. */
    return crc16(key+s+1,e-s-1) & 0x3FFF;
}

/* -----------------------------------------------------------------------------
 * CLUSTER node API
 * -------------------------------------------------------------------------- */

/* Create a new cluster node, with the specified flags.
 * If "nodename" is NULL this is considered a first handshake and a random
 * node name is assigned to this node (it will be fixed later when we'll
 * receive the first pong).
 *
 * The node is created and returned to the user, but it is not automatically
 * added to the nodes hash table. */
clusterNode *createClusterNode(char *nodename, int flags) {
    void* cluster_node_mem = zmalloc(sizeof(clusterNode));

    clusterNode* node = new (cluster_node_mem) clusterNode(nodename, flags);
    return node;
}

clusterNode::clusterNode(char *nodename, int flags)
{
    if (nodename)
        memcpy(m_name, nodename, CLUSTER_NAMELEN);
    else
        getRandomHexChars(m_name, CLUSTER_NAMELEN);
    m_ctime = mstime();
    m_configEpoch = 0;
    m_flags = flags;
    memset(m_slots,0,sizeof(m_slots));
    m_numslots = 0;
    m_numslaves = 0;
    m_slaves = NULL;
    m_slaveof = NULL;
    m_ping_sent = m_pong_received = 0;
    m_fail_time = 0;
    m_link = NULL;
    memset(m_ip,0,sizeof(m_ip));
    m_port = 0;
    m_cport = 0;
    m_fail_reports = listCreate();
    m_voted_time = 0;
    m_orphaned_time = 0;
    m_repl_offset_time = 0;
    m_repl_offset = 0;
    m_fail_reports->listSetFreeMethod(zfree);
}

/* This function is called every time we get a failure report from a node.
 * The side effect is to populate the fail_reports list (or to update
 * the timestamp of an existing report).
 *
 * 'failing' is the node that is in failure state according to the
 * 'sender' node.
 *
 * The function returns 0 if it just updates a timestamp of an existing
 * failure report from the same sender. 1 is returned if a new failure
 * report is created. */
int clusterNodeAddFailureReport(clusterNode *failing, clusterNode *sender) {
    list *l = failing->m_fail_reports;
    listNode *ln;
    clusterNodeFailReport *fr;

    /* If a failure report from the same sender already exists, just update
     * the timestamp. */
    listIter li(l);
    while ((ln = li.listNext()) != NULL) {
        fr = (clusterNodeFailReport *)ln->listNodeValue();
        if (fr->node == sender) {
            fr->time = mstime();
            return 0;
        }
    }

    /* Otherwise create a new report. */
    fr = (clusterNodeFailReport *)zmalloc(sizeof(*fr));
    fr->node = sender;
    fr->time = mstime();
    l->listAddNodeTail(fr);
    return 1;
}

/* Remove failure reports that are too old, where too old means reasonably
 * older than the global node timeout. Note that anyway for a node to be
 * flagged as FAIL we need to have a local PFAIL state that is at least
 * older than the global node timeout, so we don't just trust the number
 * of failure reports from other nodes. */
void clusterNodeCleanupFailureReports(clusterNode *node) {
    list *l = node->m_fail_reports;
    listNode *ln;
    clusterNodeFailReport *fr;
    mstime_t maxtime = server.cluster_node_timeout *
                     CLUSTER_FAIL_REPORT_VALIDITY_MULT;
    mstime_t now = mstime();

    listIter li(l);
    while ((ln = li.listNext()) != NULL) {
        fr = (clusterNodeFailReport *)ln->listNodeValue();
        if (now - fr->time > maxtime) l->listDelNode(ln);
    }
}

/* Remove the failing report for 'node' if it was previously considered
 * failing by 'sender'. This function is called when a node informs us via
 * gossip that a node is OK from its point of view (no FAIL or PFAIL flags).
 *
 * Note that this function is called relatively often as it gets called even
 * when there are no nodes failing, and is O(N), however when the cluster is
 * fine the failure reports list is empty so the function runs in constant
 * time.
 *
 * The function returns 1 if the failure report was found and removed.
 * Otherwise 0 is returned. */
int clusterNodeDelFailureReport(clusterNode *node, clusterNode *sender) {
    list *l = node->m_fail_reports;
    listNode *ln;
    clusterNodeFailReport *fr;

    /* Search for a failure report from this sender. */
    listIter li(l);
    while ((ln = li.listNext()) != NULL) {
        fr = (clusterNodeFailReport *)ln->listNodeValue();
        if (fr->node == sender) break;
    }
    if (!ln) return 0; /* No failure report from this sender. */

    /* Remove the failure report. */
    l->listDelNode(ln);
    clusterNodeCleanupFailureReports(node);
    return 1;
}

/* Return the number of external nodes that believe 'node' is failing,
 * not including this node, that may have a PFAIL or FAIL state for this
 * node as well. */
int clusterNodeFailureReportsCount(clusterNode *node) {
    clusterNodeCleanupFailureReports(node);
    return node->m_fail_reports->listLength();
}

int clusterNodeRemoveSlave(clusterNode *master, clusterNode *slave) {
    int j;

    for (j = 0; j < master->m_numslaves; j++) {
        if (master->m_slaves[j] == slave) {
            if ((j+1) < master->m_numslaves) {
                int remaining_slaves = (master->m_numslaves - j) - 1;
                memmove(master->m_slaves+j,master->m_slaves+(j+1),
                        (sizeof(*master->m_slaves) * remaining_slaves));
            }
            master->m_numslaves--;
            if (master->m_numslaves == 0)
                master->m_flags &= ~CLUSTER_NODE_MIGRATE_TO;
            return C_OK;
        }
    }
    return C_ERR;
}

int clusterNode::clusterNodeAddSlave(clusterNode *slave)
{
    int j;

    /* If it's already a slave, don't add it again. */
    for (j = 0; j < m_numslaves; j++)
        if (m_slaves[j] == slave) return C_ERR;
    m_slaves = (clusterNode **)zrealloc(m_slaves,
        sizeof(clusterNode*)*(m_numslaves+1));
    m_slaves[m_numslaves] = slave;
    m_numslaves++;
    m_flags |= CLUSTER_NODE_MIGRATE_TO;
    return C_OK;
}

int clusterCountNonFailingSlaves(clusterNode *n) {
    int j, okslaves = 0;

    for (j = 0; j < n->m_numslaves; j++)
        if (!n->m_slaves[j]->nodeFailed()) okslaves++;
    return okslaves;
}

/* Low level cleanup of the node structure. Only called by clusterDelNode(). */
void freeClusterNode(clusterNode *n) {
    sds nodename;
    int j;

    /* If the node has associated slaves, we have to set
     * all the slaves->m_slaveof fields to NULL (unknown). */
    for (j = 0; j < n->m_numslaves; j++)
        n->m_slaves[j]->m_slaveof = NULL;

    /* Remove this node from the list of slaves of its master. */
    if (n->nodeIsSlave() && n->m_slaveof) clusterNodeRemoveSlave(n->m_slaveof,n);

    /* Unlink from the set of nodes. */
    nodename = sdsnewlen(n->m_name, CLUSTER_NAMELEN);
    serverAssert(server.cluster->m_nodes->dictDelete(nodename) == DICT_OK);
    sdsfree(nodename);

    /* Release link and associated data structures. */
    freeClusterLink(n->m_link);
    listRelease(n->m_fail_reports);
    zfree(n->m_slaves);
    zfree(n);
}

/* Add a node to the nodes hash table */
int clusterAddNode(clusterNode *node) {
    int retval;

    retval = server.cluster->m_nodes->dictAdd(sdsnewlen(node->m_name,CLUSTER_NAMELEN), node);
    return (retval == DICT_OK) ? C_OK : C_ERR;
}

/* Remove a node from the cluster. The functio performs the high level
 * cleanup, calling freeClusterNode() for the low level cleanup.
 * Here we do the following:
 *
 * 1) Mark all the slots handled by it as unassigned.
 * 2) Remove all the failure reports sent by this node and referenced by
 *    other nodes.
 * 3) Free the node with freeClusterNode() that will in turn remove it
 *    from the hash table and from the list of slaves of its master, if
 *    it is a slave node.
 */
void clusterDelNode(clusterNode *delnode) {
    int j;
    dictEntry *de;

    /* 1) Mark slots as unassigned. */
    for (j = 0; j < CLUSTER_SLOTS; j++) {
        if (server.cluster->m_importing_slots_from[j] == delnode)
            server.cluster->m_importing_slots_from[j] = NULL;
        if (server.cluster->m_migrating_slots_to[j] == delnode)
            server.cluster->m_migrating_slots_to[j] = NULL;
        if (server.cluster->m_slots[j] == delnode)
            clusterDelSlot(j);
    }
    
    /* 2) Remove failure reports. */
    {
        dictIterator di(server.cluster->m_nodes, 1);
        while((de = di.dictNext()) != NULL) {
            clusterNode* node = (clusterNode*)de->dictGetVal();

            if (node == delnode) continue;
            clusterNodeDelFailureReport(node,delnode);
            }
    }

    /* 3) Free the node, unlinking it from the cluster. */
    freeClusterNode(delnode);
}

/* Node lookup by name */
clusterNode *clusterLookupNode(const char *name) {
    sds s = sdsnewlen(name, CLUSTER_NAMELEN);
    dictEntry *de;

    de = server.cluster->m_nodes->dictFind(s);
    sdsfree(s);
    if (de == NULL) return NULL;
    return (clusterNode *)de->dictGetVal();
}

/* This is only used after the handshake. When we connect a given IP/PORT
 * as a result of CLUSTER MEET we don't have the node name yet, so we
 * pick a random one, and will fix it when we receive the PONG request using
 * this function. */
void clusterRenameNode(clusterNode *node, char *newname) {
    int retval;
    sds s = sdsnewlen(node->m_name, CLUSTER_NAMELEN);

    serverLog(LL_DEBUG,"Renaming node %.40s into %.40s",
        node->m_name, newname);
    retval = server.cluster->m_nodes->dictDelete(s);
    sdsfree(s);
    serverAssert(retval == DICT_OK);
    memcpy(node->m_name, newname, CLUSTER_NAMELEN);
    clusterAddNode(node);
}

/* -----------------------------------------------------------------------------
 * CLUSTER config epoch handling
 * -------------------------------------------------------------------------- */

/* Return the greatest configEpoch found in the cluster, or the current
 * epoch if greater than any node configEpoch. */
uint64_t clusterGetMaxEpoch() {
    uint64_t max = 0;
    dictEntry *de;

    dictIterator di(server.cluster->m_nodes, 1);
    while((de = di.dictNext()) != NULL) {
        clusterNode* node = (clusterNode*)de->dictGetVal();
        if (node->m_configEpoch > max) max = node->m_configEpoch;
    }

    if (max < server.cluster->m_currentEpoch) max = server.cluster->m_currentEpoch;
    return max;
}

/* If this node epoch is zero or is not already the greatest across the
 * cluster (from the POV of the local configuration), this function will:
 *
 * 1) Generate a new config epoch, incrementing the current epoch.
 * 2) Assign the new epoch to this node, WITHOUT any consensus.
 * 3) Persist the configuration on disk before sending packets with the
 *    new configuration.
 *
 * If the new config epoch is generated and assigend, C_OK is returned,
 * otherwise C_ERR is returned (since the node has already the greatest
 * configuration around) and no operation is performed.
 *
 * Important note: this function violates the principle that config epochs
 * should be generated with consensus and should be unique across the cluster.
 * However Redis Cluster uses this auto-generated new config epochs in two
 * cases:
 *
 * 1) When slots are closed after importing. Otherwise resharding would be
 *    too expensive.
 * 2) When CLUSTER FAILOVER is called with options that force a slave to
 *    failover its master even if there is not master majority able to
 *    create a new configuration epoch.
 *
 * Redis Cluster will not explode using this function, even in the case of
 * a collision between this node and another node, generating the same
 * configuration epoch unilaterally, because the config epoch conflict
 * resolution algorithm will eventually move colliding nodes to different
 * config epochs. However using this function may violate the "last failover
 * wins" rule, so should only be used with care. */
int clusterBumpConfigEpochWithoutConsensus() {
    uint64_t maxEpoch = clusterGetMaxEpoch();

    if (myself->m_configEpoch == 0 ||
        myself->m_configEpoch != maxEpoch)
    {
        server.cluster->m_currentEpoch++;
        myself->m_configEpoch = server.cluster->m_currentEpoch;
        clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG|
                             CLUSTER_TODO_FSYNC_CONFIG);
        serverLog(LL_WARNING,
            "New configEpoch set to %llu",
            (unsigned long long) myself->m_configEpoch);
        return C_OK;
    } else {
        return C_ERR;
    }
}

/* This function is called when this node is a master, and we receive from
 * another master a configuration epoch that is equal to our configuration
 * epoch.
 *
 * BACKGROUND
 *
 * It is not possible that different slaves get the same config
 * epoch during a failover election, because the slaves need to get voted
 * by a majority. However when we perform a manual resharding of the cluster
 * the node will assign a configuration epoch to itself without to ask
 * for agreement. Usually resharding happens when the cluster is working well
 * and is supervised by the sysadmin, however it is possible for a failover
 * to happen exactly while the node we are resharding a slot to assigns itself
 * a new configuration epoch, but before it is able to propagate it.
 *
 * So technically it is possible in this condition that two nodes end with
 * the same configuration epoch.
 *
 * Another possibility is that there are bugs in the implementation causing
 * this to happen.
 *
 * Moreover when a new cluster is created, all the nodes start with the same
 * configEpoch. This collision resolution code allows nodes to automatically
 * end with a different configEpoch at startup automatically.
 *
 * In all the cases, we want a mechanism that resolves this issue automatically
 * as a safeguard. The same configuration epoch for masters serving different
 * set of slots is not harmful, but it is if the nodes end serving the same
 * slots for some reason (manual errors or software bugs) without a proper
 * failover procedure.
 *
 * In general we want a system that eventually always ends with different
 * masters having different configuration epochs whatever happened, since
 * nothign is worse than a split-brain condition in a distributed system.
 *
 * BEHAVIOR
 *
 * When this function gets called, what happens is that if this node
 * has the lexicographically smaller Node ID compared to the other node
 * with the conflicting epoch (the 'sender' node), it will assign itself
 * the greatest configuration epoch currently detected among nodes plus 1.
 *
 * This means that even if there are multiple nodes colliding, the node
 * with the greatest Node ID never moves forward, so eventually all the nodes
 * end with a different configuration epoch.
 */
void clusterHandleConfigEpochCollision(clusterNode *sender) {
    /* Prerequisites: nodes have the same configEpoch and are both masters. */
    if (sender->m_configEpoch != myself->m_configEpoch ||
        !sender->nodeIsMaster() || !myself->nodeIsMaster()) return;
    /* Don't act if the colliding node has a smaller Node ID. */
    if (memcmp(sender->m_name,myself->m_name,CLUSTER_NAMELEN) <= 0) return;
    /* Get the next ID available at the best of this node knowledge. */
    server.cluster->m_currentEpoch++;
    myself->m_configEpoch = server.cluster->m_currentEpoch;
    clusterSaveConfigOrDie(1);
    serverLog(LL_VERBOSE,
        "WARNING: configEpoch collision with node %.40s."
        " configEpoch set to %llu",
        sender->m_name,
        (unsigned long long) myself->m_configEpoch);
}

/* -----------------------------------------------------------------------------
 * CLUSTER nodes blacklist
 *
 * The nodes blacklist is just a way to ensure that a given node with a given
 * Node ID is not readded before some time elapsed (this time is specified
 * in seconds in CLUSTER_BLACKLIST_TTL).
 *
 * This is useful when we want to remove a node from the cluster completely:
 * when CLUSTER FORGET is called, it also puts the node into the blacklist so
 * that even if we receive gossip messages from other nodes that still remember
 * about the node we want to remove, we don't re-add it before some time.
 *
 * Currently the CLUSTER_BLACKLIST_TTL is set to 1 minute, this means
 * that redis-trib has 60 seconds to send CLUSTER FORGET messages to nodes
 * in the cluster without dealing with the problem of other nodes re-adding
 * back the node to nodes we already sent the FORGET command to.
 *
 * The data structure used is a hash table with an sds string representing
 * the node ID as key, and the time when it is ok to re-add the node as
 * value.
 * -------------------------------------------------------------------------- */

#define CLUSTER_BLACKLIST_TTL 60      /* 1 minute. */


/* Before of the addNode() or Exists() operations we always remove expired
 * entries from the black list. This is an O(N) operation but it is not a
 * problem since add / exists operations are called very infrequently and
 * the hash table is supposed to contain very little elements at max.
 * However without the cleanup during long uptimes and with some automated
 * node add/removal procedures, entries could accumulate. */
void clusterBlacklistCleanup(void) {
    dictEntry *de;
    dictIterator di(server.cluster->m_nodes_black_list, 1);
    while((de = di.dictNext()) != NULL) {
        int64_t expire = de->dictGetUnsignedIntegerVal();

        if (expire < server.unixtime)
            server.cluster->m_nodes_black_list->dictDelete(de->dictGetKey());
    }
}

/* Cleanup the blacklist and add a new node ID to the black list. */
void clusterBlacklistAddNode(clusterNode *node) {
    dictEntry *de;
    sds id = sdsnewlen(node->m_name,CLUSTER_NAMELEN);

    clusterBlacklistCleanup();
    if (server.cluster->m_nodes_black_list->dictAdd(id,NULL) == DICT_OK) {
        /* If the key was added, duplicate the sds string representation of
         * the key for the next lookup. We'll free it at the end. */
        id = sdsdup(id);
    }
    de = server.cluster->m_nodes_black_list->dictFind(id);
    de->dictSetUnsignedIntegerVal(time(NULL)+CLUSTER_BLACKLIST_TTL);
    sdsfree(id);
}

/* Return non-zero if the specified node ID exists in the blacklist.
 * You don't need to pass an sds string here, any pointer to 40 bytes
 * will work. */
int clusterBlacklistExists(char *nodeid) {
    sds id = sdsnewlen(nodeid,CLUSTER_NAMELEN);
    int retval;

    clusterBlacklistCleanup();
    retval = server.cluster->m_nodes_black_list->dictFind(id) != NULL;
    sdsfree(id);
    return retval;
}

/* -----------------------------------------------------------------------------
 * CLUSTER messages exchange - PING/PONG and gossip
 * -------------------------------------------------------------------------- */

/* This function checks if a given node should be marked as FAIL.
 * It happens if the following conditions are met:
 *
 * 1) We received enough failure reports from other master nodes via gossip.
 *    Enough means that the majority of the masters signaled the node is
 *    down recently.
 * 2) We believe this node is in PFAIL state.
 *
 * If a failure is detected we also inform the whole cluster about this
 * event trying to force every other node to set the FAIL flag for the node.
 *
 * Note that the form of agreement used here is weak, as we collect the majority
 * of masters state during some time, and even if we force agreement by
 * propagating the FAIL message, because of partitions we may not reach every
 * node. However:
 *
 * 1) Either we reach the majority and eventually the FAIL state will propagate
 *    to all the cluster.
 * 2) Or there is no majority so no slave promotion will be authorized and the
 *    FAIL flag will be cleared after some time.
 */
void markNodeAsFailingIfNeeded(clusterNode *node) {
    int failures;
    int needed_quorum = (server.cluster->m_size / 2) + 1;

    if (!node->nodeTimedOut()) return; /* We can reach it. */
    if (node->nodeFailed()) return; /* Already FAILing. */

    failures = clusterNodeFailureReportsCount(node);
    /* Also count myself as a voter if I'm a master. */
    if (myself->nodeIsMaster()) failures++;
    if (failures < needed_quorum) return; /* No weak agreement from masters. */

    serverLog(LL_NOTICE,
        "Marking node %.40s as failing (quorum reached).", node->m_name);

    /* Mark the node as failing. */
    node->m_flags &= ~CLUSTER_NODE_PFAIL;
    node->m_flags |= CLUSTER_NODE_FAIL;
    node->m_fail_time = mstime();

    /* Broadcast the failing node name to everybody, forcing all the other
     * reachable nodes to flag the node as FAIL. */
    if (myself->nodeIsMaster()) clusterSendFail(node->m_name);
    clusterDoBeforeSleep(CLUSTER_TODO_UPDATE_STATE|CLUSTER_TODO_SAVE_CONFIG);
}

/* This function is called only if a node is marked as FAIL, but we are able
 * to reach it again. It checks if there are the conditions to undo the FAIL
 * state. */
void clearNodeFailureIfNeeded(clusterNode *node) {
    mstime_t now = mstime();

    serverAssert(node->nodeFailed());

    /* For slaves we always clear the FAIL flag if we can contact the
     * node again. */
    if (node->nodeIsSlave() || node->m_numslots == 0) {
        serverLog(LL_NOTICE,
            "Clear FAIL state for node %.40s: %s is reachable again.",
                node->m_name,
                node->nodeIsSlave() ? "slave" : "master without slots");
        node->m_flags &= ~CLUSTER_NODE_FAIL;
        clusterDoBeforeSleep(CLUSTER_TODO_UPDATE_STATE|CLUSTER_TODO_SAVE_CONFIG);
    }

    /* If it is a master and...
     * 1) The FAIL state is old enough.
     * 2) It is yet serving slots from our point of view (not failed over).
     * Apparently no one is going to fix these slots, clear the FAIL flag. */
    if (node->nodeIsMaster() && node->m_numslots > 0 &&
        (now - node->m_fail_time) >
        (server.cluster_node_timeout * CLUSTER_FAIL_UNDO_TIME_MULT))
    {
        serverLog(LL_NOTICE,
            "Clear FAIL state for node %.40s: is reachable again and nobody is serving its slots after some time.",
                node->m_name);
        node->m_flags &= ~CLUSTER_NODE_FAIL;
        clusterDoBeforeSleep(CLUSTER_TODO_UPDATE_STATE|CLUSTER_TODO_SAVE_CONFIG);
    }
}

/* Return true if we already have a node in HANDSHAKE state matching the
 * specified ip address and port number. This function is used in order to
 * avoid adding a new handshake node for the same address multiple times. */
int clusterHandshakeInProgress(char *ip, int port, int cport) {
    dictEntry *de;
    dictIterator di(server.cluster->m_nodes, 1);
    while((de = di.dictNext()) != NULL) {
        clusterNode* node = (clusterNode*)de->dictGetVal();

        if (!node->nodeInHandshake()) continue;
        if (!strcasecmp(node->m_ip,ip) &&
            node->m_port == port &&
            node->m_cport == cport) break;
    }

    return de != NULL;
}

/* Start an handshake with the specified address if there is not one
 * already in progress. Returns non-zero if the handshake was actually
 * started. On error zero is returned and errno is set to one of the
 * following values:
 *
 * EAGAIN - There is already an handshake in progress for this address.
 * EINVAL - IP or port are not valid. */
int clusterStartHandshake(char *ip, int port, int cport) {
    clusterNode *n;
    char norm_ip[NET_IP_STR_LEN];
    struct sockaddr_storage sa;

    /* IP sanity check */
    if (inet_pton(AF_INET,ip,
            &(((struct sockaddr_in *)&sa)->sin_addr)))
    {
        sa.ss_family = AF_INET;
    } else if (inet_pton(AF_INET6,ip,
            &(((struct sockaddr_in6 *)&sa)->sin6_addr)))
    {
        sa.ss_family = AF_INET6;
    } else {
        errno = EINVAL;
        return 0;
    }

    /* Port sanity check */
    if (port <= 0 || port > 65535 || cport <= 0 || cport > 65535) {
        errno = EINVAL;
        return 0;
    }

    /* Set norm_ip as the normalized string representation of the node
     * IP address. */
    memset(norm_ip,0,NET_IP_STR_LEN);
    if (sa.ss_family == AF_INET)
        inet_ntop(AF_INET,
            (void*)&(((struct sockaddr_in *)&sa)->sin_addr),
            norm_ip,NET_IP_STR_LEN);
    else
        inet_ntop(AF_INET6,
            (void*)&(((struct sockaddr_in6 *)&sa)->sin6_addr),
            norm_ip,NET_IP_STR_LEN);

    if (clusterHandshakeInProgress(norm_ip,port,cport)) {
        errno = EAGAIN;
        return 0;
    }

    /* Add the node with a random address (NULL as first argument to
     * createClusterNode()). Everything will be fixed during the
     * handshake. */
    n = createClusterNode(NULL,CLUSTER_NODE_HANDSHAKE|CLUSTER_NODE_MEET);
    memcpy(n->m_ip,norm_ip,sizeof(n->m_ip));
    n->m_port = port;
    n->m_cport = cport;
    clusterAddNode(n);
    return 1;
}

/* Process the gossip section of PING or PONG packets.
 * Note that this function assumes that the packet is already sanity-checked
 * by the caller, not in the content of the gossip section, but in the
 * length. */
void clusterProcessGossipSection(clusterMsg *hdr, clusterLink *link) {
    uint16_t count = ntohs(hdr->m_count);
    clusterMsgDataGossip *g = (clusterMsgDataGossip*) hdr->m_data.ping.gossip;
    clusterNode *sender = link->m_node ? link->m_node : clusterLookupNode(hdr->m_sender);

    while(count--) {
        uint16_t flags = ntohs(g->m_flags);
        clusterNode *node;
        sds ci;

        if (server.verbosity == LL_DEBUG) {
            ci = representClusterNodeFlags(sdsempty(), flags);
            serverLog(LL_DEBUG,"GOSSIP %.40s %s:%d@%d %s",
                g->m_nodename,
                g->m_ip,
                ntohs(g->m_port),
                ntohs(g->m_cport),
                ci);
            sdsfree(ci);
        }

        /* Update our state accordingly to the gossip sections */
        node = clusterLookupNode(g->m_nodename);
        if (node) {
            /* We already know this node.
               Handle failure reports, only when the sender is a master. */
            if (sender && sender->nodeIsMaster() && node != myself) {
                if (flags & (CLUSTER_NODE_FAIL|CLUSTER_NODE_PFAIL)) {
                    if (clusterNodeAddFailureReport(node,sender)) {
                        serverLog(LL_VERBOSE,
                            "Node %.40s reported node %.40s as not reachable.",
                            sender->m_name, node->m_name);
                    }
                    markNodeAsFailingIfNeeded(node);
                } else {
                    if (clusterNodeDelFailureReport(node,sender)) {
                        serverLog(LL_VERBOSE,
                            "Node %.40s reported node %.40s is back online.",
                            sender->m_name, node->m_name);
                    }
                }
            }

            /* If from our POV the node is up (no failure flags are set),
             * we have no pending ping for the node, nor we have failure
             * reports for this node, update the last pong time with the
             * one we see from the other nodes. */
            if (!(flags & (CLUSTER_NODE_FAIL|CLUSTER_NODE_PFAIL)) &&
                node->m_ping_sent == 0 &&
                clusterNodeFailureReportsCount(node) == 0)
            {
                mstime_t pongtime = ntohl(g->m_pong_received);
                pongtime *= 1000; /* Convert back to milliseconds. */

                /* Replace the pong time with the received one only if
                 * it's greater than our view but is not in the future
                 * (with 500 milliseconds tolerance) from the POV of our
                 * clock. */
                if (pongtime <= (server.mstime+500) &&
                    pongtime > node->m_pong_received)
                {
                    node->m_pong_received = pongtime;
                }
            }

            /* If we already know this node, but it is not reachable, and
             * we see a different address in the gossip section of a node that
             * can talk with this other node, update the address, disconnect
             * the old link if any, so that we'll attempt to connect with the
             * new address. */
            if (node->m_flags & (CLUSTER_NODE_FAIL|CLUSTER_NODE_PFAIL) &&
                !(flags & CLUSTER_NODE_NOADDR) &&
                !(flags & (CLUSTER_NODE_FAIL|CLUSTER_NODE_PFAIL)) &&
                (strcasecmp(node->m_ip,g->m_ip) ||
                 node->m_port != ntohs(g->m_port) ||
                 node->m_cport != ntohs(g->m_cport)))
            {
                freeClusterLink(node->m_link);
                memcpy(node->m_ip,g->m_ip,NET_IP_STR_LEN);
                node->m_port = ntohs(g->m_port);
                node->m_cport = ntohs(g->m_cport);
                node->m_flags &= ~CLUSTER_NODE_NOADDR;
            }
        } else {
            /* If it's not in NOADDR state and we don't have it, we
             * start a handshake process against this IP/PORT pairs.
             *
             * Note that we require that the sender of this gossip message
             * is a well known node in our cluster, otherwise we risk
             * joining another cluster. */
            if (sender &&
                !(flags & CLUSTER_NODE_NOADDR) &&
                !clusterBlacklistExists(g->m_nodename))
            {
                clusterStartHandshake(g->m_ip,ntohs(g->m_port),ntohs(g->m_cport));
            }
        }

        /* Next node */
        g++;
    }
}

/* IP -> string conversion. 'buf' is supposed to at least be 46 bytes.
 * If 'announced_ip' length is non-zero, it is used instead of extracting
 * the IP from the socket peer address. */
void nodeIp2String(char *buf, clusterLink *link, char *announced_ip) {
    if (announced_ip[0] != '\0') {
        memcpy(buf,announced_ip,NET_IP_STR_LEN);
        buf[NET_IP_STR_LEN-1] = '\0'; /* We are not sure the input is sane. */
    } else {
        anetPeerToString(link->m_fd, buf, NET_IP_STR_LEN, NULL);
    }
}

/* Update the node address to the IP address that can be extracted
 * from link->m_fd, or if hdr->m_myip is non empty, to the address the node
 * is announcing us. The port is taken from the packet header as well.
 *
 * If the address or port changed, disconnect the node link so that we'll
 * connect again to the new address.
 *
 * If the ip/port pair are already correct no operation is performed at
 * all.
 *
 * The function returns 0 if the node address is still the same,
 * otherwise 1 is returned. */
int nodeUpdateAddressIfNeeded(clusterNode *node, clusterLink *link,
                              clusterMsg *hdr)
{
    char ip[NET_IP_STR_LEN] = {0};
    int port = ntohs(hdr->m_port);
    int cport = ntohs(hdr->m_cport);

    /* We don't proceed if the link is the same as the sender link, as this
     * function is designed to see if the node link is consistent with the
     * symmetric link that is used to receive PINGs from the node.
     *
     * As a side effect this function never frees the passed 'link', so
     * it is safe to call during packet processing. */
    if (link == node->m_link) return 0;

    nodeIp2String(ip,link,hdr->m_myip);
    if (node->m_port == port && node->m_cport == cport &&
        strcmp(ip,node->m_ip) == 0) return 0;

    /* IP / port is different, update it. */
    memcpy(node->m_ip,ip,sizeof(ip));
    node->m_port = port;
    node->m_cport = cport;
    freeClusterLink(node->m_link);
    node->m_flags &= ~CLUSTER_NODE_NOADDR;
    serverLog(LL_WARNING,"Address updated for node %.40s, now %s:%d",
        node->m_name, node->m_ip, node->m_port);

    /* Check if this is our master and we have to change the
     * replication target as well. */
    if (myself->nodeIsSlave() && myself->m_slaveof == node)
        replicationSetMaster(node->m_ip, node->m_port);
    return 1;
}

/* Reconfigure the specified node 'n' as a master. This function is called when
 * a node that we believed to be a slave is now acting as master in order to
 * update the state of the node. */
void clusterNode::clusterSetNodeAsMaster()
{
    if (nodeIsMaster()) return;

    if (m_slaveof) {
        clusterNodeRemoveSlave(m_slaveof,this);
        if (this != myself) m_flags |= CLUSTER_NODE_MIGRATE_TO;
    }
    m_flags &= ~CLUSTER_NODE_SLAVE;
    m_flags |= CLUSTER_NODE_MASTER;
    m_slaveof = NULL;

    /* Update config and state. */
    clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG|
                         CLUSTER_TODO_UPDATE_STATE);
}

/* This function is called when we receive a master configuration via a
 * PING, PONG or UPDATE packet. What we receive is a node, a configEpoch of the
 * node, and the set of slots claimed under this configEpoch.
 *
 * What we do is to rebind the slots with newer configuration compared to our
 * local configuration, and if needed, we turn ourself into a replica of the
 * node (see the function comments for more info).
 *
 * The 'sender' is the node for which we received a configuration update.
 * Sometimes it is not actually the "Sender" of the information, like in the
 * case we receive the info via an UPDATE packet. */
void clusterUpdateSlotsConfigWith(clusterNode *sender, uint64_t senderConfigEpoch, unsigned char *slots) {
    int j;
    clusterNode *curmaster, *newmaster = NULL;
    /* The dirty slots list is a list of slots for which we lose the ownership
     * while having still keys inside. This usually happens after a failover
     * or after a manual cluster reconfiguration operated by the admin.
     *
     * If the update message is not able to demote a master to slave (in this
     * case we'll resync with the master updating the whole key space), we
     * need to delete all the keys in the slots we lost ownership. */
    uint16_t dirty_slots[CLUSTER_SLOTS];
    int dirty_slots_count = 0;

    /* Here we set curmaster to this node or the node this node
     * replicates to if it's a slave. In the for loop we are
     * interested to check if slots are taken away from curmaster. */
    curmaster = myself->nodeIsMaster() ? myself : myself->m_slaveof;

    if (sender == myself) {
        serverLog(LL_WARNING,"Discarding UPDATE message about myself.");
        return;
    }

    for (j = 0; j < CLUSTER_SLOTS; j++) {
        if (bitmapTestBit(slots,j)) {
            /* The slot is already bound to the sender of this message. */
            if (server.cluster->m_slots[j] == sender) continue;

            /* The slot is in importing state, it should be modified only
             * manually via redis-trib (example: a resharding is in progress
             * and the migrating side slot was already closed and is advertising
             * a new config. We still want the slot to be closed manually). */
            if (server.cluster->m_importing_slots_from[j]) continue;

            /* We rebind the slot to the new node claiming it if:
             * 1) The slot was unassigned or the new node claims it with a
             *    greater configEpoch.
             * 2) We are not currently importing the slot. */
            if (server.cluster->m_slots[j] == NULL ||
                server.cluster->m_slots[j]->m_configEpoch < senderConfigEpoch)
            {
                /* Was this slot mine, and still contains keys? Mark it as
                 * a dirty slot. */
                if (server.cluster->m_slots[j] == myself &&
                    countKeysInSlot(j) &&
                    sender != myself)
                {
                    dirty_slots[dirty_slots_count] = j;
                    dirty_slots_count++;
                }

                if (server.cluster->m_slots[j] == curmaster)
                    newmaster = sender;
                clusterDelSlot(j);
                sender->clusterAddSlot(j);
                clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG|
                                     CLUSTER_TODO_UPDATE_STATE|
                                     CLUSTER_TODO_FSYNC_CONFIG);
            }
        }
    }

    /* If at least one slot was reassigned from a node to another node
     * with a greater configEpoch, it is possible that:
     * 1) We are a master left without slots. This means that we were
     *    failed over and we should turn into a replica of the new
     *    master.
     * 2) We are a slave and our master is left without slots. We need
     *    to replicate to the new slots owner. */
    if (newmaster && curmaster->m_numslots == 0) {
        serverLog(LL_WARNING,
            "Configuration change detected. Reconfiguring myself "
            "as a replica of %.40s", sender->m_name);
        clusterSetMaster(sender);
        clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG|
                             CLUSTER_TODO_UPDATE_STATE|
                             CLUSTER_TODO_FSYNC_CONFIG);
    } else if (dirty_slots_count) {
        /* If we are here, we received an update message which removed
         * ownership for certain slots we still have keys about, but still
         * we are serving some slots, so this master node was not demoted to
         * a slave.
         *
         * In order to maintain a consistent state between keys and slots
         * we need to remove all the keys from the slots we lost. */
        for (j = 0; j < dirty_slots_count; j++)
            delKeysInSlot(dirty_slots[j]);
    }
}

/* When this function is called, there is a packet to process starting
 * at node->rcvbuf. Releasing the buffer is up to the caller, so this
 * function should just handle the higher level stuff of processing the
 * packet, modifying the cluster state if needed.
 *
 * The function returns 1 if the link is still valid after the packet
 * was processed, otherwise 0 if the link was freed since the packet
 * processing lead to some inconsistency error (for instance a PONG
 * received from the wrong sender ID). */
int clusterProcessPacket(clusterLink *link) {
    clusterMsg *hdr = (clusterMsg*) link->m_rcvbuf;
    uint32_t totlen = ntohl(hdr->m_totlen);
    uint16_t type = ntohs(hdr->m_type);

    if (type < CLUSTERMSG_TYPE_COUNT)
        server.cluster->m_stats_bus_messages_received[type]++;
    serverLog(LL_DEBUG,"--- Processing packet of type %d, %lu bytes",
        type, (unsigned long) totlen);

    /* Perform sanity checks */
    if (totlen < 16) return 1; /* At least signature, version, totlen, count. */
    if (totlen > sdslen(link->m_rcvbuf)) return 1;

    if (ntohs(hdr->m_ver) != CLUSTER_PROTO_VER) {
        /* Can't handle messages of different versions. */
        return 1;
    }

    uint16_t flags = ntohs(hdr->m_flags);
    uint64_t senderCurrentEpoch = 0, senderConfigEpoch = 0;
    clusterNode *sender;

    if (type == CLUSTERMSG_TYPE_PING || type == CLUSTERMSG_TYPE_PONG ||
        type == CLUSTERMSG_TYPE_MEET)
    {
        uint16_t count = ntohs(hdr->m_count);
        uint32_t explen; /* expected length of this packet */

        explen = sizeof(clusterMsg)-sizeof(union clusterMsgData);
        explen += (sizeof(clusterMsgDataGossip)*count);
        if (totlen != explen) return 1;
    } else if (type == CLUSTERMSG_TYPE_FAIL) {
        uint32_t explen = sizeof(clusterMsg)-sizeof(union clusterMsgData);

        explen += sizeof(clusterMsgDataFail);
        if (totlen != explen) return 1;
    } else if (type == CLUSTERMSG_TYPE_PUBLISH) {
        uint32_t explen = sizeof(clusterMsg)-sizeof(union clusterMsgData);

        explen += sizeof(clusterMsgDataPublish) -
                8 +
                ntohl(hdr->m_data.publish.msg.channel_len) +
                ntohl(hdr->m_data.publish.msg.message_len);
        if (totlen != explen) return 1;
    } else if (type == CLUSTERMSG_TYPE_FAILOVER_AUTH_REQUEST ||
               type == CLUSTERMSG_TYPE_FAILOVER_AUTH_ACK ||
               type == CLUSTERMSG_TYPE_MFSTART)
    {
        uint32_t explen = sizeof(clusterMsg)-sizeof(union clusterMsgData);

        if (totlen != explen) return 1;
    } else if (type == CLUSTERMSG_TYPE_UPDATE) {
        uint32_t explen = sizeof(clusterMsg)-sizeof(union clusterMsgData);

        explen += sizeof(clusterMsgDataUpdate);
        if (totlen != explen) return 1;
    }

    /* Check if the sender is a known node. */
    sender = clusterLookupNode(hdr->m_sender);
    if (sender && !sender->nodeInHandshake()) {
        /* Update our curretEpoch if we see a newer epoch in the cluster. */
        senderCurrentEpoch = ntohu64(hdr->m_currentEpoch);
        senderConfigEpoch = ntohu64(hdr->m_configEpoch);
        if (senderCurrentEpoch > server.cluster->m_currentEpoch)
            server.cluster->m_currentEpoch = senderCurrentEpoch;
        /* Update the sender configEpoch if it is publishing a newer one. */
        if (senderConfigEpoch > sender->m_configEpoch) {
            sender->m_configEpoch = senderConfigEpoch;
            clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG|
                                 CLUSTER_TODO_FSYNC_CONFIG);
        }
        /* Update the replication offset info for this node. */
        sender->m_repl_offset = ntohu64(hdr->m_offset);
        sender->m_repl_offset_time = mstime();
        /* If we are a slave performing a manual failover and our master
         * sent its offset while already paused, populate the MF state. */
        if (server.cluster->m_mf_end &&
            myself->nodeIsSlave() &&
            myself->m_slaveof == sender &&
            hdr->m_mflags[0] & CLUSTERMSG_FLAG0_PAUSED &&
            server.cluster->m_mf_master_offset == 0)
        {
            server.cluster->m_mf_master_offset = sender->m_repl_offset;
            serverLog(LL_WARNING,
                "Received replication offset for paused "
                "master manual failover: %lld",
                server.cluster->m_mf_master_offset);
        }
    }

    /* Initial processing of PING and MEET requests replying with a PONG. */
    if (type == CLUSTERMSG_TYPE_PING || type == CLUSTERMSG_TYPE_MEET) {
        serverLog(LL_DEBUG,"Ping packet received: %p", (void*)link->m_node);

        /* We use incoming MEET messages in order to set the address
         * for 'myself', since only other cluster nodes will send us
         * MEET messages on handshakes, when the cluster joins, or
         * later if we changed address, and those nodes will use our
         * official address to connect to us. So by obtaining this address
         * from the socket is a simple way to discover / update our own
         * address in the cluster without it being hardcoded in the config.
         *
         * However if we don't have an address at all, we update the address
         * even with a normal PING packet. If it's wrong it will be fixed
         * by MEET later. */
        if ((type == CLUSTERMSG_TYPE_MEET || myself->m_ip[0] == '\0') &&
            server.cluster_announce_ip == NULL)
        {
            char ip[NET_IP_STR_LEN];

            if (anetSockName(link->m_fd,ip,sizeof(ip),NULL) != -1 &&
                strcmp(ip,myself->m_ip))
            {
                memcpy(myself->m_ip,ip,NET_IP_STR_LEN);
                serverLog(LL_WARNING,"IP address for this node updated to %s",
                    myself->m_ip);
                clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG);
            }
        }

        /* Add this node if it is new for us and the msg type is MEET.
         * In this stage we don't try to add the node with the right
         * flags, slaveof pointer, and so forth, as this details will be
         * resolved when we'll receive PONGs from the node. */
        if (!sender && type == CLUSTERMSG_TYPE_MEET) {
            clusterNode *node;

            node = createClusterNode(NULL,CLUSTER_NODE_HANDSHAKE);
            nodeIp2String(node->m_ip,link,hdr->m_myip);
            node->m_port = ntohs(hdr->m_port);
            node->m_cport = ntohs(hdr->m_cport);
            clusterAddNode(node);
            clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG);
        }

        /* If this is a MEET packet from an unknown node, we still process
         * the gossip section here since we have to trust the sender because
         * of the message type. */
        if (!sender && type == CLUSTERMSG_TYPE_MEET)
            clusterProcessGossipSection(hdr,link);

        /* Anyway reply with a PONG */
        clusterSendPing(link,CLUSTERMSG_TYPE_PONG);
    }

    /* PING, PONG, MEET: process config information. */
    if (type == CLUSTERMSG_TYPE_PING || type == CLUSTERMSG_TYPE_PONG ||
        type == CLUSTERMSG_TYPE_MEET)
    {
        serverLog(LL_DEBUG,"%s packet received: %p",
            type == CLUSTERMSG_TYPE_PING ? "ping" : "pong",
            (void*)link->m_node);
        if (link->m_node) {
            if (link->m_node->nodeInHandshake()) {
                /* If we already have this node, try to change the
                 * IP/port of the node with the new one. */
                if (sender) {
                    serverLog(LL_VERBOSE,
                        "Handshake: we already know node %.40s, "
                        "updating the address if needed.", sender->m_name);
                    if (nodeUpdateAddressIfNeeded(sender,link,hdr))
                    {
                        clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG|
                                             CLUSTER_TODO_UPDATE_STATE);
                    }
                    /* Free this node as we already have it. This will
                     * cause the link to be freed as well. */
                    clusterDelNode(link->m_node);
                    return 0;
                }

                /* First thing to do is replacing the random name with the
                 * right node name if this was a handshake stage. */
                clusterRenameNode(link->m_node, hdr->m_sender);
                serverLog(LL_DEBUG,"Handshake with node %.40s completed.",
                    link->m_node->m_name);
                link->m_node->m_flags &= ~CLUSTER_NODE_HANDSHAKE;
                link->m_node->m_flags |= flags&(CLUSTER_NODE_MASTER|CLUSTER_NODE_SLAVE);
                clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG);
            } else if (memcmp(link->m_node->m_name,hdr->m_sender,
                        CLUSTER_NAMELEN) != 0)
            {
                /* If the reply has a non matching node ID we
                 * disconnect this node and set it as not having an associated
                 * address. */
                serverLog(LL_DEBUG,"PONG contains mismatching sender ID. About node %.40s added %d ms ago, having flags %d",
                    link->m_node->m_name,
                    (int)(mstime()-(link->m_node->m_ctime)),
                    link->m_node->m_flags);
                link->m_node->m_flags |= CLUSTER_NODE_NOADDR;
                link->m_node->m_ip[0] = '\0';
                link->m_node->m_port = 0;
                link->m_node->m_cport = 0;
                freeClusterLink(link);
                clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG);
                return 0;
            }
        }

        /* Update the node address if it changed. */
        if (sender && type == CLUSTERMSG_TYPE_PING &&
            !sender->nodeInHandshake() &&
            nodeUpdateAddressIfNeeded(sender,link,hdr))
        {
            clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG|
                                 CLUSTER_TODO_UPDATE_STATE);
        }

        /* Update our info about the node */
        if (link->m_node && type == CLUSTERMSG_TYPE_PONG) {
            link->m_node->m_pong_received = mstime();
            link->m_node->m_ping_sent = 0;

            /* The PFAIL condition can be reversed without external
             * help if it is momentary (that is, if it does not
             * turn into a FAIL state).
             *
             * The FAIL condition is also reversible under specific
             * conditions detected by clearNodeFailureIfNeeded(). */
            if (link->m_node->nodeTimedOut()) {
                link->m_node->m_flags &= ~CLUSTER_NODE_PFAIL;
                clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG|
                                     CLUSTER_TODO_UPDATE_STATE);
            } else if (link->m_node->nodeFailed()) {
                clearNodeFailureIfNeeded(link->m_node);
            }
        }

        /* Check for role switch: slave -> master or master -> slave. */
        if (sender) {
            if (!memcmp(hdr->m_slaveof,CLUSTER_NODE_NULL_NAME,
                sizeof(hdr->m_slaveof)))
            {
                /* Node is a master. */
                sender->clusterSetNodeAsMaster();
            } else {
                /* Node is a slave. */
                clusterNode *master = clusterLookupNode(hdr->m_slaveof);

                if (sender->nodeIsMaster()) {
                    /* Master turned into a slave! Reconfigure the node. */
                    sender->clusterDelNodeSlots();
                    sender->m_flags &= ~(CLUSTER_NODE_MASTER|
                                       CLUSTER_NODE_MIGRATE_TO);
                    sender->m_flags |= CLUSTER_NODE_SLAVE;

                    /* Update config and state. */
                    clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG|
                                         CLUSTER_TODO_UPDATE_STATE);
                }

                /* Master node changed for this slave? */
                if (master && sender->m_slaveof != master) {
                    if (sender->m_slaveof)
                        clusterNodeRemoveSlave(sender->m_slaveof,sender);
                    master->clusterNodeAddSlave(sender);
                    sender->m_slaveof = master;

                    /* Update config. */
                    clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG);
                }
            }
        }

        /* Update our info about served slots.
         *
         * Note: this MUST happen after we update the master/slave state
         * so that CLUSTER_NODE_MASTER flag will be set. */

        /* Many checks are only needed if the set of served slots this
         * instance claims is different compared to the set of slots we have
         * for it. Check this ASAP to avoid other computational expansive
         * checks later. */
        clusterNode *sender_master = NULL; /* Sender or its master if slave. */
        int dirty_slots = 0; /* Sender claimed slots don't match my view? */

        if (sender) {
            sender_master = sender->nodeIsMaster() ? sender : sender->m_slaveof;
            if (sender_master) {
                dirty_slots = memcmp(sender_master->m_slots,
                        hdr->m_myslots,sizeof(hdr->m_myslots)) != 0;
            }
        }

        /* 1) If the sender of the message is a master, and we detected that
         *    the set of slots it claims changed, scan the slots to see if we
         *    need to update our configuration. */
        if (sender && sender->nodeIsMaster() && dirty_slots)
            clusterUpdateSlotsConfigWith(sender,senderConfigEpoch,hdr->m_myslots);

        /* 2) We also check for the reverse condition, that is, the sender
         *    claims to serve slots we know are served by a master with a
         *    greater configEpoch. If this happens we inform the sender.
         *
         * This is useful because sometimes after a partition heals, a
         * reappearing master may be the last one to claim a given set of
         * hash slots, but with a configuration that other instances know to
         * be deprecated. Example:
         *
         * A and B are master and slave for slots 1,2,3.
         * A is partitioned away, B gets promoted.
         * B is partitioned away, and A returns available.
         *
         * Usually B would PING A publishing its set of served slots and its
         * configEpoch, but because of the partition B can't inform A of the
         * new configuration, so other nodes that have an updated table must
         * do it. In this way A will stop to act as a master (or can try to
         * failover if there are the conditions to win the election). */
        if (sender && dirty_slots) {
            int j;

            for (j = 0; j < CLUSTER_SLOTS; j++) {
                if (bitmapTestBit(hdr->m_myslots,j)) {
                    if (server.cluster->m_slots[j] == sender ||
                        server.cluster->m_slots[j] == NULL) continue;
                    if (server.cluster->m_slots[j]->m_configEpoch >
                        senderConfigEpoch)
                    {
                        serverLog(LL_VERBOSE,
                            "Node %.40s has old slots configuration, sending "
                            "an UPDATE message about %.40s",
                                sender->m_name, server.cluster->m_slots[j]->m_name);
                        clusterSendUpdate(sender->m_link,
                            server.cluster->m_slots[j]);

                        /* TODO: instead of exiting the loop send every other
                         * UPDATE packet for other nodes that are the new owner
                         * of sender's slots. */
                        break;
                    }
                }
            }
        }

        /* If our config epoch collides with the sender's try to fix
         * the problem. */
        if (sender &&
            myself->nodeIsMaster() && sender->nodeIsMaster() &&
            senderConfigEpoch == myself->m_configEpoch)
        {
            clusterHandleConfigEpochCollision(sender);
        }

        /* Get info from the gossip section */
        if (sender) clusterProcessGossipSection(hdr,link);
    } else if (type == CLUSTERMSG_TYPE_FAIL) {
        clusterNode *failing;

        if (sender) {
            failing = clusterLookupNode(hdr->m_data.fail.about.nodename);
            if (failing &&
                !(failing->m_flags & (CLUSTER_NODE_FAIL|CLUSTER_NODE_MYSELF)))
            {
                serverLog(LL_NOTICE,
                    "FAIL message received from %.40s about %.40s",
                    hdr->m_sender, hdr->m_data.fail.about.nodename);
                failing->m_flags |= CLUSTER_NODE_FAIL;
                failing->m_fail_time = mstime();
                failing->m_flags &= ~CLUSTER_NODE_PFAIL;
                clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG|
                                     CLUSTER_TODO_UPDATE_STATE);
            }
        } else {
            serverLog(LL_NOTICE,
                "Ignoring FAIL message from unknown node %.40s about %.40s",
                hdr->m_sender, hdr->m_data.fail.about.nodename);
        }
    } else if (type == CLUSTERMSG_TYPE_PUBLISH) {
        robj *channel, *message;
        uint32_t channel_len, message_len;

        /* Don't bother creating useless objects if there are no
         * Pub/Sub subscribers. */
        if (server.pubsub_channels->dictSize() ||
           server.pubsub_patterns->listLength())
        {
            channel_len = ntohl(hdr->m_data.publish.msg.channel_len);
            message_len = ntohl(hdr->m_data.publish.msg.message_len);
            channel = createStringObject(
                        (char*)hdr->m_data.publish.msg.bulk_data,channel_len);
            message = createStringObject(
                        (char*)hdr->m_data.publish.msg.bulk_data+channel_len,
                        message_len);
            pubsubPublishMessage(channel,message);
            decrRefCount(channel);
            decrRefCount(message);
        }
    } else if (type == CLUSTERMSG_TYPE_FAILOVER_AUTH_REQUEST) {
        if (!sender) return 1;  /* We don't know that node. */
        clusterSendFailoverAuthIfNeeded(sender,hdr);
    } else if (type == CLUSTERMSG_TYPE_FAILOVER_AUTH_ACK) {
        if (!sender) return 1;  /* We don't know that node. */
        /* We consider this vote only if the sender is a master serving
         * a non zero number of slots, and its currentEpoch is greater or
         * equal to epoch where this node started the election. */
        if (sender->nodeIsMaster() && sender->m_numslots > 0 &&
            senderCurrentEpoch >= server.cluster->m_failover_auth_epoch)
        {
            server.cluster->m_failover_auth_count++;
            /* Maybe we reached a quorum here, set a flag to make sure
             * we check ASAP. */
            clusterDoBeforeSleep(CLUSTER_TODO_HANDLE_FAILOVER);
        }
    } else if (type == CLUSTERMSG_TYPE_MFSTART) {
        /* This message is acceptable only if I'm a master and the sender
         * is one of my slaves. */
        if (!sender || sender->m_slaveof != myself) return 1;
        /* Manual failover requested from slaves. Initialize the state
         * accordingly. */
        resetManualFailover();
        server.cluster->m_mf_end = mstime() + CLUSTER_MF_TIMEOUT;
        server.cluster->m_mf_slave = sender;
        pauseClients(mstime()+(CLUSTER_MF_TIMEOUT*2));
        serverLog(LL_WARNING,"Manual failover requested by slave %.40s.",
            sender->m_name);
    } else if (type == CLUSTERMSG_TYPE_UPDATE) {
        clusterNode *n; /* The node the update is about. */
        uint64_t reportedConfigEpoch =
                    ntohu64(hdr->m_data.update.nodecfg.configEpoch);

        if (!sender) return 1;  /* We don't know the sender. */
        n = clusterLookupNode(hdr->m_data.update.nodecfg.nodename);
        if (!n) return 1;   /* We don't know the reported node. */
        if (n->m_configEpoch >= reportedConfigEpoch) return 1; /* Nothing new. */

        /* If in our current config the node is a slave, set it as a master. */
        if (n->nodeIsSlave()) n->clusterSetNodeAsMaster();

        /* Update the node's configEpoch. */
        n->m_configEpoch = reportedConfigEpoch;
        clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG|
                             CLUSTER_TODO_FSYNC_CONFIG);

        /* Check the bitmap of served slots and update our
         * config accordingly. */
        clusterUpdateSlotsConfigWith(n,reportedConfigEpoch,
            hdr->m_data.update.nodecfg.slots);
    } else {
        serverLog(LL_WARNING,"Received unknown packet type: %d", type);
    }
    return 1;
}

/* This function is called when we detect the link with this node is lost.
   We set the node as no longer connected. The Cluster Cron will detect
   this connection and will try to get it connected again.

   Instead if the node is a temporary node used to accept a query, we
   completely free the node on error. */
void handleLinkIOError(clusterLink *link) {
    freeClusterLink(link);
}

/* Send data. This is handled using a trivial send buffer that gets
 * consumed by write(). We don't try to optimize this for speed too much
 * as this is a very low traffic channel. */
void clusterWriteHandler(aeEventLoop *el, int fd, void *privdata, int mask) {
    clusterLink *link = (clusterLink*) privdata;
    ssize_t nwritten;
    UNUSED(el);
    UNUSED(mask);

    nwritten = write(fd, link->m_sndbuf, sdslen(link->m_sndbuf));
    if (nwritten <= 0) {
        serverLog(LL_DEBUG,"I/O error writing to node link: %s",
            strerror(errno));
        handleLinkIOError(link);
        return;
    }
    sdsrange(link->m_sndbuf,nwritten,-1);
    if (sdslen(link->m_sndbuf) == 0)
        server.el->aeDeleteFileEvent( link->m_fd, AE_WRITABLE);
}

/* Read data. Try to read the first field of the header first to check the
 * full length of the packet. When a whole packet is in memory this function
 * will call the function to process the packet. And so forth. */
void clusterReadHandler(aeEventLoop *el, int fd, void *privdata, int mask) {
    char buf[sizeof(clusterMsg)];
    ssize_t nread;
    clusterMsg *hdr;
    clusterLink *link = (clusterLink*) privdata;
    unsigned int readlen, rcvbuflen;
    UNUSED(el);
    UNUSED(mask);

    while(1) { /* Read as long as there is data to read. */
        rcvbuflen = sdslen(link->m_rcvbuf);
        if (rcvbuflen < 8) {
            /* First, obtain the first 8 bytes to get the full message
             * length. */
            readlen = 8 - rcvbuflen;
        } else {
            /* Finally read the full message. */
            hdr = (clusterMsg*) link->m_rcvbuf;
            if (rcvbuflen == 8) {
                /* Perform some sanity check on the message signature
                 * and length. */
                if (memcmp(hdr->m_sig,"RCmb",4) != 0 ||
                    ntohl(hdr->m_totlen) < CLUSTERMSG_MIN_LEN)
                {
                    serverLog(LL_WARNING,
                        "Bad message length or signature received "
                        "from Cluster bus.");
                    handleLinkIOError(link);
                    return;
                }
            }
            readlen = ntohl(hdr->m_totlen) - rcvbuflen;
            if (readlen > sizeof(buf)) readlen = sizeof(buf);
        }

        nread = read(fd,buf,readlen);
        if (nread == -1 && errno == EAGAIN) return; /* No more data ready. */

        if (nread <= 0) {
            /* I/O error... */
            serverLog(LL_DEBUG,"I/O error reading from node link: %s",
                (nread == 0) ? "connection closed" : strerror(errno));
            handleLinkIOError(link);
            return;
        } else {
            /* Read data and recast the pointer to the new buffer. */
            link->m_rcvbuf = sdscatlen(link->m_rcvbuf,buf,nread);
            hdr = (clusterMsg*) link->m_rcvbuf;
            rcvbuflen += nread;
        }

        /* Total length obtained? Process this packet. */
        if (rcvbuflen >= 8 && rcvbuflen == ntohl(hdr->m_totlen)) {
            if (clusterProcessPacket(link)) {
                sdsfree(link->m_rcvbuf);
                link->m_rcvbuf = sdsempty();
            } else {
                return; /* Link no longer valid. */
            }
        }
    }
}

/* Put stuff into the send buffer.
 *
 * It is guaranteed that this function will never have as a side effect
 * the link to be invalidated, so it is safe to call this function
 * from event handlers that will do stuff with the same link later. */
void clusterLink::clusterSendMessage(unsigned char *msg, size_t msglen)
{
    if (sdslen(m_sndbuf) == 0 && msglen != 0)
        server.el->aeCreateFileEvent(m_fd,AE_WRITABLE,
                    clusterWriteHandler,this);

    m_sndbuf = sdscatlen(m_sndbuf, msg, msglen);

    /* Populate sent messages stats. */
    clusterMsg *hdr = (clusterMsg*) msg;
    uint16_t type = ntohs(hdr->m_type);
    if (type < CLUSTERMSG_TYPE_COUNT)
        server.cluster->m_stats_bus_messages_sent[type]++;
}

/* Send a message to all the nodes that are part of the cluster having
 * a connected link.
 *
 * It is guaranteed that this function will never have as a side effect
 * some node->m_link to be invalidated, so it is safe to call this function
 * from event handlers that will do stuff with node links later. */
void clusterBroadcastMessage(void *buf, size_t len) {
    dictEntry *de;

    dictIterator di(server.cluster->m_nodes, 1);
    while((de = di.dictNext()) != NULL) {
        clusterNode* node = (clusterNode*)de->dictGetVal();

        if (!node->m_link) continue;
        if (node->m_flags & (CLUSTER_NODE_MYSELF|CLUSTER_NODE_HANDSHAKE))
            continue;
        node->m_link->clusterSendMessage((unsigned char *)buf,len);
    }
}

/* Build the message header. hdr must point to a buffer at least
 * sizeof(clusterMsg) in bytes. */
void clusterBuildMessageHdr(clusterMsg *hdr, int type) {
    int totlen = 0;
    uint64_t offset;
    clusterNode *master;

    /* If this node is a master, we send its slots bitmap and configEpoch.
     * If this node is a slave we send the master's information instead (the
     * node is flagged as slave so the receiver knows that it is NOT really
     * in charge for this slots. */
    master = (myself->nodeIsSlave() && myself->m_slaveof) ?
              myself->m_slaveof : myself;

    memset(hdr,0,sizeof(*hdr));
    hdr->m_ver = htons(CLUSTER_PROTO_VER);
    hdr->m_sig[0] = 'R';
    hdr->m_sig[1] = 'C';
    hdr->m_sig[2] = 'm';
    hdr->m_sig[3] = 'b';
    hdr->m_type = htons(type);
    memcpy(hdr->m_sender,myself->m_name,CLUSTER_NAMELEN);

    /* If cluster-announce-ip option is enabled, force the receivers of our
     * packets to use the specified address for this node. Otherwise if the
     * first byte is zero, they'll do auto discovery. */
    memset(hdr->m_myip,0,NET_IP_STR_LEN);
    if (server.cluster_announce_ip) {
        strncpy(hdr->m_myip,server.cluster_announce_ip,NET_IP_STR_LEN);
        hdr->m_myip[NET_IP_STR_LEN-1] = '\0';
    }

    /* Handle cluster-announce-port as well. */
    int announced_port = server.cluster_announce_port ?
                         server.cluster_announce_port : server.port;
    int announced_cport = server.cluster_announce_bus_port ?
                          server.cluster_announce_bus_port :
                          (server.port + CLUSTER_PORT_INCR);

    memcpy(hdr->m_myslots,master->m_slots,sizeof(hdr->m_myslots));
    memset(hdr->m_slaveof,0,CLUSTER_NAMELEN);
    if (myself->m_slaveof != NULL)
        memcpy(hdr->m_slaveof,myself->m_slaveof->m_name, CLUSTER_NAMELEN);
    hdr->m_port = htons(announced_port);
    hdr->m_cport = htons(announced_cport);
    hdr->m_flags = htons(myself->m_flags);
    hdr->m_state = server.cluster->m_state;

    /* Set the currentEpoch and configEpochs. */
    hdr->m_currentEpoch = htonu64(server.cluster->m_currentEpoch);
    hdr->m_configEpoch = htonu64(master->m_configEpoch);

    /* Set the replication offset. */
    if (myself->nodeIsSlave())
        offset = replicationGetSlaveOffset();
    else
        offset = server.master_repl_offset;
    hdr->m_offset = htonu64(offset);

    /* Set the message flags. */
    if (myself->nodeIsMaster() && server.cluster->m_mf_end)
        hdr->m_mflags[0] |= CLUSTERMSG_FLAG0_PAUSED;

    /* Compute the message length for certain messages. For other messages
     * this is up to the caller. */
    if (type == CLUSTERMSG_TYPE_FAIL) {
        totlen = sizeof(clusterMsg)-sizeof(union clusterMsgData);
        totlen += sizeof(clusterMsgDataFail);
    } else if (type == CLUSTERMSG_TYPE_UPDATE) {
        totlen = sizeof(clusterMsg)-sizeof(union clusterMsgData);
        totlen += sizeof(clusterMsgDataUpdate);
    }
    hdr->m_totlen = htonl(totlen);
    /* For PING, PONG, and MEET, fixing the totlen field is up to the caller. */
}

/* Return non zero if the node is already present in the gossip section of the
 * message pointed by 'hdr' and having 'count' gossip entries. Otherwise
 * zero is returned. Helper for clusterSendPing(). */
int clusterNodeIsInGossipSection(clusterMsg *hdr, int count, clusterNode *n) {
    int j;
    for (j = 0; j < count; j++) {
        if (memcmp(hdr->m_data.ping.gossip[j].m_nodename,n->m_name,
                CLUSTER_NAMELEN) == 0) break;
    }
    return j != count;
}

/* Set the i-th entry of the gossip section in the message pointed by 'hdr'
 * to the info of the specified node 'n'. */
void clusterSetGossipEntry(clusterMsg *hdr, int i, clusterNode *n) {
    clusterMsgDataGossip *gossip;
    gossip = &(hdr->m_data.ping.gossip[i]);
    memcpy(gossip->m_nodename,n->m_name,CLUSTER_NAMELEN);
    gossip->m_ping_sent = htonl(n->m_ping_sent/1000);
    gossip->m_pong_received = htonl(n->m_pong_received/1000);
    memcpy(gossip->m_ip,n->m_ip,sizeof(n->m_ip));
    gossip->m_port = htons(n->m_port);
    gossip->m_cport = htons(n->m_cport);
    gossip->m_flags = htons(n->m_flags);
    gossip->m_notused1 = 0;
}

/* Send a PING or PONG packet to the specified node, making sure to add enough
 * gossip informations. */
void clusterSendPing(clusterLink *link, int type) {
    unsigned char *buf;
    clusterMsg *hdr;
    int gossipcount = 0; /* Number of gossip sections added so far. */
    int wanted; /* Number of gossip sections we want to append if possible. */
    int totlen; /* Total packet length. */
    /* freshnodes is the max number of nodes we can hope to append at all:
     * nodes available minus two (ourself and the node we are sending the
     * message to). However practically there may be less valid nodes since
     * nodes in handshake state, disconnected, are not considered. */
    int freshnodes = server.cluster->m_nodes->dictSize()-2;

    /* How many gossip sections we want to add? 1/10 of the number of nodes
     * and anyway at least 3. Why 1/10?
     *
     * If we have N masters, with N/10 entries, and we consider that in
     * node_timeout we exchange with each other node at least 4 packets
     * (we ping in the worst case in node_timeout/2 time, and we also
     * receive two pings from the host), we have a total of 8 packets
     * in the node_timeout*2 falure reports validity time. So we have
     * that, for a single PFAIL node, we can expect to receive the following
     * number of failure reports (in the specified window of time):
     *
     * PROB * GOSSIP_ENTRIES_PER_PACKET * TOTAL_PACKETS:
     *
     * PROB = probability of being featured in a single gossip entry,
     *        which is 1 / NUM_OF_NODES.
     * ENTRIES = 10.
     * TOTAL_PACKETS = 2 * 4 * NUM_OF_MASTERS.
     *
     * If we assume we have just masters (so num of nodes and num of masters
     * is the same), with 1/10 we always get over the majority, and specifically
     * 80% of the number of nodes, to account for many masters failing at the
     * same time.
     *
     * Since we have non-voting slaves that lower the probability of an entry
     * to feature our node, we set the number of entires per packet as
     * 10% of the total nodes we have. */
    wanted = floor(server.cluster->m_nodes->dictSize()/10);
    if (wanted < 3) wanted = 3;
    if (wanted > freshnodes) wanted = freshnodes;

    /* Include all the nodes in PFAIL state, so that failure reports are
     * faster to propagate to go from PFAIL to FAIL state. */
    int pfail_wanted = server.cluster->m_stats_pfail_nodes;

    /* Compute the maxium totlen to allocate our buffer. We'll fix the totlen
     * later according to the number of gossip sections we really were able
     * to put inside the packet. */
    totlen = sizeof(clusterMsg)-sizeof(union clusterMsgData);
    totlen += (sizeof(clusterMsgDataGossip)*(wanted+pfail_wanted));
    /* Note: clusterBuildMessageHdr() expects the buffer to be always at least
     * sizeof(clusterMsg) or more. */
    if (totlen < (int)sizeof(clusterMsg)) totlen = sizeof(clusterMsg);
    buf = (unsigned char *)zcalloc(totlen);
    hdr = (clusterMsg*) buf;

    /* Populate the header. */
    if (link->m_node && type == CLUSTERMSG_TYPE_PING)
        link->m_node->m_ping_sent = mstime();
    clusterBuildMessageHdr(hdr,type);

    /* Populate the gossip fields */
    int maxiterations = wanted*3;
    while(freshnodes > 0 && gossipcount < wanted && maxiterations--) {
        dictEntry *de = server.cluster->m_nodes->dictGetRandomKey();
        clusterNode* _this = (clusterNode*)de->dictGetVal();

        /* Don't include this node: the whole packet header is about us
         * already, so we just gossip about other nodes. */
        if (_this == myself) continue;

        /* PFAIL nodes will be added later. */
        if (_this->m_flags & CLUSTER_NODE_PFAIL) continue;

        /* In the gossip section don't include:
         * 1) Nodes in HANDSHAKE state.
         * 3) Nodes with the NOADDR flag set.
         * 4) Disconnected nodes if they don't have configured slots.
         */
        if (_this->m_flags & (CLUSTER_NODE_HANDSHAKE|CLUSTER_NODE_NOADDR) ||
            (_this->m_link == NULL && _this->m_numslots == 0))
        {
            freshnodes--; /* Tecnically not correct, but saves CPU. */
            continue;
        }

        /* Do not add a node we already have. */
        if (clusterNodeIsInGossipSection(hdr,gossipcount,_this)) continue;

        /* Add it */
        clusterSetGossipEntry(hdr,gossipcount,_this);
        freshnodes--;
        gossipcount++;
    }

    /* If there are PFAIL nodes, add them at the end. */
    if (pfail_wanted) {
        dictEntry *de;

        dictIterator di(server.cluster->m_nodes, 1);
        while((de = di.dictNext()) != NULL && pfail_wanted > 0) {
            clusterNode* node = (clusterNode*)de->dictGetVal();
            if (node->m_flags & CLUSTER_NODE_HANDSHAKE) continue;
            if (node->m_flags & CLUSTER_NODE_NOADDR) continue;
            if (!(node->m_flags & CLUSTER_NODE_PFAIL)) continue;
            clusterSetGossipEntry(hdr,gossipcount,node);
            freshnodes--;
            gossipcount++;
            /* We take the count of the slots we allocated, since the
             * PFAIL stats may not match perfectly with the current number
             * of PFAIL nodes. */
            pfail_wanted--;
        }
    }

    /* Ready to send... fix the totlen fiend and queue the message in the
     * output buffer. */
    totlen = sizeof(clusterMsg)-sizeof(union clusterMsgData);
    totlen += (sizeof(clusterMsgDataGossip)*gossipcount);
    hdr->m_count = htons(gossipcount);
    hdr->m_totlen = htonl(totlen);
    link->clusterSendMessage(buf,totlen);
    zfree(buf);
}

/* Send a PONG packet to every connected node that's not in handshake state
 * and for which we have a valid link.
 *
 * In Redis Cluster pongs are not used just for failure detection, but also
 * to carry important configuration information. So broadcasting a pong is
 * useful when something changes in the configuration and we want to make
 * the cluster aware ASAP (for instance after a slave promotion).
 *
 * The 'target' argument specifies the receiving instances using the
 * defines below:
 *
 * CLUSTER_BROADCAST_ALL -> All known instances.
 * CLUSTER_BROADCAST_LOCAL_SLAVES -> All slaves in my master-slaves ring.
 */
#define CLUSTER_BROADCAST_ALL 0
#define CLUSTER_BROADCAST_LOCAL_SLAVES 1
void clusterBroadcastPong(int target) {
    dictEntry *de;

    dictIterator di(server.cluster->m_nodes, 1);
    while((de = di.dictNext()) != NULL) {
        clusterNode* node = (clusterNode*)de->dictGetVal();

        if (!node->m_link) continue;
        if (node == myself || node->nodeInHandshake()) continue;
        if (target == CLUSTER_BROADCAST_LOCAL_SLAVES) {
            int local_slave =
                node->nodeIsSlave() && node->m_slaveof &&
                (node->m_slaveof == myself || node->m_slaveof == myself->m_slaveof);
            if (!local_slave) continue;
        }
        clusterSendPing(node->m_link,CLUSTERMSG_TYPE_PONG);
    }
}

/* Send a PUBLISH message.
 *
 * If link is NULL, then the message is broadcasted to the whole cluster. */
void clusterSendPublish(clusterLink *link, robj *channel, robj *message) {
    unsigned char buf[sizeof(clusterMsg)], *payload;
    clusterMsg *hdr = (clusterMsg*) buf;
    uint32_t totlen;
    uint32_t channel_len, message_len;

    channel = getDecodedObject(channel);
    message = getDecodedObject(message);
    channel_len = sdslen((sds)channel->ptr);
    message_len = sdslen((sds)message->ptr);

    clusterBuildMessageHdr(hdr,CLUSTERMSG_TYPE_PUBLISH);
    totlen = sizeof(clusterMsg)-sizeof(union clusterMsgData);
    totlen += sizeof(clusterMsgDataPublish) - 8 + channel_len + message_len;

    hdr->m_data.publish.msg.channel_len = htonl(channel_len);
    hdr->m_data.publish.msg.message_len = htonl(message_len);
    hdr->m_totlen = htonl(totlen);

    /* Try to use the local buffer if possible */
    if (totlen < sizeof(buf)) {
        payload = buf;
    } else {
        payload = (unsigned char *)zmalloc(totlen);
        memcpy(payload,hdr,sizeof(*hdr));
        hdr = (clusterMsg*) payload;
    }
    memcpy(hdr->m_data.publish.msg.bulk_data,channel->ptr,sdslen((sds)channel->ptr));
    memcpy(hdr->m_data.publish.msg.bulk_data+sdslen((sds)channel->ptr),
        message->ptr,sdslen((sds)message->ptr));

    if (link)
        link->clusterSendMessage(payload,totlen);
    else
        clusterBroadcastMessage(payload,totlen);

    decrRefCount(channel);
    decrRefCount(message);
    if (payload != buf) zfree(payload);
}

/* Send a FAIL message to all the nodes we are able to contact.
 * The FAIL message is sent when we detect that a node is failing
 * (CLUSTER_NODE_PFAIL) and we also receive a gossip confirmation of this:
 * we switch the node state to CLUSTER_NODE_FAIL and ask all the other
 * nodes to do the same ASAP. */
void clusterSendFail(char *nodename) {
    unsigned char buf[sizeof(clusterMsg)];
    clusterMsg *hdr = (clusterMsg*) buf;

    clusterBuildMessageHdr(hdr,CLUSTERMSG_TYPE_FAIL);
    memcpy(hdr->m_data.fail.about.nodename,nodename,CLUSTER_NAMELEN);
    clusterBroadcastMessage(buf,ntohl(hdr->m_totlen));
}

/* Send an UPDATE message to the specified link carrying the specified 'node'
 * slots configuration. The node name, slots bitmap, and configEpoch info
 * are included. */
void clusterSendUpdate(clusterLink *link, clusterNode *node) {
    unsigned char buf[sizeof(clusterMsg)];
    clusterMsg *hdr = (clusterMsg*) buf;

    if (link == NULL) return;
    clusterBuildMessageHdr(hdr,CLUSTERMSG_TYPE_UPDATE);
    memcpy(hdr->m_data.update.nodecfg.nodename,node->m_name,CLUSTER_NAMELEN);
    hdr->m_data.update.nodecfg.configEpoch = htonu64(node->m_configEpoch);
    memcpy(hdr->m_data.update.nodecfg.slots,node->m_slots,sizeof(node->m_slots));
    link->clusterSendMessage(buf,ntohl(hdr->m_totlen));
}

/* -----------------------------------------------------------------------------
 * CLUSTER Pub/Sub support
 *
 * For now we do very little, just propagating PUBLISH messages across the whole
 * cluster. In the future we'll try to get smarter and avoiding propagating those
 * messages to hosts without receives for a given channel.
 * -------------------------------------------------------------------------- */
void clusterPropagatePublish(robj *channel, robj *message) {
    clusterSendPublish(NULL, channel, message);
}

/* -----------------------------------------------------------------------------
 * SLAVE node specific functions
 * -------------------------------------------------------------------------- */

/* This function sends a FAILOVE_AUTH_REQUEST message to every node in order to
 * see if there is the quorum for this slave instance to failover its failing
 * master.
 *
 * Note that we send the failover request to everybody, master and slave nodes,
 * but only the masters are supposed to reply to our query. */
void clusterRequestFailoverAuth(void) {
    unsigned char buf[sizeof(clusterMsg)];
    clusterMsg *hdr = (clusterMsg*) buf;
    uint32_t totlen;

    clusterBuildMessageHdr(hdr,CLUSTERMSG_TYPE_FAILOVER_AUTH_REQUEST);
    /* If this is a manual failover, set the CLUSTERMSG_FLAG0_FORCEACK bit
     * in the header to communicate the nodes receiving the message that
     * they should authorized the failover even if the master is working. */
    if (server.cluster->m_mf_end) hdr->m_mflags[0] |= CLUSTERMSG_FLAG0_FORCEACK;
    totlen = sizeof(clusterMsg)-sizeof(union clusterMsgData);
    hdr->m_totlen = htonl(totlen);
    clusterBroadcastMessage(buf,totlen);
}

/* Send a FAILOVER_AUTH_ACK message to the specified node. */
void clusterSendFailoverAuth(clusterNode *node) {
    unsigned char buf[sizeof(clusterMsg)];
    clusterMsg *hdr = (clusterMsg*) buf;
    uint32_t totlen;

    if (!node->m_link) return;
    clusterBuildMessageHdr(hdr,CLUSTERMSG_TYPE_FAILOVER_AUTH_ACK);
    totlen = sizeof(clusterMsg)-sizeof(union clusterMsgData);
    hdr->m_totlen = htonl(totlen);
    node->m_link->clusterSendMessage(buf,totlen);
}

/* Send a MFSTART message to the specified node. */
void clusterSendMFStart(clusterNode *node) {
    unsigned char buf[sizeof(clusterMsg)];
    clusterMsg *hdr = (clusterMsg*) buf;
    uint32_t totlen;

    if (!node->m_link) return;
    clusterBuildMessageHdr(hdr,CLUSTERMSG_TYPE_MFSTART);
    totlen = sizeof(clusterMsg)-sizeof(union clusterMsgData);
    hdr->m_totlen = htonl(totlen);
    node->m_link->clusterSendMessage(buf,totlen);
}

/* Vote for the node asking for our vote if there are the conditions. */
void clusterSendFailoverAuthIfNeeded(clusterNode *node, clusterMsg *request) {
    clusterNode *master = node->m_slaveof;
    uint64_t requestCurrentEpoch = ntohu64(request->m_currentEpoch);
    uint64_t requestConfigEpoch = ntohu64(request->m_configEpoch);
    unsigned char *claimed_slots = request->m_myslots;
    int force_ack = request->m_mflags[0] & CLUSTERMSG_FLAG0_FORCEACK;
    int j;

    /* IF we are not a master serving at least 1 slot, we don't have the
     * right to vote, as the cluster size in Redis Cluster is the number
     * of masters serving at least one slot, and quorum is the cluster
     * size + 1 */
    if (myself->nodeIsSlave() || myself->m_numslots == 0) return;

    /* Request epoch must be >= our currentEpoch.
     * Note that it is impossible for it to actually be greater since
     * our currentEpoch was updated as a side effect of receiving this
     * request, if the request epoch was greater. */
    if (requestCurrentEpoch < server.cluster->m_currentEpoch) {
        serverLog(LL_WARNING,
            "Failover auth denied to %.40s: reqEpoch (%llu) < curEpoch(%llu)",
            node->m_name,
            (unsigned long long) requestCurrentEpoch,
            (unsigned long long) server.cluster->m_currentEpoch);
        return;
    }

    /* I already voted for this epoch? Return ASAP. */
    if (server.cluster->m_lastVoteEpoch == server.cluster->m_currentEpoch) {
        serverLog(LL_WARNING,
                "Failover auth denied to %.40s: already voted for epoch %llu",
                node->m_name,
                (unsigned long long) server.cluster->m_currentEpoch);
        return;
    }

    /* Node must be a slave and its master down.
     * The master can be non failing if the request is flagged
     * with CLUSTERMSG_FLAG0_FORCEACK (manual failover). */
    if (node->nodeIsMaster() || master == NULL ||
        (!master->nodeFailed() && !force_ack))
    {
        if (node->nodeIsMaster()) {
            serverLog(LL_WARNING,
                    "Failover auth denied to %.40s: it is a master node",
                    node->m_name);
        } else if (master == NULL) {
            serverLog(LL_WARNING,
                    "Failover auth denied to %.40s: I don't know its master",
                    node->m_name);
        } else if (!master->nodeFailed()) {
            serverLog(LL_WARNING,
                    "Failover auth denied to %.40s: its master is up",
                    node->m_name);
        }
        return;
    }

    /* We did not voted for a slave about this master for two
     * times the node timeout. This is not strictly needed for correctness
     * of the algorithm but makes the base case more linear. */
    if (mstime() - node->m_slaveof->m_voted_time < server.cluster_node_timeout * 2)
    {
        serverLog(LL_WARNING,
                "Failover auth denied to %.40s: "
                "can't vote about this master before %lld milliseconds",
                node->m_name,
                (long long) ((server.cluster_node_timeout*2)-
                             (mstime() - node->m_slaveof->m_voted_time)));
        return;
    }

    /* The slave requesting the vote must have a configEpoch for the claimed
     * slots that is >= the one of the masters currently serving the same
     * slots in the current configuration. */
    for (j = 0; j < CLUSTER_SLOTS; j++) {
        if (bitmapTestBit(claimed_slots, j) == 0) continue;
        if (server.cluster->m_slots[j] == NULL ||
            server.cluster->m_slots[j]->m_configEpoch <= requestConfigEpoch)
        {
            continue;
        }
        /* If we reached this point we found a slot that in our current slots
         * is served by a master with a greater configEpoch than the one claimed
         * by the slave requesting our vote. Refuse to vote for this slave. */
        serverLog(LL_WARNING,
                "Failover auth denied to %.40s: "
                "slot %d epoch (%llu) > reqEpoch (%llu)",
                node->m_name, j,
                (unsigned long long) server.cluster->m_slots[j]->m_configEpoch,
                (unsigned long long) requestConfigEpoch);
        return;
    }

    /* We can vote for this slave. */
    clusterSendFailoverAuth(node);
    server.cluster->m_lastVoteEpoch = server.cluster->m_currentEpoch;
    node->m_slaveof->m_voted_time = mstime();
    serverLog(LL_WARNING, "Failover auth granted to %.40s for epoch %llu",
        node->m_name, (unsigned long long) server.cluster->m_currentEpoch);
}

/* This function returns the "rank" of this instance, a slave, in the context
 * of its master-slaves ring. The rank of the slave is given by the number of
 * other slaves for the same master that have a better replication offset
 * compared to the local one (better means, greater, so they claim more data).
 *
 * A slave with rank 0 is the one with the greatest (most up to date)
 * replication offset, and so forth. Note that because how the rank is computed
 * multiple slaves may have the same rank, in case they have the same offset.
 *
 * The slave rank is used to add a delay to start an election in order to
 * get voted and replace a failing master. Slaves with better replication
 * offsets are more likely to win. */
int clusterGetSlaveRank(void) {
    long long myoffset;
    int j, rank = 0;
    clusterNode *master;

    serverAssert(myself->nodeIsSlave());
    master = myself->m_slaveof;
    if (master == NULL) return 0; /* Never called by slaves without master. */

    myoffset = replicationGetSlaveOffset();
    for (j = 0; j < master->m_numslaves; j++)
        if (master->m_slaves[j] != myself &&
            master->m_slaves[j]->m_repl_offset > myoffset) rank++;
    return rank;
}

/* This function is called by clusterHandleSlaveFailover() in order to
 * let the slave log why it is not able to failover. Sometimes there are
 * not the conditions, but since the failover function is called again and
 * again, we can't log the same things continuously.
 *
 * This function works by logging only if a given set of conditions are
 * true:
 *
 * 1) The reason for which the failover can't be initiated changed.
 *    The reasons also include a NONE reason we reset the state to
 *    when the slave finds that its master is fine (no FAIL flag).
 * 2) Also, the log is emitted again if the master is still down and
 *    the reason for not failing over is still the same, but more than
 *    CLUSTER_CANT_FAILOVER_RELOG_PERIOD seconds elapsed.
 * 3) Finally, the function only logs if the slave is down for more than
 *    five seconds + NODE_TIMEOUT. This way nothing is logged when a
 *    failover starts in a reasonable time.
 *
 * The function is called with the reason why the slave can't failover
 * which is one of the integer macros CLUSTER_CANT_FAILOVER_*.
 *
 * The function is guaranteed to be called only if 'myself' is a slave. */
void clusterLogCantFailover(int reason) {
    char *msg;
    static time_t lastlog_time = 0;
    mstime_t nolog_fail_time = server.cluster_node_timeout + 5000;

    /* Don't log if we have the same reason for some time. */
    if (reason == server.cluster->m_cant_failover_reason &&
        time(NULL)-lastlog_time < CLUSTER_CANT_FAILOVER_RELOG_PERIOD)
        return;

    server.cluster->m_cant_failover_reason = reason;

    /* We also don't emit any log if the master failed no long ago, the
     * goal of this function is to log slaves in a stalled condition for
     * a long time. */
    if (myself->m_slaveof &&
        myself->m_slaveof->nodeFailed() &&
        (mstime() - myself->m_slaveof->m_fail_time) < nolog_fail_time) return;

    switch(reason) {
    case CLUSTER_CANT_FAILOVER_DATA_AGE:
        msg = "Disconnected from master for longer than allowed. "
              "Please check the 'cluster-slave-validity-factor' configuration "
              "option.";
        break;
    case CLUSTER_CANT_FAILOVER_WAITING_DELAY:
        msg = "Waiting the delay before I can start a new failover.";
        break;
    case CLUSTER_CANT_FAILOVER_EXPIRED:
        msg = "Failover attempt expired.";
        break;
    case CLUSTER_CANT_FAILOVER_WAITING_VOTES:
        msg = "Waiting for votes, but majority still not reached.";
        break;
    default:
        msg = "Unknown reason code.";
        break;
    }
    lastlog_time = time(NULL);
    serverLog(LL_WARNING,"Currently unable to failover: %s", msg);
}

/* This function implements the final part of automatic and manual failovers,
 * where the slave grabs its master's hash slots, and propagates the new
 * configuration.
 *
 * Note that it's up to the caller to be sure that the node got a new
 * configuration epoch already. */
void clusterFailoverReplaceYourMaster(void) {
    int j;
    clusterNode *oldmaster = myself->m_slaveof;

    if (myself->nodeIsMaster() || oldmaster == NULL) return;

    /* 1) Turn this node into a master. */
    myself->clusterSetNodeAsMaster();
    replicationUnsetMaster();

    /* 2) Claim all the slots assigned to our master. */
    for (j = 0; j < CLUSTER_SLOTS; j++) {
        if (oldmaster->clusterNodeGetSlotBit(j)) {
            clusterDelSlot(j);
            myself->clusterAddSlot(j);
        }
    }

    /* 3) Update state and save config. */
    clusterUpdateState();
    clusterSaveConfigOrDie(1);

    /* 4) Pong all the other nodes so that they can update the state
     *    accordingly and detect that we switched to master role. */
    clusterBroadcastPong(CLUSTER_BROADCAST_ALL);

    /* 5) If there was a manual failover in progress, clear the state. */
    resetManualFailover();
}

/* This function is called if we are a slave node and our master serving
 * a non-zero amount of hash slots is in FAIL state.
 *
 * The gaol of this function is:
 * 1) To check if we are able to perform a failover, is our data updated?
 * 2) Try to get elected by masters.
 * 3) Perform the failover informing all the other nodes.
 */
void clusterHandleSlaveFailover(void) {
    mstime_t data_age;
    mstime_t auth_age = mstime() - server.cluster->m_failover_auth_time;
    int needed_quorum = (server.cluster->m_size / 2) + 1;
    int manual_failover = server.cluster->m_mf_end != 0 &&
                          server.cluster->m_mf_can_start;
    mstime_t auth_timeout, auth_retry_time;

    server.cluster->m_todo_before_sleep &= ~CLUSTER_TODO_HANDLE_FAILOVER;

    /* Compute the failover timeout (the max time we have to send votes
     * and wait for replies), and the failover retry time (the time to wait
     * before trying to get voted again).
     *
     * Timeout is MAX(NODE_TIMEOUT*2,2000) milliseconds.
     * Retry is two times the Timeout.
     */
    auth_timeout = server.cluster_node_timeout*2;
    if (auth_timeout < 2000) auth_timeout = 2000;
    auth_retry_time = auth_timeout*2;

    /* Pre conditions to run the function, that must be met both in case
     * of an automatic or manual failover:
     * 1) We are a slave.
     * 2) Our master is flagged as FAIL, or this is a manual failover.
     * 3) It is serving slots. */
    if (myself->nodeIsMaster() ||
        myself->m_slaveof == NULL ||
        (!myself->m_slaveof->nodeFailed() && !manual_failover) ||
        myself->m_slaveof->m_numslots == 0)
    {
        /* There are no reasons to failover, so we set the reason why we
         * are returning without failing over to NONE. */
        server.cluster->m_cant_failover_reason = CLUSTER_CANT_FAILOVER_NONE;
        return;
    }

    /* Set data_age to the number of seconds we are disconnected from
     * the master. */
    if (server.repl_state == REPL_STATE_CONNECTED) {
        data_age = (mstime_t)(server.unixtime - server.master->lastinteraction)
                   * 1000;
    } else {
        data_age = (mstime_t)(server.unixtime - server.repl_down_since) * 1000;
    }

    /* Remove the node timeout from the data age as it is fine that we are
     * disconnected from our master at least for the time it was down to be
     * flagged as FAIL, that's the baseline. */
    if (data_age > server.cluster_node_timeout)
        data_age -= server.cluster_node_timeout;

    /* Check if our data is recent enough according to the slave validity
     * factor configured by the user.
     *
     * Check bypassed for manual failovers. */
    if (server.cluster_slave_validity_factor &&
        data_age >
        (((mstime_t)server.repl_ping_slave_period * 1000) +
         (server.cluster_node_timeout * server.cluster_slave_validity_factor)))
    {
        if (!manual_failover) {
            clusterLogCantFailover(CLUSTER_CANT_FAILOVER_DATA_AGE);
            return;
        }
    }

    /* If the previous failover attempt timedout and the retry time has
     * elapsed, we can setup a new one. */
    if (auth_age > auth_retry_time) {
        server.cluster->m_failover_auth_time = mstime() +
            500 + /* Fixed delay of 500 milliseconds, let FAIL msg propagate. */
            random() % 500; /* Random delay between 0 and 500 milliseconds. */
        server.cluster->m_failover_auth_count = 0;
        server.cluster->m_failover_auth_sent = 0;
        server.cluster->m_failover_auth_rank = clusterGetSlaveRank();
        /* We add another delay that is proportional to the slave rank.
         * Specifically 1 second * rank. This way slaves that have a probably
         * less updated replication offset, are penalized. */
        server.cluster->m_failover_auth_time +=
            server.cluster->m_failover_auth_rank * 1000;
        /* However if this is a manual failover, no delay is needed. */
        if (server.cluster->m_mf_end) {
            server.cluster->m_failover_auth_time = mstime();
            server.cluster->m_failover_auth_rank = 0;
        }
        serverLog(LL_WARNING,
            "Start of election delayed for %lld milliseconds "
            "(rank #%d, offset %lld).",
            server.cluster->m_failover_auth_time - mstime(),
            server.cluster->m_failover_auth_rank,
            replicationGetSlaveOffset());
        /* Now that we have a scheduled election, broadcast our offset
         * to all the other slaves so that they'll updated their offsets
         * if our offset is better. */
        clusterBroadcastPong(CLUSTER_BROADCAST_LOCAL_SLAVES);
        return;
    }

    /* It is possible that we received more updated offsets from other
     * slaves for the same master since we computed our election delay.
     * Update the delay if our rank changed.
     *
     * Not performed if this is a manual failover. */
    if (server.cluster->m_failover_auth_sent == 0 &&
        server.cluster->m_mf_end == 0)
    {
        int newrank = clusterGetSlaveRank();
        if (newrank > server.cluster->m_failover_auth_rank) {
            long long added_delay =
                (newrank - server.cluster->m_failover_auth_rank) * 1000;
            server.cluster->m_failover_auth_time += added_delay;
            server.cluster->m_failover_auth_rank = newrank;
            serverLog(LL_WARNING,
                "Slave rank updated to #%d, added %lld milliseconds of delay.",
                newrank, added_delay);
        }
    }

    /* Return ASAP if we can't still start the election. */
    if (mstime() < server.cluster->m_failover_auth_time) {
        clusterLogCantFailover(CLUSTER_CANT_FAILOVER_WAITING_DELAY);
        return;
    }

    /* Return ASAP if the election is too old to be valid. */
    if (auth_age > auth_timeout) {
        clusterLogCantFailover(CLUSTER_CANT_FAILOVER_EXPIRED);
        return;
    }

    /* Ask for votes if needed. */
    if (server.cluster->m_failover_auth_sent == 0) {
        server.cluster->m_currentEpoch++;
        server.cluster->m_failover_auth_epoch = server.cluster->m_currentEpoch;
        serverLog(LL_WARNING,"Starting a failover election for epoch %llu.",
            (unsigned long long) server.cluster->m_currentEpoch);
        clusterRequestFailoverAuth();
        server.cluster->m_failover_auth_sent = 1;
        clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG|
                             CLUSTER_TODO_UPDATE_STATE|
                             CLUSTER_TODO_FSYNC_CONFIG);
        return; /* Wait for replies. */
    }

    /* Check if we reached the quorum. */
    if (server.cluster->m_failover_auth_count >= needed_quorum) {
        /* We have the quorum, we can finally failover the master. */

        serverLog(LL_WARNING,
            "Failover election won: I'm the new master.");

        /* Update my configEpoch to the epoch of the election. */
        if (myself->m_configEpoch < server.cluster->m_failover_auth_epoch) {
            myself->m_configEpoch = server.cluster->m_failover_auth_epoch;
            serverLog(LL_WARNING,
                "configEpoch set to %llu after successful failover",
                (unsigned long long) myself->m_configEpoch);
        }

        /* Take responsibility for the cluster slots. */
        clusterFailoverReplaceYourMaster();
    } else {
        clusterLogCantFailover(CLUSTER_CANT_FAILOVER_WAITING_VOTES);
    }
}

/* -----------------------------------------------------------------------------
 * CLUSTER slave migration
 *
 * Slave migration is the process that allows a slave of a master that is
 * already covered by at least another slave, to "migrate" to a master that
 * is orpaned, that is, left with no working slaves.
 * ------------------------------------------------------------------------- */

/* This function is responsible to decide if this replica should be migrated
 * to a different (orphaned) master. It is called by the clusterCron() function
 * only if:
 *
 * 1) We are a slave node.
 * 2) It was detected that there is at least one orphaned master in
 *    the cluster.
 * 3) We are a slave of one of the masters with the greatest number of
 *    slaves.
 *
 * This checks are performed by the caller since it requires to iterate
 * the nodes anyway, so we spend time into clusterHandleSlaveMigration()
 * if definitely needed.
 *
 * The fuction is called with a pre-computed max_slaves, that is the max
 * number of working (not in FAIL state) slaves for a single master.
 *
 * Additional conditions for migration are examined inside the function.
 */
void clusterHandleSlaveMigration(int max_slaves) {
    int j, okslaves = 0;
    clusterNode *mymaster = myself->m_slaveof, *target = NULL, *candidate = NULL;
    dictEntry *de;

    /* Step 1: Don't migrate if the cluster state is not ok. */
    if (server.cluster->m_state != CLUSTER_OK) return;

    /* Step 2: Don't migrate if my master will not be left with at least
     *         'migration-barrier' slaves after my migration. */
    if (mymaster == NULL) return;
    for (j = 0; j < mymaster->m_numslaves; j++)
        if (!mymaster->m_slaves[j]->nodeFailed() &&
            !mymaster->m_slaves[j]->nodeTimedOut()) okslaves++;
    if (okslaves <= server.cluster_migration_barrier) return;

    /* Step 3: Idenitfy a candidate for migration, and check if among the
     * masters with the greatest number of ok slaves, I'm the one with the
     * smallest node ID (the "candidate slave").
     *
     * Note: this means that eventually a replica migration will occurr
     * since slaves that are reachable again always have their FAIL flag
     * cleared, so eventually there must be a candidate. At the same time
     * this does not mean that there are no race conditions possible (two
     * slaves migrating at the same time), but this is unlikely to
     * happen, and harmless when happens. */
    candidate = myself;
    {
        dictIterator di(server.cluster->m_nodes, 1);
        while((de = di.dictNext()) != NULL) {
            clusterNode* node = (clusterNode*)de->dictGetVal();
            int okslaves = 0, is_orphaned = 1;

            /* We want to migrate only if this master is working, orphaned, and
             * used to have slaves or if failed over a master that had slaves
             * (MIGRATE_TO flag). This way we only migrate to instances that were
             * supposed to have replicas. */
            if (node->nodeIsSlave() || node->nodeFailed()) is_orphaned = 0;
            if (!(node->m_flags & CLUSTER_NODE_MIGRATE_TO)) is_orphaned = 0;

            /* Check number of working slaves. */
            if (node->nodeIsMaster()) okslaves = clusterCountNonFailingSlaves(node);
            if (okslaves > 0) is_orphaned = 0;

            if (is_orphaned) {
                if (!target && node->m_numslots > 0) target = node;

                /* Track the starting time of the orphaned condition for this
                 * master. */
                if (!node->m_orphaned_time) node->m_orphaned_time = mstime();
            } else {
                node->m_orphaned_time = 0;
            }

            /* Check if I'm the slave candidate for the migration: attached
             * to a master with the maximum number of slaves and with the smallest
             * node ID. */
            if (okslaves == max_slaves) {
                for (j = 0; j < node->m_numslaves; j++) {
                    if (memcmp(node->m_slaves[j]->m_name,
                               candidate->m_name,
                               CLUSTER_NAMELEN) < 0)
                    {
                        candidate = node->m_slaves[j];
                    }
                }
            }
        }
    }

    /* Step 4: perform the migration if there is a target, and if I'm the
     * candidate, but only if the master is continuously orphaned for a
     * couple of seconds, so that during failovers, we give some time to
     * the natural slaves of this instance to advertise their switch from
     * the old master to the new one. */
    if (target && candidate == myself &&
        (mstime()-target->m_orphaned_time) > CLUSTER_SLAVE_MIGRATION_DELAY)
    {
        serverLog(LL_WARNING,"Migrating to orphaned master %.40s",
            target->m_name);
        clusterSetMaster(target);
    }
}

/* -----------------------------------------------------------------------------
 * CLUSTER manual failover
 *
 * This are the important steps performed by slaves during a manual failover:
 * 1) User send CLUSTER FAILOVER command. The failover state is initialized
 *    setting mf_end to the millisecond unix time at which we'll abort the
 *    attempt.
 * 2) Slave sends a MFSTART message to the master requesting to pause clients
 *    for two times the manual failover timeout CLUSTER_MF_TIMEOUT.
 *    When master is paused for manual failover, it also starts to flag
 *    packets with CLUSTERMSG_FLAG0_PAUSED.
 * 3) Slave waits for master to send its replication offset flagged as PAUSED.
 * 4) If slave received the offset from the master, and its offset matches,
 *    mf_can_start is set to 1, and clusterHandleSlaveFailover() will perform
 *    the failover as usually, with the difference that the vote request
 *    will be modified to force masters to vote for a slave that has a
 *    working master.
 *
 * From the point of view of the master things are simpler: when a
 * PAUSE_CLIENTS packet is received the master sets mf_end as well and
 * the sender in mf_slave. During the time limit for the manual failover
 * the master will just send PINGs more often to this slave, flagged with
 * the PAUSED flag, so that the slave will set mf_master_offset when receiving
 * a packet from the master with this flag set.
 *
 * The gaol of the manual failover is to perform a fast failover without
 * data loss due to the asynchronous master-slave replication.
 * -------------------------------------------------------------------------- */

/* Reset the manual failover state. This works for both masters and slavesa
 * as all the state about manual failover is cleared.
 *
 * The function can be used both to initialize the manual failover state at
 * startup or to abort a manual failover in progress. */
void resetManualFailover() {
    if (server.cluster->m_mf_end && clientsArePaused()) {
        server.clients_pause_end_time = 0;
        clientsArePaused(); /* Just use the side effect of the function. */
    }
    server.cluster->m_mf_end = 0; /* No manual failover in progress. */
    server.cluster->m_mf_can_start = 0;
    server.cluster->m_mf_slave = NULL;
    server.cluster->m_mf_master_offset = 0;
}

/* If a manual failover timed out, abort it. */
void manualFailoverCheckTimeout(void) {
    if (server.cluster->m_mf_end && server.cluster->m_mf_end < mstime()) {
        serverLog(LL_WARNING,"Manual failover timed out.");
        resetManualFailover();
    }
}

/* This function is called from the cluster cron function in order to go
 * forward with a manual failover state machine. */
void clusterHandleManualFailover(void) {
    /* Return ASAP if no manual failover is in progress. */
    if (server.cluster->m_mf_end == 0) return;

    /* If mf_can_start is non-zero, the failover was already triggered so the
     * next steps are performed by clusterHandleSlaveFailover(). */
    if (server.cluster->m_mf_can_start) return;

    if (server.cluster->m_mf_master_offset == 0) return; /* Wait for offset... */

    if (server.cluster->m_mf_master_offset == replicationGetSlaveOffset()) {
        /* Our replication offset matches the master replication offset
         * announced after clients were paused. We can start the failover. */
        server.cluster->m_mf_can_start = 1;
        serverLog(LL_WARNING,
            "All master replication stream processed, "
            "manual failover can start.");
    }
}

/* -----------------------------------------------------------------------------
 * CLUSTER cron job
 * -------------------------------------------------------------------------- */

/* This is executed 10 times every second */
void clusterCron(void) {
    dictEntry *de;
    int update_state = 0;
    int orphaned_masters; /* How many masters there are without ok slaves. */
    int max_slaves; /* Max number of ok slaves for a single master. */
    int this_slaves; /* Number of ok slaves for our master (if we are slave). */
    mstime_t min_pong = 0, now = mstime();
    clusterNode *min_pong_node = NULL;
    static unsigned long long iteration = 0;
    mstime_t handshake_timeout;

    iteration++; /* Number of times this function was called so far. */

    /* We want to take myself->m_ip in sync with the cluster-announce-ip option.
     * The option can be set at runtime via CONFIG SET, so we periodically check
     * if the option changed to reflect this into myself->m_ip. */
    {
        static char *prev_ip = NULL;
        char *curr_ip = server.cluster_announce_ip;
        int changed = 0;

        if (prev_ip == NULL && curr_ip != NULL) changed = 1;
        if (prev_ip != NULL && curr_ip == NULL) changed = 1;
        if (prev_ip && curr_ip && strcmp(prev_ip,curr_ip)) changed = 1;

        if (changed) {
            prev_ip = curr_ip;
            if (prev_ip) prev_ip = zstrdup(prev_ip);

            if (curr_ip) {
                strncpy(myself->m_ip,server.cluster_announce_ip,NET_IP_STR_LEN);
                myself->m_ip[NET_IP_STR_LEN-1] = '\0';
            } else {
                myself->m_ip[0] = '\0'; /* Force autodetection. */
            }
        }
    }

    /* The handshake timeout is the time after which a handshake node that was
     * not turned into a normal node is removed from the nodes. Usually it is
     * just the NODE_TIMEOUT value, but when NODE_TIMEOUT is too small we use
     * the value of 1 second. */
    handshake_timeout = server.cluster_node_timeout;
    if (handshake_timeout < 1000) handshake_timeout = 1000;

    /* Check if we have disconnected nodes and re-establish the connection.
     * Also update a few stats while we are here, that can be used to make
     * better decisions in other part of the code. */
     {
        dictIterator di(server.cluster->m_nodes, 1);
        server.cluster->m_stats_pfail_nodes = 0;
        while((de = di.dictNext()) != NULL) {
            clusterNode* node = (clusterNode*)de->dictGetVal();

            /* Not interested in reconnecting the link with myself or nodes
             * for which we have no address. */
            if (node->m_flags & (CLUSTER_NODE_MYSELF|CLUSTER_NODE_NOADDR)) continue;

            if (node->m_flags & CLUSTER_NODE_PFAIL)
                server.cluster->m_stats_pfail_nodes++;

            /* A Node in HANDSHAKE state has a limited lifespan equal to the
             * configured node timeout. */
            if (node->nodeInHandshake() && now - node->m_ctime > handshake_timeout) {
                clusterDelNode(node);
                continue;
            }

            if (node->m_link == NULL) {
                int fd;
                mstime_t old_ping_sent;
                clusterLink *link;

                fd = anetTcpNonBlockBindConnect(server.neterr, node->m_ip,
                    node->m_cport, NET_FIRST_BIND_ADDR);
                if (fd == -1) {
                    /* We got a synchronous error from connect before
                     * clusterSendPing() had a chance to be called.
                     * If node->m_ping_sent is zero, failure detection can't work,
                     * so we claim we actually sent a ping now (that will
                     * be really sent as soon as the link is obtained). */
                    if (node->m_ping_sent == 0) node->m_ping_sent = mstime();
                    serverLog(LL_DEBUG, "Unable to connect to "
                        "Cluster Node [%s]:%d -> %s", node->m_ip,
                        node->m_cport, server.neterr);
                    continue;
                }
                link = createClusterLink(node, fd);
                node->m_link = link;
                server.el->aeCreateFileEvent(link->m_fd,AE_READABLE,
                        clusterReadHandler,link);
                /* Queue a PING in the new connection ASAP: this is crucial
                 * to avoid false positives in failure detection.
                 *
                 * If the node is flagged as MEET, we send a MEET message instead
                 * of a PING one, to force the receiver to add us in its node
                 * table. */
                old_ping_sent = node->m_ping_sent;
                clusterSendPing(link, node->m_flags & CLUSTER_NODE_MEET ?
                        CLUSTERMSG_TYPE_MEET : CLUSTERMSG_TYPE_PING);
                if (old_ping_sent) {
                    /* If there was an active ping before the link was
                     * disconnected, we want to restore the ping time, otherwise
                     * replaced by the clusterSendPing() call. */
                    node->m_ping_sent = old_ping_sent;
                }
                /* We can clear the flag after the first packet is sent.
                 * If we'll never receive a PONG, we'll never send new packets
                 * to this node. Instead after the PONG is received and we
                 * are no longer in meet/handshake status, we want to send
                 * normal PING packets. */
                node->m_flags &= ~CLUSTER_NODE_MEET;

                serverLog(LL_DEBUG,"Connecting with Node %.40s at %s:%d",
                        node->m_name, node->m_ip, node->m_cport);
            }
        }
    }

    /* Ping some random node 1 time every 10 iterations, so that we usually ping
     * one random node every second. */
    if (!(iteration % 10)) {
        int j;

        /* Check a few random nodes and ping the one with the oldest
         * pong_received time. */
        for (j = 0; j < 5; j++) {
            de = server.cluster->m_nodes->dictGetRandomKey();
            clusterNode* _this = (clusterNode*)de->dictGetVal();

            /* Don't ping nodes disconnected or with a ping currently active. */
            if (_this->m_link == NULL || _this->m_ping_sent != 0) continue;
            if (_this->m_flags & (CLUSTER_NODE_MYSELF|CLUSTER_NODE_HANDSHAKE))
                continue;
            if (min_pong_node == NULL || min_pong > _this->m_pong_received) {
                min_pong_node = _this;
                min_pong = _this->m_pong_received;
            }
        }
        if (min_pong_node) {
            serverLog(LL_DEBUG,"Pinging node %.40s", min_pong_node->m_name);
            clusterSendPing(min_pong_node->m_link, CLUSTERMSG_TYPE_PING);
        }
    }

    /* Iterate nodes to check if we need to flag something as failing.
     * This loop is also responsible to:
     * 1) Check if there are orphaned masters (masters without non failing
     *    slaves).
     * 2) Count the max number of non failing slaves for a single master.
     * 3) Count the number of slaves for our master, if we are a slave. */
    orphaned_masters = 0;
    max_slaves = 0;
    this_slaves = 0;
    {
        dictIterator di(server.cluster->m_nodes, 1);
        while((de = di.dictNext()) != NULL) {
            clusterNode* node = (clusterNode*)de->dictGetVal();
            now = mstime(); /* Use an updated time at every iteration. */
            mstime_t delay;

            if (node->m_flags &
                (CLUSTER_NODE_MYSELF|CLUSTER_NODE_NOADDR|CLUSTER_NODE_HANDSHAKE))
                    continue;

            /* Orphaned master check, useful only if the current instance
             * is a slave that may migrate to another master. */
            if (myself->nodeIsSlave() && node->nodeIsMaster() && !node->nodeFailed()) {
                int okslaves = clusterCountNonFailingSlaves(node);

                /* A master is orphaned if it is serving a non-zero number of
                 * slots, have no working slaves, but used to have at least one
                 * slave, or failed over a master that used to have slaves. */
                if (okslaves == 0 && node->m_numslots > 0 &&
                    node->m_flags & CLUSTER_NODE_MIGRATE_TO)
                {
                    orphaned_masters++;
                }
                if (okslaves > max_slaves) max_slaves = okslaves;
                if (myself->nodeIsSlave() && myself->m_slaveof == node)
                    this_slaves = okslaves;
            }

            /* If we are waiting for the PONG more than half the cluster
             * timeout, reconnect the link: maybe there is a connection
             * issue even if the node is alive. */
            if (node->m_link && /* is connected */
                now - node->m_link->m_ctime >
                server.cluster_node_timeout && /* was not already reconnected */
                node->m_ping_sent && /* we already sent a ping */
                node->m_pong_received < node->m_ping_sent && /* still waiting pong */
                /* and we are waiting for the pong more than timeout/2 */
                now - node->m_ping_sent > server.cluster_node_timeout/2)
            {
                /* Disconnect the link, it will be reconnected automatically. */
                freeClusterLink(node->m_link);
            }

            /* If we have currently no active ping in this instance, and the
             * received PONG is older than half the cluster timeout, send
             * a new ping now, to ensure all the nodes are pinged without
             * a too big delay. */
            if (node->m_link &&
                node->m_ping_sent == 0 &&
                (now - node->m_pong_received) > server.cluster_node_timeout/2)
            {
                clusterSendPing(node->m_link, CLUSTERMSG_TYPE_PING);
                continue;
            }

            /* If we are a master and one of the slaves requested a manual
             * failover, ping it continuously. */
            if (server.cluster->m_mf_end &&
                myself->nodeIsMaster() &&
                server.cluster->m_mf_slave == node &&
                node->m_link)
            {
                clusterSendPing(node->m_link, CLUSTERMSG_TYPE_PING);
                continue;
            }

            /* Check only if we have an active ping for this instance. */
            if (node->m_ping_sent == 0) continue;

            /* Compute the delay of the PONG. Note that if we already received
             * the PONG, then node->m_ping_sent is zero, so can't reach this
             * code at all. */
            delay = now - node->m_ping_sent;

            if (delay > server.cluster_node_timeout) {
                /* Timeout reached. Set the node as possibly failing if it is
                 * not already in this state. */
                if (!(node->m_flags & (CLUSTER_NODE_PFAIL|CLUSTER_NODE_FAIL))) {
                    serverLog(LL_DEBUG,"*** NODE %.40s possibly failing",
                        node->m_name);
                    node->m_flags |= CLUSTER_NODE_PFAIL;
                    update_state = 1;
                }
            }
        }
}

    /* If we are a slave node but the replication is still turned off,
     * enable it if we know the address of our master and it appears to
     * be up. */
    if (myself->nodeIsSlave() &&
        server.masterhost == NULL &&
        myself->m_slaveof &&
        myself->m_slaveof->nodeHasAddr())
    {
        replicationSetMaster(myself->m_slaveof->m_ip, myself->m_slaveof->m_port);
    }

    /* Abourt a manual failover if the timeout is reached. */
    manualFailoverCheckTimeout();

    if (myself->nodeIsSlave()) {
        clusterHandleManualFailover();
        clusterHandleSlaveFailover();
        /* If there are orphaned slaves, and we are a slave among the masters
         * with the max number of non-failing slaves, consider migrating to
         * the orphaned masters. Note that it does not make sense to try
         * a migration if there is no master with at least *two* working
         * slaves. */
        if (orphaned_masters && max_slaves >= 2 && this_slaves == max_slaves)
            clusterHandleSlaveMigration(max_slaves);
    }

    if (update_state || server.cluster->m_state == CLUSTER_FAIL)
        clusterUpdateState();
}

/* This function is called before the event handler returns to sleep for
 * events. It is useful to perform operations that must be done ASAP in
 * reaction to events fired but that are not safe to perform inside event
 * handlers, or to perform potentially expansive tasks that we need to do
 * a single time before replying to clients. */
void clusterBeforeSleep(void) {
    /* Handle failover, this is needed when it is likely that there is already
     * the quorum from masters in order to react fast. */
    if (server.cluster->m_todo_before_sleep & CLUSTER_TODO_HANDLE_FAILOVER)
        clusterHandleSlaveFailover();

    /* Update the cluster state. */
    if (server.cluster->m_todo_before_sleep & CLUSTER_TODO_UPDATE_STATE)
        clusterUpdateState();

    /* Save the config, possibly using fsync. */
    if (server.cluster->m_todo_before_sleep & CLUSTER_TODO_SAVE_CONFIG) {
        int fsync = server.cluster->m_todo_before_sleep &
                    CLUSTER_TODO_FSYNC_CONFIG;
        clusterSaveConfigOrDie(fsync);
    }

    /* Reset our flags (not strictly needed since every single function
     * called for flags set should be able to clear its flag). */
    server.cluster->m_todo_before_sleep = 0;
}

void clusterDoBeforeSleep(int flags) {
    server.cluster->m_todo_before_sleep |= flags;
}

/* -----------------------------------------------------------------------------
 * Slots management
 * -------------------------------------------------------------------------- */

/* Test bit 'pos' in a generic bitmap. Return 1 if the bit is set,
 * otherwise 0. */
int bitmapTestBit(unsigned char *bitmap, int pos) {
    off_t byte = pos/8;
    int bit = pos&7;
    return (bitmap[byte] & (1<<bit)) != 0;
}

/* Set the bit at position 'pos' in a bitmap. */
void bitmapSetBit(unsigned char *bitmap, int pos) {
    off_t byte = pos/8;
    int bit = pos&7;
    bitmap[byte] |= 1<<bit;
}

/* Clear the bit at position 'pos' in a bitmap. */
void bitmapClearBit(unsigned char *bitmap, int pos) {
    off_t byte = pos/8;
    int bit = pos&7;
    bitmap[byte] &= ~(1<<bit);
}

/* Return non-zero if there is at least one master with slaves in the cluster.
 * Otherwise zero is returned. Used by clusterNodeSetSlotBit() to set the
 * MIGRATE_TO flag the when a master gets the first slot. */
int clusterMastersHaveSlaves(void) {
    dictIterator di(server.cluster->m_nodes, 1);
    dictEntry *de;
    int slaves = 0;
    while((de = di.dictNext()) != NULL) {
        clusterNode* node = (clusterNode*)de->dictGetVal();

        if (node->nodeIsSlave()) continue;
        slaves += node->m_numslaves;
    }

    return slaves != 0;
}

/* Set the slot bit and return the old value. */
int clusterNode::clusterNodeSetSlotBit(int slot)
{
    int old = bitmapTestBit(m_slots,slot);
    bitmapSetBit(m_slots,slot);
    if (!old) {
        m_numslots++;
        /* When a master gets its first slot, even if it has no slaves,
         * it gets flagged with MIGRATE_TO, that is, the master is a valid
         * target for replicas migration, if and only if at least one of
         * the other masters has slaves right now.
         *
         * Normally masters are valid targerts of replica migration if:
         * 1. The used to have slaves (but no longer have).
         * 2. They are slaves failing over a master that used to have slaves.
         *
         * However new masters with slots assigned are considered valid
         * migration tagets if the rest of the cluster is not a slave-less.
         *
         * See https://github.com/antirez/redis/issues/3043 for more info. */
        if (m_numslots == 1 && clusterMastersHaveSlaves())
            m_flags |= CLUSTER_NODE_MIGRATE_TO;
    }
    return old;
}

/* Clear the slot bit and return the old value. */
int clusterNodeClearSlotBit(clusterNode *n, int slot) {
    int old = bitmapTestBit(n->m_slots,slot);
    bitmapClearBit(n->m_slots,slot);
    if (old) n->m_numslots--;
    return old;
}

/* Return the slot bit from the cluster node structure. */
int clusterNode::clusterNodeGetSlotBit(int slot) {
    return bitmapTestBit(m_slots,slot);
}

/* Add the specified slot to the list of slots that node 'n' will
 * serve. Return C_OK if the operation ended with success.
 * If the slot is already assigned to another instance this is considered
 * an error and C_ERR is returned. */
int clusterNode::clusterAddSlot(int slot) {
    if (server.cluster->m_slots[slot]) return C_ERR;
    clusterNodeSetSlotBit(slot);
    server.cluster->m_slots[slot] = this;
    return C_OK;
}

/* Delete the specified slot marking it as unassigned.
 * Returns C_OK if the slot was assigned, otherwise if the slot was
 * already unassigned C_ERR is returned. */
int clusterDelSlot(int slot) {
    clusterNode *n = server.cluster->m_slots[slot];

    if (!n) return C_ERR;
    serverAssert(clusterNodeClearSlotBit(n,slot) == 1);
    server.cluster->m_slots[slot] = NULL;
    return C_OK;
}

/* Delete all the slots associated with the specified node.
 * The number of deleted slots is returned. */
int clusterNode::clusterDelNodeSlots() {
    int deleted = 0, j;

    for (j = 0; j < CLUSTER_SLOTS; j++) {
        if (clusterNodeGetSlotBit(j)) {
            clusterDelSlot(j);
            deleted++;
        }
    }
    return deleted;
}

/* Clear the migrating / importing state for all the slots.
 * This is useful at initialization and when turning a master into slave. */
void clusterCloseAllSlots() {
    memset(server.cluster->m_migrating_slots_to,0,
        sizeof(server.cluster->m_migrating_slots_to));
    memset(server.cluster->m_importing_slots_from,0,
        sizeof(server.cluster->m_importing_slots_from));
}

/* -----------------------------------------------------------------------------
 * Cluster state evaluation function
 * -------------------------------------------------------------------------- */

/* The following are defines that are only used in the evaluation function
 * and are based on heuristics. Actaully the main point about the rejoin and
 * writable delay is that they should be a few orders of magnitude larger
 * than the network latency. */
#define CLUSTER_MAX_REJOIN_DELAY 5000
#define CLUSTER_MIN_REJOIN_DELAY 500
#define CLUSTER_WRITABLE_DELAY 2000

void clusterUpdateState(void) {
    int j, new_state;
    int reachable_masters = 0;
    static mstime_t among_minority_time;
    static mstime_t first_call_time = 0;

    server.cluster->m_todo_before_sleep &= ~CLUSTER_TODO_UPDATE_STATE;

    /* If this is a master node, wait some time before turning the state
     * into OK, since it is not a good idea to rejoin the cluster as a writable
     * master, after a reboot, without giving the cluster a chance to
     * reconfigure this node. Note that the delay is calculated starting from
     * the first call to this function and not since the server start, in order
     * to don't count the DB loading time. */
    if (first_call_time == 0) first_call_time = mstime();
    if (myself->nodeIsMaster() &&
        server.cluster->m_state == CLUSTER_FAIL &&
        mstime() - first_call_time < CLUSTER_WRITABLE_DELAY) return;

    /* Start assuming the state is OK. We'll turn it into FAIL if there
     * are the right conditions. */
    new_state = CLUSTER_OK;

    /* Check if all the slots are covered. */
    if (server.cluster_require_full_coverage) {
        for (j = 0; j < CLUSTER_SLOTS; j++) {
            if (server.cluster->m_slots[j] == NULL ||
                server.cluster->m_slots[j]->m_flags & (CLUSTER_NODE_FAIL))
            {
                new_state = CLUSTER_FAIL;
                break;
            }
        }
    }

    /* Compute the cluster size, that is the number of master nodes
     * serving at least a single slot.
     *
     * At the same time count the number of reachable masters having
     * at least one slot. */
    {
        dictEntry *de;

        server.cluster->m_size = 0;
        dictIterator di(server.cluster->m_nodes, 1);
        while((de = di.dictNext()) != NULL) {
            clusterNode* node = (clusterNode*)de->dictGetVal();

            if (node->nodeIsMaster() && node->m_numslots) {
                server.cluster->m_size++;
                if ((node->m_flags & (CLUSTER_NODE_FAIL|CLUSTER_NODE_PFAIL)) == 0)
                    reachable_masters++;
            }
        }
    }

    /* If we are in a minority partition, change the cluster state
     * to FAIL. */
    {
        int needed_quorum = (server.cluster->m_size / 2) + 1;

        if (reachable_masters < needed_quorum) {
            new_state = CLUSTER_FAIL;
            among_minority_time = mstime();
        }
    }

    /* Log a state change */
    if (new_state != server.cluster->m_state) {
        mstime_t rejoin_delay = server.cluster_node_timeout;

        /* If the instance is a master and was partitioned away with the
         * minority, don't let it accept queries for some time after the
         * partition heals, to make sure there is enough time to receive
         * a configuration update. */
        if (rejoin_delay > CLUSTER_MAX_REJOIN_DELAY)
            rejoin_delay = CLUSTER_MAX_REJOIN_DELAY;
        if (rejoin_delay < CLUSTER_MIN_REJOIN_DELAY)
            rejoin_delay = CLUSTER_MIN_REJOIN_DELAY;

        if (new_state == CLUSTER_OK &&
            myself->nodeIsMaster() &&
            mstime() - among_minority_time < rejoin_delay)
        {
            return;
        }

        /* Change the state and log the event. */
        serverLog(LL_WARNING,"Cluster state changed: %s",
            new_state == CLUSTER_OK ? "ok" : "fail");
        server.cluster->m_state = new_state;
    }
}

/* This function is called after the node startup in order to verify that data
 * loaded from disk is in agreement with the cluster configuration:
 *
 * 1) If we find keys about hash slots we have no responsibility for, the
 *    following happens:
 *    A) If no other node is in charge according to the current cluster
 *       configuration, we add these slots to our node.
 *    B) If according to our config other nodes are already in charge for
 *       this lots, we set the slots as IMPORTING from our point of view
 *       in order to justify we have those slots, and in order to make
 *       redis-trib aware of the issue, so that it can try to fix it.
 * 2) If we find data in a DB different than DB0 we return C_ERR to
 *    signal the caller it should quit the server with an error message
 *    or take other actions.
 *
 * The function always returns C_OK even if it will try to correct
 * the error described in "1". However if data is found in DB different
 * from DB0, C_ERR is returned.
 *
 * The function also uses the logging facility in order to warn the user
 * about desynchronizations between the data we have in memory and the
 * cluster configuration. */
int verifyClusterConfigWithData(void) {
    int j;
    int update_config = 0;

    /* If this node is a slave, don't perform the check at all as we
     * completely depend on the replication stream. */
    if (myself->nodeIsSlave()) return C_OK;

    /* Make sure we only have keys in DB0. */
    for (j = 1; j < server.dbnum; j++) {
        if (server.db[j].m_dict->dictSize()) return C_ERR;
    }

    /* Check that all the slots we see populated memory have a corresponding
     * entry in the cluster table. Otherwise fix the table. */
    for (j = 0; j < CLUSTER_SLOTS; j++) {
        if (!countKeysInSlot(j)) continue; /* No keys in this slot. */
        /* Check if we are assigned to this slot or if we are importing it.
         * In both cases check the next slot as the configuration makes
         * sense. */
        if (server.cluster->m_slots[j] == myself ||
            server.cluster->m_importing_slots_from[j] != NULL) continue;

        /* If we are here data and cluster config don't agree, and we have
         * slot 'j' populated even if we are not importing it, nor we are
         * assigned to this slot. Fix this condition. */

        update_config++;
        /* Case A: slot is unassigned. Take responsibility for it. */
        if (server.cluster->m_slots[j] == NULL) {
            serverLog(LL_WARNING, "I have keys for unassigned slot %d. "
                                    "Taking responsibility for it.",j);
            myself->clusterAddSlot(j);
        } else {
            serverLog(LL_WARNING, "I have keys for slot %d, but the slot is "
                                    "assigned to another node. "
                                    "Setting it to importing state.",j);
            server.cluster->m_importing_slots_from[j] = server.cluster->m_slots[j];
        }
    }
    if (update_config) clusterSaveConfigOrDie(1);
    return C_OK;
}

/* -----------------------------------------------------------------------------
 * SLAVE nodes handling
 * -------------------------------------------------------------------------- */

/* Set the specified node 'n' as master for this node.
 * If this node is currently a master, it is turned into a slave. */
void clusterSetMaster(clusterNode *n) {
    serverAssert(n != myself);
    serverAssert(myself->m_numslots == 0);

    if (myself->nodeIsMaster()) {
        myself->m_flags &= ~(CLUSTER_NODE_MASTER|CLUSTER_NODE_MIGRATE_TO);
        myself->m_flags |= CLUSTER_NODE_SLAVE;
        clusterCloseAllSlots();
    } else {
        if (myself->m_slaveof)
            clusterNodeRemoveSlave(myself->m_slaveof,myself);
    }
    myself->m_slaveof = n;
    n->clusterNodeAddSlave(myself);
    replicationSetMaster(n->m_ip, n->m_port);
    resetManualFailover();
}

/* -----------------------------------------------------------------------------
 * Nodes to string representation functions.
 * -------------------------------------------------------------------------- */

struct redisNodeFlags {
    uint16_t flag;
    char *name;
};

static struct redisNodeFlags redisNodeFlagsTable[] = {
    {CLUSTER_NODE_MYSELF,       "myself,"},
    {CLUSTER_NODE_MASTER,       "master,"},
    {CLUSTER_NODE_SLAVE,        "slave,"},
    {CLUSTER_NODE_PFAIL,        "fail?,"},
    {CLUSTER_NODE_FAIL,         "fail,"},
    {CLUSTER_NODE_HANDSHAKE,    "handshake,"},
    {CLUSTER_NODE_NOADDR,       "noaddr,"}
};

/* Concatenate the comma separated list of node flags to the given SDS
 * string 'ci'. */
sds representClusterNodeFlags(sds ci, uint16_t flags) {
    size_t orig_len = sdslen(ci);
    int i, size = sizeof(redisNodeFlagsTable)/sizeof(struct redisNodeFlags);
    for (i = 0; i < size; i++) {
        struct redisNodeFlags *nodeflag = redisNodeFlagsTable + i;
        if (flags & nodeflag->flag) ci = sdscat(ci, nodeflag->name);
    }
    /* If no flag was added, add the "noflags" special flag. */
    if (sdslen(ci) == orig_len) ci = sdscat(ci,"noflags,");
    sdsIncrLen(ci,-1); /* Remove trailing comma. */
    return ci;
}

/* Generate a csv-alike representation of the specified cluster node.
 * See clusterGenNodesDescription() top comment for more information.
 *
 * The function returns the string representation as an SDS string. */
sds clusterGenNodeDescription(clusterNode *node) {
    int j, start;
    sds ci;

    /* Node coordinates */
    ci = sdscatprintf(sdsempty(),"%.40s %s:%d@%d ",
        node->m_name,
        node->m_ip,
        node->m_port,
        node->m_cport);

    /* Flags */
    ci = representClusterNodeFlags(ci, node->m_flags);

    /* Slave of... or just "-" */
    if (node->m_slaveof)
        ci = sdscatprintf(ci," %.40s ",node->m_slaveof->m_name);
    else
        ci = sdscatlen(ci," - ",3);

    /* Latency from the POV of this node, config epoch, link status */
    ci = sdscatprintf(ci,"%lld %lld %llu %s",
        (long long) node->m_ping_sent,
        (long long) node->m_pong_received,
        (unsigned long long) node->m_configEpoch,
        (node->m_link || node->m_flags & CLUSTER_NODE_MYSELF) ?
                    "connected" : "disconnected");

    /* Slots served by this instance */
    start = -1;
    for (j = 0; j < CLUSTER_SLOTS; j++) {
        int bit;

        if ((bit = node->clusterNodeGetSlotBit(j)) != 0) {
            if (start == -1) start = j;
        }
        if (start != -1 && (!bit || j == CLUSTER_SLOTS-1)) {
            if (bit && j == CLUSTER_SLOTS-1) j++;

            if (start == j-1) {
                ci = sdscatprintf(ci," %d",start);
            } else {
                ci = sdscatprintf(ci," %d-%d",start,j-1);
            }
            start = -1;
        }
    }

    /* Just for MYSELF node we also dump info about slots that
     * we are migrating to other instances or importing from other
     * instances. */
    if (node->m_flags & CLUSTER_NODE_MYSELF) {
        for (j = 0; j < CLUSTER_SLOTS; j++) {
            if (server.cluster->m_migrating_slots_to[j]) {
                ci = sdscatprintf(ci," [%d->-%.40s]",j,
                    server.cluster->m_migrating_slots_to[j]->m_name);
            } else if (server.cluster->m_importing_slots_from[j]) {
                ci = sdscatprintf(ci," [%d-<-%.40s]",j,
                    server.cluster->m_importing_slots_from[j]->m_name);
            }
        }
    }
    return ci;
}

/* Generate a csv-alike representation of the nodes we are aware of,
 * including the "myself" node, and return an SDS string containing the
 * representation (it is up to the caller to free it).
 *
 * All the nodes matching at least one of the node flags specified in
 * "filter" are excluded from the output, so using zero as a filter will
 * include all the known nodes in the representation, including nodes in
 * the HANDSHAKE state.
 *
 * The representation obtained using this function is used for the output
 * of the CLUSTER NODES function, and as format for the cluster
 * configuration file (nodes.conf) for a given node. */
sds clusterGenNodesDescription(int filter) {
    sds ci = sdsempty(), ni;
    dictEntry *de;

    dictIterator di(server.cluster->m_nodes, 1);
    while((de = di.dictNext()) != NULL) {
        clusterNode* node = (clusterNode*)de->dictGetVal();

        if (node->m_flags & filter) continue;
        ni = clusterGenNodeDescription(node);
        ci = sdscatsds(ci,ni);
        sdsfree(ni);
        ci = sdscatlen(ci,"\n",1);
    }
    return ci;
}

/* -----------------------------------------------------------------------------
 * CLUSTER command
 * -------------------------------------------------------------------------- */

const char *clusterGetMessageTypeString(int type) {
    switch(type) {
    case CLUSTERMSG_TYPE_PING: return "ping";
    case CLUSTERMSG_TYPE_PONG: return "pong";
    case CLUSTERMSG_TYPE_MEET: return "meet";
    case CLUSTERMSG_TYPE_FAIL: return "fail";
    case CLUSTERMSG_TYPE_PUBLISH: return "publish";
    case CLUSTERMSG_TYPE_FAILOVER_AUTH_REQUEST: return "auth-req";
    case CLUSTERMSG_TYPE_FAILOVER_AUTH_ACK: return "auth-ack";
    case CLUSTERMSG_TYPE_UPDATE: return "update";
    case CLUSTERMSG_TYPE_MFSTART: return "mfstart";
    }
    return "unknown";
}

int getSlotOrReply(client *c, robj *o) {
    long long slot;

    if (getLongLongFromObject(o,&slot) != C_OK ||
        slot < 0 || slot >= CLUSTER_SLOTS)
    {
        addReplyError(c,"Invalid or out of range slot");
        return -1;
    }
    return (int) slot;
}

void clusterReplyMultiBulkSlots(client *c) {
    /* Format: 1) 1) start slot
     *            2) end slot
     *            3) 1) master IP
     *               2) master port
     *               3) node ID
     *            4) 1) replica IP
     *               2) replica port
     *               3) node ID
     *           ... continued until done
     */

    int num_masters = 0;
    void *slot_replylen = addDeferredMultiBulkLength(c);

    dictEntry *de;
    dictIterator di(server.cluster->m_nodes, 1);
    while((de = di.dictNext()) != NULL) {
        clusterNode* node = (clusterNode*)de->dictGetVal();
        int j = 0, start = -1;

        /* Skip slaves (that are iterated when producing the output of their
         * master) and  masters not serving any slot. */
        if (!node->nodeIsMaster() || node->m_numslots == 0) continue;

        for (j = 0; j < CLUSTER_SLOTS; j++) {
            int bit, i;

            if ((bit = node->clusterNodeGetSlotBit(j)) != 0) {
                if (start == -1) start = j;
            }
            if (start != -1 && (!bit || j == CLUSTER_SLOTS-1)) {
                int nested_elements = 3; /* slots (2) + master addr (1). */
                void *nested_replylen = addDeferredMultiBulkLength(c);

                if (bit && j == CLUSTER_SLOTS-1) j++;

                /* If slot exists in output map, add to it's list.
                 * else, create a new output map for this slot */
                if (start == j-1) {
                    addReplyLongLong(c, start); /* only one slot; low==high */
                    addReplyLongLong(c, start);
                } else {
                    addReplyLongLong(c, start); /* low */
                    addReplyLongLong(c, j-1);   /* high */
                }
                start = -1;

                /* First node reply position is always the master */
                addReplyMultiBulkLen(c, 3);
                addReplyBulkCString(c, node->m_ip);
                addReplyLongLong(c, node->m_port);
                addReplyBulkCBuffer(c, node->m_name, CLUSTER_NAMELEN);

                /* Remaining nodes in reply are replicas for slot range */
                for (i = 0; i < node->m_numslaves; i++) {
                    /* This loop is copy/pasted from clusterGenNodeDescription()
                     * with modifications for per-slot node aggregation */
                    if (node->m_slaves[i]->nodeFailed()) continue;
                    addReplyMultiBulkLen(c, 3);
                    addReplyBulkCString(c, node->m_slaves[i]->m_ip);
                    addReplyLongLong(c, node->m_slaves[i]->m_port);
                    addReplyBulkCBuffer(c, node->m_slaves[i]->m_name, CLUSTER_NAMELEN);
                    nested_elements++;
                }
                setDeferredMultiBulkLength(c, nested_replylen, nested_elements);
                num_masters++;
            }
        }
    }

    setDeferredMultiBulkLength(c, slot_replylen, num_masters);
}

void clusterCommand(client *c) {
    if (server.cluster_enabled == 0) {
        addReplyError(c,"This instance has cluster support disabled");
        return;
    }

    if (!strcasecmp((const char*)c->argv[1]->ptr,"meet") && (c->argc == 4 || c->argc == 5)) {
        /* CLUSTER MEET <ip> <port> [cport] */
        long long port, cport;

        if (getLongLongFromObject(c->argv[3], &port) != C_OK) {
            addReplyErrorFormat(c,"Invalid TCP base port specified: %s",
                                (char*)c->argv[3]->ptr);
            return;
        }

        if (c->argc == 5) {
            if (getLongLongFromObject(c->argv[4], &cport) != C_OK) {
                addReplyErrorFormat(c,"Invalid TCP bus port specified: %s",
                                    (char*)c->argv[4]->ptr);
                return;
            }
        } else {
            cport = port + CLUSTER_PORT_INCR;
        }

        if (clusterStartHandshake((char *)c->argv[2]->ptr,port,cport) == 0 &&
            errno == EINVAL)
        {
            addReplyErrorFormat(c,"Invalid node address specified: %s:%s",
                            (char*)c->argv[2]->ptr, (char*)c->argv[3]->ptr);
        } else {
            addReply(c,shared.ok);
        }
    } else if (!strcasecmp((const char*)c->argv[1]->ptr,"nodes") && c->argc == 2) {
        /* CLUSTER NODES */
        robj *o;
        sds ci = clusterGenNodesDescription(0);

        o = createObject(OBJ_STRING,ci);
        addReplyBulk(c,o);
        decrRefCount(o);
    } else if (!strcasecmp((const char*)c->argv[1]->ptr,"myid") && c->argc == 2) {
        /* CLUSTER MYID */
        addReplyBulkCBuffer(c,myself->m_name, CLUSTER_NAMELEN);
    } else if (!strcasecmp((const char*)c->argv[1]->ptr,"slots") && c->argc == 2) {
        /* CLUSTER SLOTS */
        clusterReplyMultiBulkSlots(c);
    } else if (!strcasecmp((const char*)c->argv[1]->ptr,"flushslots") && c->argc == 2) {
        /* CLUSTER FLUSHSLOTS */
        if (server.db[0].m_dict->dictSize() != 0) {
            addReplyError(c,"DB must be empty to perform CLUSTER FLUSHSLOTS.");
            return;
        }
        myself->clusterDelNodeSlots();
        clusterDoBeforeSleep(CLUSTER_TODO_UPDATE_STATE|CLUSTER_TODO_SAVE_CONFIG);
        addReply(c,shared.ok);
    } else if ((!strcasecmp((const char*)c->argv[1]->ptr,"addslots") ||
               !strcasecmp((const char*)c->argv[1]->ptr,"delslots")) && c->argc >= 3)
    {
        /* CLUSTER ADDSLOTS <slot> [slot] ... */
        /* CLUSTER DELSLOTS <slot> [slot] ... */
        int j, slot;
        unsigned char* slots = (unsigned char*)zmalloc(CLUSTER_SLOTS);
        int del = !strcasecmp((const char*)c->argv[1]->ptr,"delslots");

        memset(slots,0,CLUSTER_SLOTS);
        /* Check that all the arguments are parseable and that all the
         * slots are not already busy. */
        for (j = 2; j < c->argc; j++) {
            if ((slot = getSlotOrReply(c,c->argv[j])) == -1) {
                zfree(slots);
                return;
            }
            if (del && server.cluster->m_slots[slot] == NULL) {
                addReplyErrorFormat(c,"Slot %d is already unassigned", slot);
                zfree(slots);
                return;
            } else if (!del && server.cluster->m_slots[slot]) {
                addReplyErrorFormat(c,"Slot %d is already busy", slot);
                zfree(slots);
                return;
            }
            if (slots[slot]++ == 1) {
                addReplyErrorFormat(c,"Slot %d specified multiple times",
                    (int)slot);
                zfree(slots);
                return;
            }
        }
        for (j = 0; j < CLUSTER_SLOTS; j++) {
            if (slots[j]) {
                int retval;

                /* If this slot was set as importing we can clear this
                 * state as now we are the real owner of the slot. */
                if (server.cluster->m_importing_slots_from[j])
                    server.cluster->m_importing_slots_from[j] = NULL;

                retval = del ? clusterDelSlot(j) :
                               myself->clusterAddSlot(j);
                serverAssertWithInfo(c,NULL,retval == C_OK);
            }
        }
        zfree(slots);
        clusterDoBeforeSleep(CLUSTER_TODO_UPDATE_STATE|CLUSTER_TODO_SAVE_CONFIG);
        addReply(c,shared.ok);
    } else if (!strcasecmp((const char*)c->argv[1]->ptr,"setslot") && c->argc >= 4) {
        /* SETSLOT 10 MIGRATING <node ID> */
        /* SETSLOT 10 IMPORTING <node ID> */
        /* SETSLOT 10 STABLE */
        /* SETSLOT 10 NODE <node ID> */
        int slot;
        clusterNode *n;

        if (myself->nodeIsSlave()) {
            addReplyError(c,"Please use SETSLOT only with masters.");
            return;
        }

        if ((slot = getSlotOrReply(c,c->argv[2])) == -1) return;

        if (!strcasecmp((const char*)c->argv[3]->ptr,"migrating") && c->argc == 5) {
            if (server.cluster->m_slots[slot] != myself) {
                addReplyErrorFormat(c,"I'm not the owner of hash slot %u",slot);
                return;
            }
            if ((n = clusterLookupNode((const char *)c->argv[4]->ptr)) == NULL) {
                addReplyErrorFormat(c,"I don't know about node %s",
                    (char*)c->argv[4]->ptr);
                return;
            }
            server.cluster->m_migrating_slots_to[slot] = n;
        } else if (!strcasecmp((const char*)c->argv[3]->ptr,"importing") && c->argc == 5) {
            if (server.cluster->m_slots[slot] == myself) {
                addReplyErrorFormat(c,
                    "I'm already the owner of hash slot %u",slot);
                return;
            }
            if ((n = clusterLookupNode((const char *)c->argv[4]->ptr)) == NULL) {
                addReplyErrorFormat(c,"I don't know about node %s",
                    (char*)c->argv[4]->ptr);
                return;
            }
            server.cluster->m_importing_slots_from[slot] = n;
        } else if (!strcasecmp((const char*)c->argv[3]->ptr,"stable") && c->argc == 4) {
            /* CLUSTER SETSLOT <SLOT> STABLE */
            server.cluster->m_importing_slots_from[slot] = NULL;
            server.cluster->m_migrating_slots_to[slot] = NULL;
        } else if (!strcasecmp((const char*)c->argv[3]->ptr,"node") && c->argc == 5) {
            /* CLUSTER SETSLOT <SLOT> NODE <NODE ID> */
            clusterNode *n = clusterLookupNode((const char *)c->argv[4]->ptr);

            if (!n) {
                addReplyErrorFormat(c,"Unknown node %s",
                    (char*)c->argv[4]->ptr);
                return;
            }
            /* If this hash slot was served by 'myself' before to switch
             * make sure there are no longer local keys for this hash slot. */
            if (server.cluster->m_slots[slot] == myself && n != myself) {
                if (countKeysInSlot(slot) != 0) {
                    addReplyErrorFormat(c,
                        "Can't assign hashslot %d to a different node "
                        "while I still hold keys for this hash slot.", slot);
                    return;
                }
            }
            /* If this slot is in migrating status but we have no keys
             * for it assigning the slot to another node will clear
             * the migratig status. */
            if (countKeysInSlot(slot) == 0 &&
                server.cluster->m_migrating_slots_to[slot])
                server.cluster->m_migrating_slots_to[slot] = NULL;

            /* If this node was importing this slot, assigning the slot to
             * itself also clears the importing status. */
            if (n == myself &&
                server.cluster->m_importing_slots_from[slot])
            {
                /* This slot was manually migrated, set this node configEpoch
                 * to a new epoch so that the new version can be propagated
                 * by the cluster.
                 *
                 * Note that if this ever results in a collision with another
                 * node getting the same configEpoch, for example because a
                 * failover happens at the same time we close the slot, the
                 * configEpoch collision resolution will fix it assigning
                 * a different epoch to each node. */
                if (clusterBumpConfigEpochWithoutConsensus() == C_OK) {
                    serverLog(LL_WARNING,
                        "configEpoch updated after importing slot %d", slot);
                }
                server.cluster->m_importing_slots_from[slot] = NULL;
            }
            clusterDelSlot(slot);
            n->clusterAddSlot(slot);
        } else {
            addReplyError(c,
                "Invalid CLUSTER SETSLOT action or number of arguments");
            return;
        }
        clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG|CLUSTER_TODO_UPDATE_STATE);
        addReply(c,shared.ok);
    } else if (!strcasecmp((const char*)c->argv[1]->ptr,"bumpepoch") && c->argc == 2) {
        /* CLUSTER BUMPEPOCH */
        int retval = clusterBumpConfigEpochWithoutConsensus();
        sds reply = sdscatprintf(sdsempty(),"+%s %llu\r\n",
                (retval == C_OK) ? "BUMPED" : "STILL",
                (unsigned long long) myself->m_configEpoch);
        addReplySds(c,reply);
    } else if (!strcasecmp((const char*)c->argv[1]->ptr,"info") && c->argc == 2) {
        /* CLUSTER INFO */
        char *statestr[] = {"ok","fail","needhelp"};
        int slots_assigned = 0, slots_ok = 0, slots_pfail = 0, slots_fail = 0;
        uint64_t myepoch;
        int j;

        for (j = 0; j < CLUSTER_SLOTS; j++) {
            clusterNode *n = server.cluster->m_slots[j];

            if (n == NULL) continue;
            slots_assigned++;
            if (n->nodeFailed()) {
                slots_fail++;
            } else if (n->nodeTimedOut()) {
                slots_pfail++;
            } else {
                slots_ok++;
            }
        }

        myepoch = (myself->nodeIsSlave() && myself->m_slaveof) ?
                  myself->m_slaveof->m_configEpoch : myself->m_configEpoch;

        sds info = sdscatprintf(sdsempty(),
            "cluster_state:%s\r\n"
            "cluster_slots_assigned:%d\r\n"
            "cluster_slots_ok:%d\r\n"
            "cluster_slots_pfail:%d\r\n"
            "cluster_slots_fail:%d\r\n"
            "cluster_known_nodes:%lu\r\n"
            "cluster_size:%d\r\n"
            "cluster_current_epoch:%llu\r\n"
            "cluster_my_epoch:%llu\r\n"
            , statestr[server.cluster->m_state],
            slots_assigned,
            slots_ok,
            slots_pfail,
            slots_fail,
            server.cluster->m_nodes->dictSize(),
            server.cluster->m_size,
            (unsigned long long) server.cluster->m_currentEpoch,
            (unsigned long long) myepoch
        );

        /* Show stats about messages sent and received. */
        long long tot_msg_sent = 0;
        long long tot_msg_received = 0;

        for (int i = 0; i < CLUSTERMSG_TYPE_COUNT; i++) {
            if (server.cluster->m_stats_bus_messages_sent[i] == 0) continue;
            tot_msg_sent += server.cluster->m_stats_bus_messages_sent[i];
            info = sdscatprintf(info,
                "cluster_stats_messages_%s_sent:%lld\r\n",
                clusterGetMessageTypeString(i),
                server.cluster->m_stats_bus_messages_sent[i]);
        }
        info = sdscatprintf(info,
            "cluster_stats_messages_sent:%lld\r\n", tot_msg_sent);

        for (int i = 0; i < CLUSTERMSG_TYPE_COUNT; i++) {
            if (server.cluster->m_stats_bus_messages_received[i] == 0) continue;
            tot_msg_received += server.cluster->m_stats_bus_messages_received[i];
            info = sdscatprintf(info,
                "cluster_stats_messages_%s_received:%lld\r\n",
                clusterGetMessageTypeString(i),
                server.cluster->m_stats_bus_messages_received[i]);
        }
        info = sdscatprintf(info,
            "cluster_stats_messages_received:%lld\r\n", tot_msg_received);

        /* Produce the reply protocol. */
        addReplySds(c,sdscatprintf(sdsempty(),"$%lu\r\n",
            (unsigned long)sdslen(info)));
        addReplySds(c,info);
        addReply(c,shared.crlf);
    } else if (!strcasecmp((const char*)c->argv[1]->ptr,"saveconfig") && c->argc == 2) {
        int retval = clusterSaveConfig(1);

        if (retval == 0)
            addReply(c,shared.ok);
        else
            addReplyErrorFormat(c,"error saving the cluster node config: %s",
                strerror(errno));
    } else if (!strcasecmp((const char*)c->argv[1]->ptr,"keyslot") && c->argc == 3) {
        /* CLUSTER KEYSLOT <key> */
        sds key = (sds)c->argv[2]->ptr;

        addReplyLongLong(c,keyHashSlot(key,sdslen(key)));
    } else if (!strcasecmp((const char*)c->argv[1]->ptr,"countkeysinslot") && c->argc == 3) {
        /* CLUSTER COUNTKEYSINSLOT <slot> */
        long long slot;

        if (getLongLongFromObjectOrReply(c,c->argv[2],&slot,NULL) != C_OK)
            return;
        if (slot < 0 || slot >= CLUSTER_SLOTS) {
            addReplyError(c,"Invalid slot");
            return;
        }
        addReplyLongLong(c,countKeysInSlot(slot));
    } else if (!strcasecmp((const char*)c->argv[1]->ptr,"getkeysinslot") && c->argc == 4) {
        /* CLUSTER GETKEYSINSLOT <slot> <count> */
        long long maxkeys, slot;
        unsigned int numkeys, j;
        robj **keys;

        if (getLongLongFromObjectOrReply(c,c->argv[2],&slot,NULL) != C_OK)
            return;
        if (getLongLongFromObjectOrReply(c,c->argv[3],&maxkeys,NULL)
            != C_OK)
            return;
        if (slot < 0 || slot >= CLUSTER_SLOTS || maxkeys < 0) {
            addReplyError(c,"Invalid slot or number of keys");
            return;
        }

        /* Avoid allocating more than needed in case of large COUNT argument
         * and smaller actual number of keys. */
        unsigned int keys_in_slot = countKeysInSlot(slot);
        if (maxkeys > keys_in_slot) maxkeys = keys_in_slot;

        keys = (robj **)zmalloc(sizeof(robj*)*maxkeys);
        numkeys = getKeysInSlot(slot, keys, maxkeys);
        addReplyMultiBulkLen(c,numkeys);
        for (j = 0; j < numkeys; j++) {
            addReplyBulk(c,keys[j]);
            decrRefCount(keys[j]);
        }
        zfree(keys);
    } else if (!strcasecmp((const char*)c->argv[1]->ptr,"forget") && c->argc == 3) {
        /* CLUSTER FORGET <NODE ID> */
        clusterNode *n = clusterLookupNode((const char *)c->argv[2]->ptr);

        if (!n) {
            addReplyErrorFormat(c,"Unknown node %s", (char*)c->argv[2]->ptr);
            return;
        } else if (n == myself) {
            addReplyError(c,"I tried hard but I can't forget myself...");
            return;
        } else if (myself->nodeIsSlave() && myself->m_slaveof == n) {
            addReplyError(c,"Can't forget my master!");
            return;
        }
        clusterBlacklistAddNode(n);
        clusterDelNode(n);
        clusterDoBeforeSleep(CLUSTER_TODO_UPDATE_STATE|
                             CLUSTER_TODO_SAVE_CONFIG);
        addReply(c,shared.ok);
    } else if (!strcasecmp((const char*)c->argv[1]->ptr,"replicate") && c->argc == 3) {
        /* CLUSTER REPLICATE <NODE ID> */
        clusterNode *n = clusterLookupNode((const char *)c->argv[2]->ptr);

        /* Lookup the specified node in our table. */
        if (!n) {
            addReplyErrorFormat(c,"Unknown node %s", (char*)c->argv[2]->ptr);
            return;
        }

        /* I can't replicate myself. */
        if (n == myself) {
            addReplyError(c,"Can't replicate myself");
            return;
        }

        /* Can't replicate a slave. */
        if (n->nodeIsSlave()) {
            addReplyError(c,"I can only replicate a master, not a slave.");
            return;
        }

        /* If the instance is currently a master, it should have no assigned
         * slots nor keys to accept to replicate some other node.
         * Slaves can switch to another master without issues. */
        if (myself->nodeIsMaster() &&
            (myself->m_numslots != 0 || server.db[0].m_dict->dictSize() != 0)) {
            addReplyError(c,
                "To set a master the node must be empty and "
                "without assigned slots.");
            return;
        }

        /* Set the master. */
        clusterSetMaster(n);
        clusterDoBeforeSleep(CLUSTER_TODO_UPDATE_STATE|CLUSTER_TODO_SAVE_CONFIG);
        addReply(c,shared.ok);
    } else if (!strcasecmp((const char*)c->argv[1]->ptr,"slaves") && c->argc == 3) {
        /* CLUSTER SLAVES <NODE ID> */
        clusterNode *n = clusterLookupNode((const char *)c->argv[2]->ptr);
        int j;

        /* Lookup the specified node in our table. */
        if (!n) {
            addReplyErrorFormat(c,"Unknown node %s", (char*)c->argv[2]->ptr);
            return;
        }

        if (n->nodeIsSlave()) {
            addReplyError(c,"The specified node is not a master");
            return;
        }

        addReplyMultiBulkLen(c,n->m_numslaves);
        for (j = 0; j < n->m_numslaves; j++) {
            sds ni = clusterGenNodeDescription(n->m_slaves[j]);
            addReplyBulkCString(c,ni);
            sdsfree(ni);
        }
    } else if (!strcasecmp((const char*)c->argv[1]->ptr,"count-failure-reports") &&
               c->argc == 3)
    {
        /* CLUSTER COUNT-FAILURE-REPORTS <NODE ID> */
        clusterNode *n = clusterLookupNode((const char *)c->argv[2]->ptr);

        if (!n) {
            addReplyErrorFormat(c,"Unknown node %s", (char*)c->argv[2]->ptr);
            return;
        } else {
            addReplyLongLong(c,clusterNodeFailureReportsCount(n));
        }
    } else if (!strcasecmp((const char*)c->argv[1]->ptr,"failover") &&
               (c->argc == 2 || c->argc == 3))
    {
        /* CLUSTER FAILOVER [FORCE|TAKEOVER] */
        int force = 0, takeover = 0;

        if (c->argc == 3) {
            if (!strcasecmp((const char*)c->argv[2]->ptr,"force")) {
                force = 1;
            } else if (!strcasecmp((const char*)c->argv[2]->ptr,"takeover")) {
                takeover = 1;
                force = 1; /* Takeover also implies force. */
            } else {
                addReply(c,shared.syntaxerr);
                return;
            }
        }

        /* Check preconditions. */
        if (myself->nodeIsMaster()) {
            addReplyError(c,"You should send CLUSTER FAILOVER to a slave");
            return;
        } else if (myself->m_slaveof == NULL) {
            addReplyError(c,"I'm a slave but my master is unknown to me");
            return;
        } else if (!force &&
                   (myself->m_slaveof->nodeFailed() ||
                    myself->m_slaveof->m_link == NULL))
        {
            addReplyError(c,"Master is down or failed, "
                            "please use CLUSTER FAILOVER FORCE");
            return;
        }
        resetManualFailover();
        server.cluster->m_mf_end = mstime() + CLUSTER_MF_TIMEOUT;

        if (takeover) {
            /* A takeover does not perform any initial check. It just
             * generates a new configuration epoch for this node without
             * consensus, claims the master's slots, and broadcast the new
             * configuration. */
            serverLog(LL_WARNING,"Taking over the master (user request).");
            clusterBumpConfigEpochWithoutConsensus();
            clusterFailoverReplaceYourMaster();
        } else if (force) {
            /* If this is a forced failover, we don't need to talk with our
             * master to agree about the offset. We just failover taking over
             * it without coordination. */
            serverLog(LL_WARNING,"Forced failover user request accepted.");
            server.cluster->m_mf_can_start = 1;
        } else {
            serverLog(LL_WARNING,"Manual failover user request accepted.");
            clusterSendMFStart(myself->m_slaveof);
        }
        addReply(c,shared.ok);
    } else if (!strcasecmp((const char*)c->argv[1]->ptr,"set-config-epoch") && c->argc == 3)
    {
        /* CLUSTER SET-CONFIG-EPOCH <epoch>
         *
         * The user is allowed to set the config epoch only when a node is
         * totally fresh: no config epoch, no other known node, and so forth.
         * This happens at cluster creation time to start with a cluster where
         * every node has a different node ID, without to rely on the conflicts
         * resolution system which is too slow when a big cluster is created. */
        long long epoch;

        if (getLongLongFromObjectOrReply(c,c->argv[2],&epoch,NULL) != C_OK)
            return;

        if (epoch < 0) {
            addReplyErrorFormat(c,"Invalid config epoch specified: %lld",epoch);
        } else if (server.cluster->m_nodes->dictSize() > 1) {
            addReplyError(c,"The user can assign a config epoch only when the "
                            "node does not know any other node.");
        } else if (myself->m_configEpoch != 0) {
            addReplyError(c,"Node config epoch is already non-zero");
        } else {
            myself->m_configEpoch = epoch;
            serverLog(LL_WARNING,
                "configEpoch set to %llu via CLUSTER SET-CONFIG-EPOCH",
                (unsigned long long) myself->m_configEpoch);

            if (server.cluster->m_currentEpoch < (uint64_t)epoch)
                server.cluster->m_currentEpoch = epoch;
            /* No need to fsync the config here since in the unlucky event
             * of a failure to persist the config, the conflict resolution code
             * will assign an unique config to this node. */
            clusterDoBeforeSleep(CLUSTER_TODO_UPDATE_STATE|
                                 CLUSTER_TODO_SAVE_CONFIG);
            addReply(c,shared.ok);
        }
    } else if (!strcasecmp((const char*)c->argv[1]->ptr,"reset") &&
               (c->argc == 2 || c->argc == 3))
    {
        /* CLUSTER RESET [SOFT|HARD] */
        int hard = 0;

        /* Parse soft/hard argument. Default is soft. */
        if (c->argc == 3) {
            if (!strcasecmp((const char*)c->argv[2]->ptr,"hard")) {
                hard = 1;
            } else if (!strcasecmp((const char*)c->argv[2]->ptr,"soft")) {
                hard = 0;
            } else {
                addReply(c,shared.syntaxerr);
                return;
            }
        }

        /* Slaves can be reset while containing data, but not master nodes
         * that must be empty. */
        if (myself->nodeIsMaster() && c->db->m_dict->dictSize() != 0) {
            addReplyError(c,"CLUSTER RESET can't be called with "
                            "master nodes containing keys");
            return;
        }
        clusterReset(hard);
        addReply(c,shared.ok);
    } else {
        addReplyError(c,"Wrong CLUSTER subcommand or number of arguments");
    }
}

/* -----------------------------------------------------------------------------
 * DUMP, RESTORE and MIGRATE commands
 * -------------------------------------------------------------------------- */

/* Generates a DUMP-format representation of the object 'o', adding it to the
 * io stream pointed by 'rio'. This function can't fail. */
void createDumpPayload(rioBufferIO *payload, robj *o) {
    unsigned char buf[2];
    uint64_t crc;

    /* Serialize the object in a RDB-like format. It consist of an object type
     * byte followed by the serialized object. This is understood by RESTORE. */
    serverAssert(rdbSaveObjectType(payload,o));
    serverAssert(rdbSaveObject(payload,o));

    /* Write the footer, this is how it looks like:
     * ----------------+---------------------+---------------+
     * ... RDB payload | 2 bytes RDB version | 8 bytes CRC64 |
     * ----------------+---------------------+---------------+
     * RDB version and CRC are both in little endian.
     */

    /* RDB version */
    buf[0] = RDB_VERSION & 0xff;
    buf[1] = (RDB_VERSION >> 8) & 0xff;
    payload->m_ptr = sdscatlen(payload->m_ptr,buf,2);

    /* CRC64 */
    crc = crc64(0,(unsigned char*)payload->m_ptr,
                sdslen(payload->m_ptr));
    memrev64ifbe(&crc);
    payload->m_ptr = sdscatlen(payload->m_ptr,&crc,8);
}

/* Verify that the RDB version of the dump payload matches the one of this Redis
 * instance and that the checksum is ok.
 * If the DUMP payload looks valid C_OK is returned, otherwise C_ERR
 * is returned. */
int verifyDumpPayload(unsigned char *p, size_t len) {
    unsigned char *footer;
    uint16_t rdbver;
    uint64_t crc;

    /* At least 2 bytes of RDB version and 8 of CRC64 should be present. */
    if (len < 10) return C_ERR;
    footer = p+(len-10);

    /* Verify RDB version */
    rdbver = (footer[1] << 8) | footer[0];
    if (rdbver > RDB_VERSION) return C_ERR;

    /* Verify CRC64 */
    crc = crc64(0,p,len-8);
    memrev64ifbe(&crc);
    return (memcmp(&crc,footer+2,8) == 0) ? C_OK : C_ERR;
}

/* DUMP keyname
 * DUMP is actually not used by Redis Cluster but it is the obvious
 * complement of RESTORE and can be useful for different applications. */
void dumpCommand(client *c) {
    robj *o, *dumpobj;

    /* Check if the key is here. */
    if ((o = lookupKeyRead(c->db,c->argv[1])) == NULL) {
        addReply(c,shared.nullbulk);
        return;
    }

    /* Create the DUMP encoded representation. */
    rioBufferIO payload(sdsempty());
    createDumpPayload(&payload, o);

    /* Transfer to the client */
    dumpobj = createObject(OBJ_STRING, payload.m_ptr);
    addReplyBulk(c,dumpobj);
    decrRefCount(dumpobj);
    return;
}

/* RESTORE key ttl serialized-value [REPLACE] */
void restoreCommand(client *c) {
    long long ttl;
    int j, type, replace = 0;
    robj *obj;

    /* Parse additional options */
    for (j = 4; j < c->argc; j++) {
        if (!strcasecmp((const char*)c->argv[j]->ptr,"replace")) {
            replace = 1;
        } else {
            addReply(c,shared.syntaxerr);
            return;
        }
    }

    /* Make sure this key does not already exist here... */
    if (!replace && lookupKeyWrite(c->db,c->argv[1]) != NULL) {
        addReply(c,shared.busykeyerr);
        return;
    }

    /* Check if the TTL value makes sense */
    if (getLongLongFromObjectOrReply(c,c->argv[2],&ttl,NULL) != C_OK) {
        return;
    } else if (ttl < 0) {
        addReplyError(c,"Invalid TTL value, must be >= 0");
        return;
    }

    /* Verify RDB version and data checksum. */
    if (verifyDumpPayload((unsigned char *)c->argv[3]->ptr,sdslen((sds)c->argv[3]->ptr)) == C_ERR)
    {
        addReplyError(c,"DUMP payload version or checksum are wrong");
        return;
    }

    rioBufferIO payload((sds)c->argv[3]->ptr);

    if (((type = rdbLoadObjectType(&payload)) == -1) ||
        ((obj = rdbLoadObject(type,&payload)) == NULL))
    {
        addReplyError(c,"Bad data format");
        return;
    }

    /* Remove the old key if needed. */
    if (replace) dbDelete(c->db,c->argv[1]);

    /* Create the key and set the TTL if any */
    dbAdd(c->db,c->argv[1],obj);
    if (ttl) setExpire(c,c->db,c->argv[1],mstime()+ttl);
    signalModifiedKey(c->db,c->argv[1]);
    addReply(c,shared.ok);
    server.dirty++;
}

/* MIGRATE socket cache implementation.
 *
 * We take a map between host:ip and a TCP socket that we used to connect
 * to this instance in recent time.
 * This sockets are closed when the max number we cache is reached, and also
 * in serverCron() when they are around for more than a few seconds. */
#define MIGRATE_SOCKET_CACHE_ITEMS 64 /* max num of items in the cache. */
#define MIGRATE_SOCKET_CACHE_TTL 10 /* close cached sockets after 10 sec. */

typedef struct migrateCachedSocket {
    int fd;
    long last_dbid;
    time_t last_use_time;
} migrateCachedSocket;

/* Return a migrateCachedSocket containing a TCP socket connected with the
 * target instance, possibly returning a cached one.
 *
 * This function is responsible of sending errors to the client if a
 * connection can't be established. In this case -1 is returned.
 * Otherwise on success the socket is returned, and the caller should not
 * attempt to free it after usage.
 *
 * If the caller detects an error while using the socket, migrateCloseSocket()
 * should be called so that the connection will be created from scratch
 * the next time. */
migrateCachedSocket* migrateGetSocket(client *c, robj *host, robj *port, long timeout) {
    int fd;
    sds name = sdsempty();
    migrateCachedSocket *cs;

    /* Check if we have an already cached socket for this ip:port pair. */
    name = sdscatlen(name,host->ptr,sdslen((sds)host->ptr));
    name = sdscatlen(name,":",1);
    name = sdscatlen(name,port->ptr,sdslen((sds)port->ptr));
    cs = (migrateCachedSocket *)server.migrate_cached_sockets->dictFetchValue(name);
    if (cs) {
        sdsfree(name);
        cs->last_use_time = server.unixtime;
        return cs;
    }

    /* No cached socket, create one. */
    if (server.migrate_cached_sockets->dictSize() == MIGRATE_SOCKET_CACHE_ITEMS) {
        /* Too many items, drop one at random. */
        dictEntry *de = server.cluster->m_nodes->dictGetRandomKey();
        cs = (migrateCachedSocket *)de->dictGetVal();
        close(cs->fd);
        zfree(cs);
        server.migrate_cached_sockets->dictDelete(de->dictGetKey());
    }

    /* Create the socket */
    fd = anetTcpNonBlockConnect((char*)server.neterr, (char*)c->argv[1]->ptr,
                                atoi((const char*)c->argv[2]->ptr));
    if (fd == -1) {
        sdsfree(name);
        addReplyErrorFormat(c,"Can't connect to target node: %s",
            server.neterr);
        return NULL;
    }
    anetEnableTcpNoDelay(server.neterr,fd);

    /* Check if it connects within the specified timeout. */
    if ((aeWait(fd,AE_WRITABLE,timeout) & AE_WRITABLE) == 0) {
        sdsfree(name);
        addReplySds(c,
            sdsnew("-IOERR error or timeout connecting to the client\r\n"));
        close(fd);
        return NULL;
    }

    /* Add to the cache and return it to the caller. */
    cs = (migrateCachedSocket *)zmalloc(sizeof(*cs));
    cs->fd = fd;
    cs->last_dbid = -1;
    cs->last_use_time = server.unixtime;
    server.migrate_cached_sockets->dictAdd(name,cs);
    return cs;
}

/* Free a migrate cached connection. */
void migrateCloseSocket(robj *host, robj *port) {
    sds name = sdsempty();
    migrateCachedSocket *cs;

    name = sdscatlen(name,host->ptr,sdslen((sds)host->ptr));
    name = sdscatlen(name,":",1);
    name = sdscatlen(name,port->ptr,sdslen((sds)port->ptr));
    cs = (migrateCachedSocket *)server.migrate_cached_sockets->dictFetchValue(name);
    if (!cs) {
        sdsfree(name);
        return;
    }

    close(cs->fd);
    zfree(cs);
    server.migrate_cached_sockets->dictDelete(name);
    sdsfree(name);
}

void migrateCloseTimedoutSockets(void) {
    dictIterator di(server.migrate_cached_sockets, 1);
    dictEntry *de;

    while((de = di.dictNext()) != NULL) {
        migrateCachedSocket* cs = (migrateCachedSocket*)de->dictGetVal();

        if ((server.unixtime - cs->last_use_time) > MIGRATE_SOCKET_CACHE_TTL) {
            close(cs->fd);
            zfree(cs);
            server.migrate_cached_sockets->dictDelete(de->dictGetKey());
        }
    }
}

/* MIGRATE host port key dbid timeout [COPY | REPLACE]
 *
 * On in the multiple keys form:
 *
 * MIGRATE host port "" dbid timeout [COPY | REPLACE] KEYS key1 key2 ... keyN */
void migrateCommand(client *c) {
    migrateCachedSocket *cs;
    int copy, replace, j;
    long timeout;
    long dbid;
    robj **ov = NULL; /* Objects to migrate. */
    robj **kv = NULL; /* Key names. */
    robj **newargv = NULL; /* Used to rewrite the command as DEL ... keys ... */
    int may_retry = 1;
    int write_error = 0;
    int argv_rewritten = 0;

    /* To support the KEYS option we need the following additional state. */
    int first_key = 3; /* Argument index of the first key. */
    int num_keys = 1;  /* By default only migrate the 'key' argument. */

    /* Initialization */
    copy = 0;
    replace = 0;
    
    int error_from_target = 0;
    int socket_error = 0;
    int del_idx = 1; /* Index of the key argument for the replicated DEL op. */

    /* Parse additional options */
    for (j = 6; j < c->argc; j++) {
        if (!strcasecmp((const char*)c->argv[j]->ptr,"copy")) {
            copy = 1;
        } else if (!strcasecmp((const char*)c->argv[j]->ptr,"replace")) {
            replace = 1;
        } else if (!strcasecmp((const char*)c->argv[j]->ptr,"keys")) {
            if (sdslen((sds)c->argv[3]->ptr) != 0) {
                addReplyError(c,
                    "When using MIGRATE KEYS option, the key argument"
                    " must be set to the empty string");
                return;
            }
            first_key = j+1;
            num_keys = c->argc - j - 1;
            break; /* All the remaining args are keys. */
        } else {
            addReply(c,shared.syntaxerr);
            return;
        }
    }

    /* Sanity check */
    if (getLongFromObjectOrReply(c,c->argv[5],&timeout,NULL) != C_OK ||
        getLongFromObjectOrReply(c,c->argv[4],&dbid,NULL) != C_OK)
    {
        return;
    }
    if (timeout <= 0) timeout = 1000;

    /* Check if the keys are here. If at least one key is to migrate, do it
     * otherwise if all the keys are missing reply with "NOKEY" to signal
     * the caller there was nothing to migrate. We don't return an error in
     * this case, since often this is due to a normal condition like the key
     * expiring in the meantime. */
    ov = (robj **)zrealloc(ov,sizeof(robj*)*num_keys);
    kv = (robj **)zrealloc(kv,sizeof(robj*)*num_keys);
    int oi = 0;

    for (j = 0; j < num_keys; j++) {
        if ((ov[oi] = lookupKeyRead(c->db,c->argv[first_key+j])) != NULL) {
            kv[oi] = c->argv[first_key+j];
            oi++;
        }
    }
    num_keys = oi;
    if (num_keys == 0) {
        zfree(ov); zfree(kv);
        addReplySds(c,sdsnew("+NOKEY\r\n"));
        return;
    }

try_again:
    write_error = 0;

    /* Connect */
    cs = migrateGetSocket(c,c->argv[1],c->argv[2],timeout);
    if (cs == NULL) {
        zfree(ov); zfree(kv);
        return; /* error sent to the client by migrateGetSocket() */
    }
    rioBufferIO cmd(sdsempty());

    /* Send the SELECT command if the current DB is not already selected. */
    int select = cs->last_dbid != dbid; /* Should we emit SELECT? */
    if (select) {
        serverAssertWithInfo(c,NULL,cmd.rioWriteBulkCount('*',2));
        serverAssertWithInfo(c,NULL,cmd.rioWriteBulkString("SELECT",6));
        serverAssertWithInfo(c,NULL,cmd.rioWriteBulkLongLong(dbid));
    }

    /* Create RESTORE payload and generate the protocol to call the command. */
    for (j = 0; j < num_keys; j++) {
        long long ttl = 0;
        long long expireat = getExpire(c->db,kv[j]);

        if (expireat != -1) {
            ttl = expireat-mstime();
            if (ttl < 1) ttl = 1;
        }
        serverAssertWithInfo(c,NULL,cmd.rioWriteBulkCount('*',replace ? 5 : 4));
        if (server.cluster_enabled)
            serverAssertWithInfo(c,NULL,
                cmd.rioWriteBulkString("RESTORE-ASKING",14));
        else
            serverAssertWithInfo(c,NULL,cmd.rioWriteBulkString("RESTORE",7));
        serverAssertWithInfo(c,NULL,sdsEncodedObject(kv[j]));
        serverAssertWithInfo(c,NULL,cmd.rioWriteBulkString((char*)kv[j]->ptr,
                sdslen((sds)kv[j]->ptr)));
        serverAssertWithInfo(c,NULL,cmd.rioWriteBulkLongLong(ttl));

        /* Emit the payload argument, that is the serialized object using
         * the DUMP format. */
        rioBufferIO payload(sdsempty());
        createDumpPayload(&payload, ov[j]);

        serverAssertWithInfo(c,NULL,
            cmd.rioWriteBulkString(payload.m_ptr,
                               sdslen(payload.m_ptr)));
        sdsfree(payload.m_ptr);

        /* Add the REPLACE option to the RESTORE command if it was specified
         * as a MIGRATE option. */
        if (replace)
            serverAssertWithInfo(c, NULL, cmd.rioWriteBulkString("REPLACE",7));
    }

    /* Transfer the query to the other node in 64K chunks. */
    errno = 0;
    {
        sds buf = cmd.m_ptr;
        size_t pos = 0, towrite;
        int nwritten = 0;

        while ((towrite = sdslen(buf)-pos) > 0) {
            towrite = (towrite > (64*1024) ? (64*1024) : towrite);
            nwritten = syncWrite(cs->fd, buf+pos, towrite, timeout);
            if (nwritten != (signed)towrite) {
                write_error = 1;
                goto socket_err;
            }
            pos += nwritten;
        }
    }

    char buf1[1024]; /* Select reply. */
    char buf2[1024]; /* Restore reply. */

    /* Read the SELECT reply if needed. */
    if (select && syncReadLine(cs->fd, buf1, sizeof(buf1), timeout) <= 0)
        goto socket_err;

    /* Read the RESTORE replies. */

    if (!copy) newargv = (robj **)zmalloc(sizeof(robj*)*(num_keys+1));

    for (j = 0; j < num_keys; j++) {
        if (syncReadLine(cs->fd, buf2, sizeof(buf2), timeout) <= 0) {
            socket_error = 1;
            break;
        }
        if ((select && buf1[0] == '-') || buf2[0] == '-') {
            /* On error assume that last_dbid is no longer valid. */
            if (!error_from_target) {
                cs->last_dbid = -1;
                addReplyErrorFormat(c,"Target instance replied with error: %s",
                    (select && buf1[0] == '-') ? buf1+1 : buf2+1);
                error_from_target = 1;
            }
        } else {
            if (!copy) {
                /* No COPY option: remove the local key, signal the change. */
                dbDelete(c->db,kv[j]);
                signalModifiedKey(c->db,kv[j]);
                server.dirty++;

                /* Populate the argument vector to replace the old one. */
                newargv[del_idx++] = kv[j];
                incrRefCount(kv[j]);
            }
        }
    }

    /* On socket error, if we want to retry, do it now before rewriting the
     * command vector. We only retry if we are sure nothing was processed
     * and we failed to read the first reply (j == 0 test). */
    if (!error_from_target && socket_error && j == 0 && may_retry &&
        errno != ETIMEDOUT)
    {
        goto socket_err; /* A retry is guaranteed because of tested conditions.*/
    }

    /* On socket errors, close the migration socket now that we still have
     * the original host/port in the ARGV. Later the original command may be
     * rewritten to DEL and will be too later. */
    if (socket_error) migrateCloseSocket(c->argv[1],c->argv[2]);

    if (!copy) {
        /* Translate MIGRATE as DEL for replication/AOF. Note that we do
         * this only for the keys for which we received an acknowledgement
         * from the receiving Redis server, by using the del_idx index. */
        if (del_idx > 1) {
            newargv[0] = createStringObject("DEL",3);
            /* Note that the following call takes ownership of newargv. */
            replaceClientCommandVector(c,del_idx,newargv);
            argv_rewritten = 1;
        } else {
            /* No key transfer acknowledged, no need to rewrite as DEL. */
            zfree(newargv);
        }
        newargv = NULL; /* Make it safe to call zfree() on it in the future. */
    }

    /* If we are here and a socket error happened, we don't want to retry.
     * Just signal the problem to the client, but only do it if we did not
     * already queue a different error reported by the destination server. */
    if (!error_from_target && socket_error) {
        may_retry = 0;
        goto socket_err;
    }

    if (!error_from_target) {
        /* Success! Update the last_dbid in migrateCachedSocket, so that we can
         * avoid SELECT the next time if the target DB is the same. Reply +OK.
         *
         * Note: If we reached this point, even if socket_error is true
         * still the SELECT command succeeded (otherwise the code jumps to
         * socket_err label. */
        cs->last_dbid = dbid;
        addReply(c,shared.ok);
    } else {
        /* On error we already sent it in the for loop above, and set
         * the curretly selected socket to -1 to force SELECT the next time. */
    }

    sdsfree(cmd.m_ptr);
    zfree(ov); zfree(kv); zfree(newargv);
    return;

/* On socket errors we try to close the cached socket and try again.
 * It is very common for the cached socket to get closed, if just reopening
 * it works it's a shame to notify the error to the caller. */
socket_err:
    /* Cleanup we want to perform in both the retry and no retry case.
     * Note: Closing the migrate socket will also force SELECT next time. */
    sdsfree(cmd.m_ptr);

    /* If the command was rewritten as DEL and there was a socket error,
     * we already closed the socket earlier. While migrateCloseSocket()
     * is idempotent, the host/port arguments are now gone, so don't do it
     * again. */
    if (!argv_rewritten) migrateCloseSocket(c->argv[1],c->argv[2]);
    zfree(newargv);
    newargv = NULL; /* This will get reallocated on retry. */

    /* Retry only if it's not a timeout and we never attempted a retry
     * (or the code jumping here did not set may_retry to zero). */
    if (errno != ETIMEDOUT && may_retry) {
        may_retry = 0;
        goto try_again;
    }

    /* Cleanup we want to do if no retry is attempted. */
    zfree(ov); zfree(kv);
    addReplySds(c,
        sdscatprintf(sdsempty(),
            "-IOERR error or timeout %s to target instance\r\n",
            write_error ? "writing" : "reading"));
    return;
}

/* -----------------------------------------------------------------------------
 * Cluster functions related to serving / redirecting clients
 * -------------------------------------------------------------------------- */

/* The ASKING command is required after a -ASK redirection.
 * The client should issue ASKING before to actually send the command to
 * the target instance. See the Redis Cluster specification for more
 * information. */
void askingCommand(client *c) {
    if (server.cluster_enabled == 0) {
        addReplyError(c,"This instance has cluster support disabled");
        return;
    }
    c->m_flags |= CLIENT_ASKING;
    addReply(c,shared.ok);
}

/* The READONLY command is used by clients to enter the read-only mode.
 * In this mode slaves will not redirect clients as long as clients access
 * with read-only commands to keys that are served by the slave's master. */
void readonlyCommand(client *c) {
    if (server.cluster_enabled == 0) {
        addReplyError(c,"This instance has cluster support disabled");
        return;
    }
    c->m_flags |= CLIENT_READONLY;
    addReply(c,shared.ok);
}

/* The READWRITE command just clears the READONLY command state. */
void readwriteCommand(client *c) {
    c->m_flags &= ~CLIENT_READONLY;
    addReply(c,shared.ok);
}

/* Return the pointer to the cluster node that is able to serve the command.
 * For the function to succeed the command should only target either:
 *
 * 1) A single key (even multiple times like LPOPRPUSH mylist mylist).
 * 2) Multiple keys in the same hash slot, while the slot is stable (no
 *    resharding in progress).
 *
 * On success the function returns the node that is able to serve the request.
 * If the node is not 'myself' a redirection must be perfomed. The kind of
 * redirection is specified setting the integer passed by reference
 * 'error_code', which will be set to CLUSTER_REDIR_ASK or
 * CLUSTER_REDIR_MOVED.
 *
 * When the node is 'myself' 'error_code' is set to CLUSTER_REDIR_NONE.
 *
 * If the command fails NULL is returned, and the reason of the failure is
 * provided via 'error_code', which will be set to:
 *
 * CLUSTER_REDIR_CROSS_SLOT if the request contains multiple keys that
 * don't belong to the same hash slot.
 *
 * CLUSTER_REDIR_UNSTABLE if the request contains multiple keys
 * belonging to the same slot, but the slot is not stable (in migration or
 * importing state, likely because a resharding is in progress).
 *
 * CLUSTER_REDIR_DOWN_UNBOUND if the request addresses a slot which is
 * not bound to any node. In this case the cluster global state should be
 * already "down" but it is fragile to rely on the update of the global state,
 * so we also handle it here.
 *
 * CLUSTER_REDIR_DOWN_STATE if the cluster is down but the user attempts to
 * execute a command that addresses one or more keys. */
clusterNode *getNodeByQuery(client *c, struct redisCommand *cmd, robj **argv, int argc, int *hashslot, int *error_code) {
    clusterNode *n = NULL;
    robj *firstkey = NULL;
    int multiple_keys = 0;
    multiState *ms, _ms;
    multiCmd mc;
    int i, slot = 0, migrating_slot = 0, importing_slot = 0, missing_keys = 0;

    /* Set error code optimistically for the base case. */
    if (error_code) *error_code = CLUSTER_REDIR_NONE;

    /* We handle all the cases as if they were EXEC commands, so we have
     * a common code path for everything */
    if (cmd->proc == execCommand) {
        /* If CLIENT_MULTI flag is not set EXEC is just going to return an
         * error. */
        if (!(c->m_flags & CLIENT_MULTI)) return myself;
        ms = &c->mstate;
    } else {
        /* In order to have a single codepath create a fake Multi State
         * structure if the client is not in MULTI/EXEC state, this way
         * we have a single codepath below. */
        ms = &_ms;
        _ms.m_commands = &mc;
        _ms.m_count = 1;
        mc.argv = argv;
        mc.argc = argc;
        mc.cmd = cmd;
    }

    /* Check that all the keys are in the same hash slot, and obtain this
     * slot and the node associated. */
    for (i = 0; i < ms->m_count; i++) {
        struct redisCommand *mcmd;
        robj **margv;
        int margc, *keyindex, numkeys, j;

        mcmd = ms->m_commands[i].cmd;
        margc = ms->m_commands[i].argc;
        margv = ms->m_commands[i].argv;

        keyindex = getKeysFromCommand(mcmd,margv,margc,&numkeys);
        for (j = 0; j < numkeys; j++) {
            robj *thiskey = margv[keyindex[j]];
            int thisslot = keyHashSlot((char*)thiskey->ptr,
                                       sdslen((sds)thiskey->ptr));

            if (firstkey == NULL) {
                /* This is the first key we see. Check what is the slot
                 * and node. */
                firstkey = thiskey;
                slot = thisslot;
                n = server.cluster->m_slots[slot];

                /* Error: If a slot is not served, we are in "cluster down"
                 * state. However the state is yet to be updated, so this was
                 * not trapped earlier in processCommand(). Report the same
                 * error to the client. */
                if (n == NULL) {
                    getKeysFreeResult(keyindex);
                    if (error_code)
                        *error_code = CLUSTER_REDIR_DOWN_UNBOUND;
                    return NULL;
                }

                /* If we are migrating or importing this slot, we need to check
                 * if we have all the keys in the request (the only way we
                 * can safely serve the request, otherwise we return a TRYAGAIN
                 * error). To do so we set the importing/migrating state and
                 * increment a counter for every missing key. */
                if (n == myself &&
                    server.cluster->m_migrating_slots_to[slot] != NULL)
                {
                    migrating_slot = 1;
                } else if (server.cluster->m_importing_slots_from[slot] != NULL) {
                    importing_slot = 1;
                }
            } else {
                /* If it is not the first key, make sure it is exactly
                 * the same key as the first we saw. */
                if (!equalStringObjects(firstkey,thiskey)) {
                    if (slot != thisslot) {
                        /* Error: multiple keys from different slots. */
                        getKeysFreeResult(keyindex);
                        if (error_code)
                            *error_code = CLUSTER_REDIR_CROSS_SLOT;
                        return NULL;
                    } else {
                        /* Flag this request as one with multiple different
                         * keys. */
                        multiple_keys = 1;
                    }
                }
            }

            /* Migarting / Improrting slot? Count keys we don't have. */
            if ((migrating_slot || importing_slot) &&
                lookupKeyRead(&server.db[0],thiskey) == NULL)
            {
                missing_keys++;
            }
        }
        getKeysFreeResult(keyindex);
    }

    /* No key at all in command? then we can serve the request
     * without redirections or errors in all the cases. */
    if (n == NULL) return myself;

    /* Cluster is globally down but we got keys? We can't serve the request. */
    if (server.cluster->m_state != CLUSTER_OK) {
        if (error_code) *error_code = CLUSTER_REDIR_DOWN_STATE;
        return NULL;
    }

    /* Return the hashslot by reference. */
    if (hashslot) *hashslot = slot;

    /* MIGRATE always works in the context of the local node if the slot
     * is open (migrating or importing state). We need to be able to freely
     * move keys among instances in this case. */
    if ((migrating_slot || importing_slot) && cmd->proc == migrateCommand)
        return myself;

    /* If we don't have all the keys and we are migrating the slot, send
     * an ASK redirection. */
    if (migrating_slot && missing_keys) {
        if (error_code) *error_code = CLUSTER_REDIR_ASK;
        return server.cluster->m_migrating_slots_to[slot];
    }

    /* If we are receiving the slot, and the client correctly flagged the
     * request as "ASKING", we can serve the request. However if the request
     * involves multiple keys and we don't have them all, the only option is
     * to send a TRYAGAIN error. */
    if (importing_slot &&
        (c->m_flags & CLIENT_ASKING || cmd->m_flags & CMD_ASKING))
    {
        if (multiple_keys && missing_keys) {
            if (error_code) *error_code = CLUSTER_REDIR_UNSTABLE;
            return NULL;
        } else {
            return myself;
        }
    }

    /* Handle the read-only client case reading from a slave: if this
     * node is a slave and the request is about an hash slot our master
     * is serving, we can reply without redirection. */
    if (c->m_flags & CLIENT_READONLY &&
        cmd->m_flags & CMD_READONLY &&
        myself->nodeIsSlave() &&
        myself->m_slaveof == n)
    {
        return myself;
    }

    /* Base case: just return the right node. However if this node is not
     * myself, set error_code to MOVED since we need to issue a rediretion. */
    if (n != myself && error_code) *error_code = CLUSTER_REDIR_MOVED;
    return n;
}

/* Send the client the right redirection code, according to error_code
 * that should be set to one of CLUSTER_REDIR_* macros.
 *
 * If CLUSTER_REDIR_ASK or CLUSTER_REDIR_MOVED error codes
 * are used, then the node 'n' should not be NULL, but should be the
 * node we want to mention in the redirection. Moreover hashslot should
 * be set to the hash slot that caused the redirection. */
void clusterRedirectClient(client *c, clusterNode *n, int hashslot, int error_code) {
    if (error_code == CLUSTER_REDIR_CROSS_SLOT) {
        addReplySds(c,sdsnew("-CROSSSLOT Keys in request don't hash to the same slot\r\n"));
    } else if (error_code == CLUSTER_REDIR_UNSTABLE) {
        /* The request spawns mutliple keys in the same slot,
         * but the slot is not "stable" currently as there is
         * a migration or import in progress. */
        addReplySds(c,sdsnew("-TRYAGAIN Multiple keys request during rehashing of slot\r\n"));
    } else if (error_code == CLUSTER_REDIR_DOWN_STATE) {
        addReplySds(c,sdsnew("-CLUSTERDOWN The cluster is down\r\n"));
    } else if (error_code == CLUSTER_REDIR_DOWN_UNBOUND) {
        addReplySds(c,sdsnew("-CLUSTERDOWN Hash slot not served\r\n"));
    } else if (error_code == CLUSTER_REDIR_MOVED ||
               error_code == CLUSTER_REDIR_ASK)
    {
        addReplySds(c,sdscatprintf(sdsempty(),
            "-%s %d %s:%d\r\n",
            (error_code == CLUSTER_REDIR_ASK) ? "ASK" : "MOVED",
            hashslot,n->m_ip,n->m_port));
    } else {
        serverPanic("getNodeByQuery() unknown error.");
    }
}

/* This function is called by the function processing clients incrementally
 * to detect timeouts, in order to handle the following case:
 *
 * 1) A client blocks with BLPOP or similar blocking operation.
 * 2) The master migrates the hash slot elsewhere or turns into a slave.
 * 3) The client may remain blocked forever (or up to the max timeout time)
 *    waiting for a key change that will never happen.
 *
 * If the client is found to be blocked into an hash slot this node no
 * longer handles, the client is sent a redirection error, and the function
 * returns 1. Otherwise 0 is returned and no operation is performed. */
int clusterRedirectBlockedClientIfNeeded(client *c) {
    if (c->m_flags & CLIENT_BLOCKED && c->btype == BLOCKED_LIST) {
        dictEntry *de;

        /* If the cluster is down, unblock the client with the right error. */
        if (server.cluster->m_state == CLUSTER_FAIL) {
            clusterRedirectClient(c,NULL,0,CLUSTER_REDIR_DOWN_STATE);
            return 1;
        }

        /* All keys must belong to the same slot, so check first key only. */
        dictIterator di(c->bpop.keys);
        if ((de = di.dictNext()) != NULL) {
            robj *key = (robj *)de->dictGetKey();
            int slot = keyHashSlot((char*)key->ptr, sdslen((sds)key->ptr));
            clusterNode *node = server.cluster->m_slots[slot];

            /* We send an error and unblock the client if:
             * 1) The slot is unassigned, emitting a cluster down error.
             * 2) The slot is not handled by this node, nor being imported. */
            if (node != myself &&
                server.cluster->m_importing_slots_from[slot] == NULL)
            {
                if (node == NULL) {
                    clusterRedirectClient(c,NULL,0,
                        CLUSTER_REDIR_DOWN_UNBOUND);
                } else {
                    clusterRedirectClient(c,node,slot,
                        CLUSTER_REDIR_MOVED);
                }
                return 1;
            }
        }
    }
    return 0;
}
