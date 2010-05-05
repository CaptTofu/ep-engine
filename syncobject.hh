/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
#ifndef SYNCOBJECT_HH
#define SYNCOBJECT_HH 1

#include <stdexcept>
#include <iostream>
#include <sstream>
#include <pthread.h>
#include <sys/time.h>
#include <math.h>

#include "common.hh"

/**
 * Abstraction built on top of pthread mutexes
 */
class SyncObject : public Mutex {
public:
    SyncObject() : Mutex() {
        if (pthread_cond_init(&cond, NULL) != 0) {
            throw std::runtime_error("MUTEX ERROR: Failed to initialize cond.");
        }
    }

    ~SyncObject() {
        if (pthread_cond_destroy(&cond) != 0) {
            throw std::runtime_error("MUTEX ERROR: Failed to destroy cond.");
        }
    }

    void wait() {
        if (pthread_cond_wait(&cond, &mutex) != 0) {
            throw std::runtime_error("Failed to wait for condition.");
        }
    }

    bool wait(const struct timeval &tv) {
        struct timespec ts;
        ts.tv_sec = tv.tv_sec + 0;
        ts.tv_nsec = tv.tv_usec * 1000;

        switch (pthread_cond_timedwait(&cond, &mutex, &ts)) {
        case 0:
            return true;
        case ETIMEDOUT:
            return false;
        default:
            throw std::runtime_error("Failed timed_wait for condition.");
        }
    }

    bool wait(const double secs) {
        float ip, fp;
        struct timeval tv;
        gettimeofday(&tv, NULL);
        fp = modff(secs, &ip);
        int usec = (fp * 1e6) + tv.tv_usec + 0;
        div_t d = div(usec, 1000000);
        tv.tv_sec = ip + tv.tv_sec + d.quot + 0;
        tv.tv_usec = d.rem;
        return wait(tv);
    }

    void notify() {
        if(pthread_cond_broadcast(&cond) != 0) {
            throw std::runtime_error("Failed to broadcast change.");
        }
    }

private:
    pthread_cond_t cond;

    DISALLOW_COPY_AND_ASSIGN(SyncObject);
};

#endif

