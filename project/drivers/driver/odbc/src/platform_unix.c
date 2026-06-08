// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

/*
 * ScratchBird ODBC Driver - Unix Platform Implementation
 * Copyright (c) 2025-2026 Dalton Calford
 */
#include "scratchbird/odbc/platform.h"

#if SB_PLATFORM_UNIX

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/time.h>

/* Event implementation for Unix */
int sb_event_create(SB_EVENT* e, int manual, int initial) {
    if (!e) return -1;
    
    memset(e, 0, sizeof(SB_EVENT));
    
    if (pthread_mutex_init(&e->mutex, NULL) != 0) {
        return -1;
    }
    
    if (pthread_cond_init(&e->cond, NULL) != 0) {
        pthread_mutex_destroy(&e->mutex);
        return -1;
    }
    
    e->signaled = initial ? 1 : 0;
    e->manual_reset = manual ? 1 : 0;
    
    return 0;
}

int sb_event_set(SB_EVENT* e) {
    if (!e) return -1;
    
    pthread_mutex_lock(&e->mutex);
    e->signaled = 1;
    pthread_cond_broadcast(&e->cond);
    pthread_mutex_unlock(&e->mutex);
    
    return 0;
}

int sb_event_reset(SB_EVENT* e) {
    if (!e) return -1;
    
    pthread_mutex_lock(&e->mutex);
    e->signaled = 0;
    pthread_mutex_unlock(&e->mutex);
    
    return 0;
}

int sb_event_wait(SB_EVENT* e) {
    if (!e) return -1;
    
    pthread_mutex_lock(&e->mutex);
    while (!e->signaled) {
        pthread_cond_wait(&e->cond, &e->mutex);
    }
    if (!e->manual_reset) {
        e->signaled = 0;
    }
    pthread_mutex_unlock(&e->mutex);
    
    return 0;
}

int sb_event_wait_timeout(SB_EVENT* e, DWORD ms) {
    struct timespec ts;
    struct timeval tv;
    int result = 0;
    
    if (!e) return -1;
    
    gettimeofday(&tv, NULL);
    ts.tv_sec = tv.tv_sec + (ms / 1000);
    ts.tv_nsec = (tv.tv_usec + (ms % 1000) * 1000) * 1000;
    if (ts.tv_nsec >= 1000000000) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000;
    }
    
    pthread_mutex_lock(&e->mutex);
    while (!e->signaled && result == 0) {
        result = pthread_cond_timedwait(&e->cond, &e->mutex, &ts);
    }
    
    if (result == 0) {
        if (!e->manual_reset) {
            e->signaled = 0;
        }
    } else if (result == ETIMEDOUT) {
        result = 1; /* Timeout */
    }
    
    pthread_mutex_unlock(&e->mutex);
    return result;
}

void sb_event_destroy(SB_EVENT* e) {
    if (!e) return;
    
    pthread_cond_destroy(&e->cond);
    pthread_mutex_destroy(&e->mutex);
    memset(e, 0, sizeof(SB_EVENT));
}

/* Platform initialization */
int sb_platform_init(void) {
    /* Nothing special needed for Unix */
    return 0;
}

void sb_platform_cleanup(void) {
    /* Nothing special needed for Unix */
}

#endif /* SB_PLATFORM_UNIX */
