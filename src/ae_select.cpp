/* Select()-based ae.c module.
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


#include <sys/select.h>
#include <string.h>

typedef struct aeApiState {
    fd_set rfds, wfds;
    /* We need to have a copy of the fd sets as it's not safe to reuse
     * FD sets after select(). */
    fd_set _rfds, _wfds;
} aeApiState;

int aeEventLoop::aeApiCreate() {
    aeApiState* state = (aeApiState*)zmalloc(sizeof(aeApiState));

    if (!state) return -1;
    FD_ZERO(&state->rfds);
    FD_ZERO(&state->wfds);
    m_apidata = state;
    return 0;
}

int aeEventLoop::aeApiResize(int setsize) {
    /* Just ensure we have enough room in the fd_set type. */
    if (setsize >= FD_SETSIZE) return -1;
    return 0;
}

void aeEventLoop::aeApiFree() {
    zfree(m_apidata);
}

int aeEventLoop::aeApiAddEvent(int fd, int mask) {
    aeApiState *state = m_apidata;

    if (mask & AE_READABLE) FD_SET(fd,&state->rfds);
    if (mask & AE_WRITABLE) FD_SET(fd,&state->wfds);
    return 0;
}

void aeEventLoop::aeApiDelEvent(int fd, int mask) {
    aeApiState *state = m_apidata;

    if (mask & AE_READABLE) FD_CLR(fd,&state->rfds);
    if (mask & AE_WRITABLE) FD_CLR(fd,&state->wfds);
}

int aeEventLoop::aeApiPoll(struct timeval *tvp) {
    aeApiState *state = m_apidata;
    int retval, j, numevents = 0;

    memcpy(&state->_rfds,&state->rfds,sizeof(fd_set));
    memcpy(&state->_wfds,&state->wfds,sizeof(fd_set));

    retval = select(m_maxfd+1,
                &state->_rfds,&state->_wfds,NULL,tvp);
    if (retval > 0) {
        for (j = 0; j <= m_maxfd; j++) {
            int mask = 0;
            aeFileEvent *fe = &m_events[j];

            if (fe->m_mask == AE_NONE) continue;
            if (fe->m_mask & AE_READABLE && FD_ISSET(j,&state->_rfds))
                mask |= AE_READABLE;
            if (fe->m_mask & AE_WRITABLE && FD_ISSET(j,&state->_wfds))
                mask |= AE_WRITABLE;
            m_fired[numevents].m_fd = j;
            m_fired[numevents].m_mask = mask;
            numevents++;
        }
    }
    return numevents;
}

char *aeEventLoop::aeApiName() {
    return "select";
}
