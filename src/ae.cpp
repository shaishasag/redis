/* A simple event-driven programming library. Originally I wrote this code
 * for the Jim's event-loop (Jim is a Tcl interpreter) but later translated
 * it in form of a library for easy reuse.
 *
 * Copyright (c) 2006-2010, Salvatore Sanfilippo <antirez at gmail dot com>
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

#include <stdio.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdlib.h>
#include <poll.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <new>

#include "ae.h"
#include "zmalloc.h"
#include "config.h"

/* Include the best multiplexing layer supported by this system.
 * The following should be ordered by performances, descending. */
#ifdef HAVE_EVPORT
#include "ae_evport.cpp"
#else
    #ifdef HAVE_EPOLL
    #include "ae_epoll.cpp"
    #else
        #ifdef HAVE_KQUEUE
        #include "ae_kqueue.cpp"
        #else
        #include "ae_select.cpp"
        #endif
    #endif
#endif

static void aeAddMillisecondsToNow(long long milliseconds, long *sec, long *ms);

aeFileEvent::aeFileEvent()
: m_mask(AE_NONE)
, m_rfileProc(NULL)
, m_wfileProc(NULL)
, m_clientData(NULL)
{

}

aeTimeEvent::aeTimeEvent(long long in_id, long long in_milliseconds, aeTimeProc *proc, void *in_clientData, aeEventFinalizerProc *in_finalizerProc, aeTimeEvent* in_next)
: m_id(in_id)
, m_when_sec(0) /* seconds */
, m_when_ms(0) /* milliseconds */
, m_timeProc(proc)
, m_finalizerProc(in_finalizerProc)
, m_clientData(in_clientData)
, m_next(in_next)
{
    aeAddMillisecondsToNow(in_milliseconds, &m_when_sec, &m_when_ms);
}

aeEventLoop::aeEventLoop(int in_setsize)
{
    m_events = (aeFileEvent *)zmalloc(sizeof(aeFileEvent)*in_setsize);
    m_fired  = (aeFiredEvent *)zmalloc(sizeof(aeFiredEvent)*in_setsize);
    m_setsize = in_setsize;
    m_lastTime = time(NULL);
    m_timeEventHead = NULL;
    m_timeEventNextId = 0;
    m_stop = 0;
    m_maxfd = -1;
    m_beforesleep = NULL;
    m_aftersleep = NULL;
    aeApiCreate();
    /* Events with m_mask == AE_NONE are not set. So let's initialize the
     * vector with it. */
    for (int i = 0; i < m_setsize; i++)
        new (m_events+i) aeFileEvent;
}

aeEventLoop *aeCreateEventLoop(int in_setsize)
{
    aeEventLoop *eventLoop = NULL;
    void *evenLoopMem = zmalloc(sizeof(aeEventLoop));
    if (evenLoopMem == NULL)
        goto err;
    eventLoop = new (evenLoopMem) aeEventLoop(in_setsize);
    if (eventLoop->m_events == NULL || eventLoop->m_fired == NULL)
        goto err;
    
    return eventLoop;

err:
    if (eventLoop != NULL) {
        aeDeleteEventLoop(eventLoop);
    }
    return NULL;
}

/* Return the current set size. */
int aeEventLoop::aeGetSetSize() {
    return m_setsize;
}

/* Resize the maximum set size of the event loop.
 * If the requested set size is smaller than the current set size, but
 * there is already a file descriptor in use that is >= the requested
 * set size minus one, AE_ERR is returned and the operation is not
 * performed at all.
 *
 * Otherwise AE_OK is returned and the operation is successful. */
int aeEventLoop::aeResizeSetSize(int in_setsize) {
    int i;

    if (m_setsize == in_setsize) return AE_OK;
    if (m_maxfd >= in_setsize) return AE_ERR;
    if (aeApiResize(in_setsize) == -1) return AE_ERR;

    m_events = (aeFileEvent *)zrealloc(m_events,sizeof(aeFileEvent)*in_setsize);
    m_fired = (aeFiredEvent *)zrealloc(m_fired,sizeof(aeFiredEvent)*in_setsize);
    m_setsize = in_setsize;

    /* Make sure that if we created new slots, they are initialized with
     * an AE_NONE m_mask. */
    for (i = m_maxfd+1; i < m_setsize; i++)
        new (m_events+i) aeFileEvent;
    return AE_OK;
}

