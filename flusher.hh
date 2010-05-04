/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
#ifndef FLUSHER_H
#define FLUSHER_H 1

#include "common.hh"
#include "ep.hh"
#include "dispatcher.hh"

enum flusher_state {
    initializing,
    running,
    pausing,
    paused,
    stopping,
    stopped
};

class Flusher;

class FlusherStepper : public DispatcherCallback {
public:
    FlusherStepper(Flusher* f) : flusher(f) { }
    virtual bool callback(Dispatcher &d, TaskId t);
private:
    Flusher *flusher;
};

class Flusher {
public:
    Flusher(EventuallyPersistentStore *st, Dispatcher *d):
        store(st), _state(initializing), dispatcher(d), callback(this) {
    }
    ~Flusher() {
        stop();
    }

    bool stop();
    bool pause();
    bool resume();

    void initialize();

    void start(void);
    bool step(Dispatcher&, TaskId);

    bool transition_state(enum flusher_state to);

    enum flusher_state state() const;
    const char * const stateName() const;
private:
    EventuallyPersistentStore *store;
    volatile enum flusher_state _state;
    TaskId task;
    Dispatcher *dispatcher;
    FlusherStepper callback;
    const char * const stateName(enum flusher_state st) const;
    int doFlush(bool shouldWait);

    DISALLOW_COPY_AND_ASSIGN(Flusher);
};

#endif /* FLUSHER_H */
