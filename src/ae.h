/* A simple event-driven programming library. Originally I wrote this code
 * for the Jim's event-loop (Jim is a Tcl interpreter) but later translated
 * it in form of a library for easy reuse.
 *
 * Copyright (c) 2006-2012, Salvatore Sanfilippo <antirez at gmail dot com>
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

#ifndef __AE_H__
#define __AE_H__

#include <time.h>

#define AE_OK 0
#define AE_ERR -1

#define AE_NONE 0
#define AE_READABLE 1
#define AE_WRITABLE 2

#define AE_FILE_EVENTS 1
#define AE_TIME_EVENTS 2
#define AE_ALL_EVENTS (AE_FILE_EVENTS|AE_TIME_EVENTS)
#define AE_DONT_WAIT 4
#define AE_CALL_AFTER_SLEEP 8

#define AE_NOMORE -1
#define AE_DELETED_EVENT_ID -1

/* Macros */
#define AE_NOTUSED(V) ((void) V)

class aeEventLoop;

/* Types and data structures */
typedef void aeFileProc(aeEventLoop*, int fd, void *clientData, int mask);
typedef int aeTimeProc(aeEventLoop*, long long id, void *clientData);
typedef void aeEventFinalizerProc(aeEventLoop*, void *clientData);
typedef void aeBeforeSleepProc(aeEventLoop*);

/* File event structure */
class aeFileEvent
{
public:
    aeFileEvent();
    int m_mask; /* one of AE_(READABLE|WRITABLE) */
    aeFileProc *m_rfileProc;
    aeFileProc *m_wfileProc;
    void *m_clientData;
};

/* Time event structure */
class aeTimeEvent
{
public:
    aeTimeEvent(long long in_id, long long in_milliseconds, aeTimeProc *proc, void *in_clientData, aeEventFinalizerProc *in_finalizerProc, aeTimeEvent* in_next);

    long long m_id; /* time event identifier. */
    long m_when_sec; /* seconds */
    long m_when_ms; /* milliseconds */
    aeTimeProc *m_timeProc;
    aeEventFinalizerProc *m_finalizerProc;
    void *m_clientData;
    struct aeTimeEvent *m_next;
};

/* A fired event */
struct aeFiredEvent {
    int m_fd;
    int m_mask;
};

/* Prototypes */

aeEventLoop *aeCreateEventLoop(int in_setsize);
void aeDeleteEventLoop(aeEventLoop* eventLoop);
int aeWait(int fd, int mask, long long milliseconds);

/* State of an event based program */
class aeEventLoop
{
    friend aeEventLoop *aeCreateEventLoop(int in_setsize);
public:
    aeEventLoop(int setsize);
    ~aeEventLoop();
    
    void aeStop();
    int aeCreateFileEvent(int fd, int mask, aeFileProc *proc, void *clientData);
    void aeDeleteFileEvent(int fd, int mask);
    int aeGetFileEvents(int fd);
    long long aeCreateTimeEvent(long long milliseconds,
        aeTimeProc *proc, void *clientData,
        aeEventFinalizerProc *finalizerProc);
    int aeDeleteTimeEvent(long long id);
    int aeProcessEvents(int flags);
    void aeMain();
    void aeSetBeforeSleepProc(aeBeforeSleepProc *beforesleep);
    void aeSetAfterSleepProc(aeBeforeSleepProc *aftersleep);
    int aeGetSetSize();
    int aeResizeSetSize(int in_setsize);
    char *aeApiName();

private:
    int m_maxfd;   /* highest file descriptor currently registered */
    int m_setsize; /* max number of file descriptors tracked */
    long long m_timeEventNextId;
    time_t m_lastTime;     /* Used to detect system clock skew */
    aeFileEvent *m_events; /* Registered events */
    aeFiredEvent *m_fired; /* Fired events */
    aeTimeEvent *m_timeEventHead;
    int m_stop;
    void *m_apidata; /* This is used for polling API specific data */
    aeBeforeSleepProc *m_beforesleep;
    aeBeforeSleepProc *m_aftersleep;

    aeTimeEvent* aeSearchNearestTimer();
    int processTimeEvents();

    int aeApiCreate();
    int aeApiResize(int setsize);
    void aeApiFree();
    int aeApiAddEvent(int fd, int mask);
    void aeApiDelEvent(int fd, int delmask);
    int aeApiPoll(struct timeval *tvp);

};


#endif