aeEventLoop::~aeEventLoop()
{
    aeApiFree();
    zfree(m_events);
    zfree(m_fired);
}

void aeDeleteEventLoop(aeEventLoop *eventLoop)
{
    if (NULL != eventLoop)
    {
        eventLoop->~aeEventLoop();
        zfree(eventLoop);
    }
}

void aeEventLoop::aeStop() {
    m_stop = 1;
}

int aeEventLoop::aeCreateFileEvent(int fd, int mask, aeFileProc *proc, void *clientData)
{
    if (fd >= m_setsize) {
        errno = ERANGE;
        return AE_ERR;
    }
    aeFileEvent *fe = &m_events[fd];

    if (aeApiAddEvent(fd, mask) == -1)
        return AE_ERR;
    fe->m_mask |= mask;
    if (mask & AE_READABLE) fe->m_rfileProc = proc;
    if (mask & AE_WRITABLE) fe->m_wfileProc = proc;
    fe->m_clientData = clientData;
    if (fd > m_maxfd)
        m_maxfd = fd;
    return AE_OK;
}

void aeEventLoop::aeDeleteFileEvent(int fd, int mask)
{
    if (fd >= m_setsize) return;
    aeFileEvent *fe = &m_events[fd];
    if (fe->m_mask == AE_NONE) return;

    aeApiDelEvent(fd, mask);
    fe->m_mask = fe->m_mask & (~mask);
    if (fd == m_maxfd && fe->m_mask == AE_NONE) {
        /* Update the max fd */
        int j;

        for (j = m_maxfd-1; j >= 0; j--)
            if (m_events[j].m_mask != AE_NONE) break;
        m_maxfd = j;
    }
}

int aeEventLoop::aeGetFileEvents(int fd) {
    if (fd >= m_setsize) return 0;
    aeFileEvent *fe = &m_events[fd];

    return fe->m_mask;
}

static void aeGetTime(long *seconds, long *milliseconds)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);
    *seconds = tv.tv_sec;
    *milliseconds = tv.tv_usec/1000;
}

static void aeAddMillisecondsToNow(long long milliseconds, long *sec, long *ms) {
    long cur_sec, cur_ms, when_sec, when_ms;

    aeGetTime(&cur_sec, &cur_ms);
    when_sec = cur_sec + milliseconds/1000;
    when_ms = cur_ms + milliseconds%1000;
    if (when_ms >= 1000) {
        when_sec ++;
        when_ms -= 1000;
    }
    *sec = when_sec;
    *ms = when_ms;
}

long long aeEventLoop::aeCreateTimeEvent(long long milliseconds, aeTimeProc *proc, void *clientData, aeEventFinalizerProc *finalizerProc)
{
    aeTimeEvent *te = NULL;

    void* aeTimeEvent_mem = zmalloc(sizeof(aeTimeEvent));
    if (aeTimeEvent_mem == NULL) return AE_ERR;

    long long id = m_timeEventNextId++;
    te = new (aeTimeEvent_mem) aeTimeEvent(id, milliseconds, proc, clientData, finalizerProc, m_timeEventHead);

    m_timeEventHead = te;
    return id;
}

int aeEventLoop::aeDeleteTimeEvent(long long id)
{
    aeTimeEvent *te = m_timeEventHead;
    while(te) {
        if (te->m_id == id) {
            te->m_id = AE_DELETED_EVENT_ID;
            return AE_OK;
        }
        te = te->m_next;
    }
    return AE_ERR; /* NO event with the specified ID found */
}

/* Search the first timer to fire.
 * This operation is useful to know how many time the select can be
 * put in sleep without to delay any event.
 * If there are no timers NULL is returned.
 *
 * Note that's O(N) since time events are unsorted.
 * Possible optimizations (not needed by Redis so far, but...):
 * 1) Insert the event in order, so that the nearest is just the head.
 *    Much better but still insertion or deletion of timers is O(N).
 * 2) Use a skiplist to have this operation as O(1) and insertion as O(log(N)).
 */
