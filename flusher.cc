/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */

#include "flusher.hh"

bool FlusherStepper::callback(Dispatcher &d, TaskId t) {
    getLogger()->log(EXTENSION_LOG_DEBUG, NULL, "Stepping flusher\n");
    return flusher->step(d, t);
}

bool Flusher::stop(void) {
    return transition_state(stopping);
}

bool Flusher::pause(void) {
    return transition_state(pausing);
}

bool Flusher::resume(void) {
    return transition_state(running);
}

static bool validTransition(enum flusher_state from,
                            enum flusher_state to)
{
    bool rv(true);
    if (from == initializing && to == running) {
    } else if (from == running && to == pausing) {
    } else if (from == running && to == stopping) {
    } else if (from == pausing && to == paused) {
    } else if (from == stopping && to == stopped) {
    } else if (from == paused && to == running) {
    } else if (from == paused && to == stopping) {
    } else if (from == pausing && to == stopping) {
    } else {
        rv = false;
    }
    return rv;
}

const char * const Flusher::stateName(enum flusher_state st) const {
    static const char * const stateNames[] = {
        "initializing", "running", "pausing", "paused", "stopping", "stopped"
    };
    assert(st >= initializing && st <= stopped);
    return stateNames[st];
}

bool Flusher::transition_state(enum flusher_state to) {

    getLogger()->log(EXTENSION_LOG_DEBUG, NULL,
                     "Attempting transition from %s to %s\n",
                     stateName(_state), stateName(to));

    if (!validTransition(_state, to)) {
        return false;
    }

    getLogger()->log(EXTENSION_LOG_DEBUG, NULL, "Transitioning from %s to %s\n",
                     stateName(_state), stateName(to));

    _state = to;
    //Reschedule the task
    dispatcher->kill(task);
    start();
    return true;
}

const char * const Flusher::stateName() const {
    return stateName(_state);
}

enum flusher_state Flusher::state() const {
    return _state;
}

void Flusher::initialize(void) {
    getLogger()->log(EXTENSION_LOG_DEBUG, NULL,
                     "Initializing flusher; warming up\n");

    time_t startTime = time(NULL);
    store->warmup();
    store->stats.warmupTime = time(NULL) - startTime;
    store->stats.warmupComplete = true;
    store->stats.curr_items += store->stats.warmedUp;

    getLogger()->log(EXTENSION_LOG_DEBUG, NULL,
                     "Warmup completed in %ds\n", store->stats.warmupTime);
    transition_state(running);
}

void Flusher::start(void) {
    task = dispatcher->schedule(&callback);
}

bool Flusher::step(Dispatcher &d, TaskId tid) {
    try {
        switch (_state) {
        case initializing:
            initialize();
            return true;
        case paused:
            return false;
        case pausing:
            transition_state(paused);
            return false;
        case running:
            {
                int n = doFlush(true);
                if (n > 0) {
                    if (_state == running && n > 0) {
                        d.snooze(tid, n);
                    }
                }
            }
            return true;
        case stopping:
            {
                std::stringstream ss;
                ss << "Shutting down flusher (Write of all dirty items)"
                   << std::endl;
                getLogger()->log(EXTENSION_LOG_DEBUG, NULL, "%s",
                                 ss.str().c_str());
            }
            store->stats.min_data_age = 0;
            doFlush(false);
            getLogger()->log(EXTENSION_LOG_DEBUG, NULL, "Flusher stopped\n");
            transition_state(stopped);
            return false;
        case stopped:
            return false;
        default:
            getLogger()->log(EXTENSION_LOG_WARNING, NULL,
                "Unexpected state in flusher: %s", stateName());
            assert(false);
        }
    } catch(std::runtime_error &e) {
        std::stringstream ss;
        ss << "Exception in flusher loop: " << e.what() << std::endl;
        getLogger()->log(EXTENSION_LOG_WARNING, NULL, "%s",
                         ss.str().c_str());
        assert(false);
    }
}

int Flusher::doFlush(bool shouldWait) {
    int rv(0);
    std::queue<std::string> *q = store->beginFlush(shouldWait);
    getLogger()->log(EXTENSION_LOG_DEBUG, NULL,
                     "Looking for something to flush.\n");
    if (q) {
        getLogger()->log(EXTENSION_LOG_DEBUG, NULL,
                         "Flushing a write queue.\n");
        std::queue<std::string> *rejectQueue = new std::queue<std::string>();
        rel_time_t flush_start = ep_current_time();
        rv = store->stats.min_data_age;

        while (!q->empty()) {
            int n = store->flushSome(q, rejectQueue);
            if (_state == pausing) {
                transition_state(paused);
            }
            if (n < rv) {
                rv = n;
            }
        }
        getLogger()->log(EXTENSION_LOG_DEBUG, NULL, "Rejected %d items\n",
                         rejectQueue->size());
        store->completeFlush(rejectQueue, flush_start);

        getLogger()->log(EXTENSION_LOG_INFO, NULL,
                         "Completed a flush, age of oldest item was %ds\n",
                         rv);

        delete rejectQueue;
    }
    return rv;
}