aeTimeEvent *aeEventLoop::aeSearchNearestTimer()
{
    aeTimeEvent *te = m_timeEventHead;
    aeTimeEvent *nearest = NULL;

    while(te) {
        if (!nearest || te->m_when_sec < nearest->m_when_sec ||
                (te->m_when_sec == nearest->m_when_sec &&
                 te->m_when_ms < nearest->m_when_ms))
            nearest = te;
        te = te->m_next;
    }
    return nearest;
}

/* Process time events */
int aeEventLoop::processTimeEvents()
{
    int processed = 0;
    aeTimeEvent *te, *prev;
    long long maxId;
    time_t now = time(NULL);

    /* If the system clock is moved to the future, and then set back to the
     * right value, time events may be delayed in a random way. Often this
     * means that scheduled operations will not be performed soon enough.
     *
     * Here we try to detect system clock skews, and force all the time
     * events to be processed ASAP when this happens: the idea is that
     * processing events earlier is less dangerous than delaying them
     * indefinitely, and practice suggests it is. */
    if (now < m_lastTime) {
        te = m_timeEventHead;
        while(te) {
            te->m_when_sec = 0;
            te = te->m_next;
        }
    }
    m_lastTime = now;

    prev = NULL;
    te = m_timeEventHead;
    maxId = m_timeEventNextId-1;
    while(te) {
        long now_sec, now_ms;
        long long id;

        /* Remove events scheduled for deletion. */
        if (te->m_id == AE_DELETED_EVENT_ID) {
            aeTimeEvent *next = te->m_next;
            if (prev == NULL)
                m_timeEventHead = te->m_next;
            else
                prev->m_next = te->m_next;
            if (te->m_finalizerProc)
                te->m_finalizerProc(this, te->m_clientData);
            zfree(te);
            te = next;
            continue;
        }

        /* Make sure we don't process time events created by time events in
         * this iteration. Note that this check is currently useless: we always
         * add new timers on the head, however if we change the implementation
         * detail, this check may be useful again: we keep it here for future
         * defense. */
        if (te->m_id > maxId) {
            te = te->m_next;
            continue;
        }
        aeGetTime(&now_sec, &now_ms);
        if (now_sec > te->m_when_sec ||
            (now_sec == te->m_when_sec && now_ms >= te->m_when_ms))
        {
            int retval;

            id = te->m_id;
            retval = te->m_timeProc(this, id, te->m_clientData);
            processed++;
            if (retval != AE_NOMORE) {
                aeAddMillisecondsToNow(retval, &te->m_when_sec, &te->m_when_ms);
            } else {
                te->m_id = AE_DELETED_EVENT_ID;
            }
        }
        prev = te;
        te = te->m_next;
    }
    return processed;
}

/* Process every pending time event, then every pending file event
 * (that may be registered by time event callbacks just processed).
 * Without special flags the function sleeps until some file event
 * fires, or when the next time event occurs (if any).
 *
 * If flags is 0, the function does nothing and returns.
 * if flags has AE_ALL_EVENTS set, all the kind of events are processed.
 * if flags has AE_FILE_EVENTS set, file events are processed.
 * if flags has AE_TIME_EVENTS set, time events are processed.
 * if flags has AE_DONT_WAIT set the function returns ASAP until all
 * if flags has AE_CALL_AFTER_SLEEP set, the aftersleep callback is called.
 * the events that's possible to process without to wait are processed.
 *
 * The function returns the number of events processed. */
int aeEventLoop::aeProcessEvents(int flags)
{
    int processed = 0, numevents;

    /* Nothing to do? return ASAP */
    if (!(flags & AE_TIME_EVENTS) && !(flags & AE_FILE_EVENTS)) return 0;

    /* Note that we want call select() even if there are no
     * file events to process as long as we want to process time
     * events, in order to sleep until the next time event is ready
     * to fire. */
    if (m_maxfd != -1 ||
        ((flags & AE_TIME_EVENTS) && !(flags & AE_DONT_WAIT))) {
        int j;
        aeTimeEvent *shortest = NULL;
        struct timeval tv, *tvp;

        if (flags & AE_TIME_EVENTS && !(flags & AE_DONT_WAIT))
            shortest = aeSearchNearestTimer();
        if (shortest) {
            long now_sec, now_ms;

            aeGetTime(&now_sec, &now_ms);
            tvp = &tv;

            /* How many milliseconds we need to wait for the next
             * time event to fire? */
            long long ms =
                (shortest->m_when_sec - now_sec)*1000 +
                shortest->m_when_ms - now_ms;

            if (ms > 0) {
                tvp->tv_sec = ms/1000;
                tvp->tv_usec = (ms % 1000)*1000;
            } else {
                tvp->tv_sec = 0;
                tvp->tv_usec = 0;
            }
        } else {
            /* If we have to check for events but need to return
             * ASAP because of AE_DONT_WAIT we need to set the timeout
             * to zero */
            if (flags & AE_DONT_WAIT) {
                tv.tv_sec = tv.tv_usec = 0;
                tvp = &tv;
            } else {
                /* Otherwise we can block */
                tvp = NULL; /* wait forever */
            }
        }

        /* Call the multiplexing API, will return only on timeout or when
         * some event fires. */
        numevents = aeApiPoll(tvp);

        /* After sleep callback. */
        if (m_aftersleep != NULL && flags & AE_CALL_AFTER_SLEEP)
            m_aftersleep(this);

        for (j = 0; j < numevents; j++) {
            aeFileEvent *fe = &m_events[m_fired[j].m_fd];
            int mask = m_fired[j].m_mask;
            int fd = m_fired[j].m_fd;
            int rfired = 0;

	    /* note the fe->m_mask & mask & ... code: maybe an already processed
             * event removed an element that fired and we still didn't
             * processed, so we check if the event is still valid. */
            if (fe->m_mask & mask & AE_READABLE) {
                rfired = 1;
                fe->m_rfileProc(this,fd,fe->m_clientData,mask);
            }
            if (fe->m_mask & mask & AE_WRITABLE) {
                if (!rfired || fe->m_wfileProc != fe->m_rfileProc)
                    fe->m_wfileProc(this,fd,fe->m_clientData,mask);
            }
            processed++;
        }
    }
    /* Check time events */
    if (flags & AE_TIME_EVENTS)
        processed += processTimeEvents();

    return processed; /* return the number of processed file/time events */
}

/* Wait for milliseconds until the given file descriptor becomes
 * writable/readable/exception */
int aeWait(int fd, int mask, long long milliseconds) {
    struct pollfd pfd;
    int retmask = 0, retval;

    memset(&pfd, 0, sizeof(pfd));
    pfd.fd = fd;
    if (mask & AE_READABLE) pfd.events |= POLLIN;
    if (mask & AE_WRITABLE) pfd.events |= POLLOUT;

    if ((retval = poll(&pfd, 1, milliseconds))== 1) {
        if (pfd.revents & POLLIN) retmask |= AE_READABLE;
        if (pfd.revents & POLLOUT) retmask |= AE_WRITABLE;
	if (pfd.revents & POLLERR) retmask |= AE_WRITABLE;
        if (pfd.revents & POLLHUP) retmask |= AE_WRITABLE;
        return retmask;
    } else {
        return retval;
    }
}

void aeEventLoop::aeMain() {
    m_stop = 0;
    while (!m_stop) {
        if (m_beforesleep != NULL)
            m_beforesleep(this);
        aeProcessEvents(AE_ALL_EVENTS|AE_CALL_AFTER_SLEEP);
    }
}

void aeEventLoop::aeSetBeforeSleepProc(aeBeforeSleepProc *in_beforesleep) {
    m_beforesleep = in_beforesleep;
}

void aeEventLoop::aeSetAfterSleepProc(aeBeforeSleepProc *in_aftersleep) {
    m_aftersleep = in_aftersleep;
}
