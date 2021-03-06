/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
#include "ep.hh"
#include "flusher.hh"
#include "locks.hh"
#include "dispatcher.hh"

#include <vector>
#include <time.h>
#include <string.h>

extern "C" {
    static rel_time_t uninitialized_current_time(void) {
        abort();
        return 0;
    }

    rel_time_t (*ep_current_time)() = uninitialized_current_time;
}

EventuallyPersistentStore::EventuallyPersistentStore(KVStore *t,
                                                     size_t est) :
    loadStorageKVPairCallback(storage, stats)
{
    est_size = est;
    stats.min_data_age.set(DEFAULT_MIN_DATA_AGE);
    stats.queue_age_cap.set(DEFAULT_MIN_DATA_AGE_CAP);

    doPersistence = getenv("EP_NO_PERSISTENCE") == NULL;
    dispatcher = new Dispatcher();
    flusher = new Flusher(this, dispatcher);

    setTxnSize(DEFAULT_TXN_SIZE);

    underlying = t;

    startDispatcher();
    startFlusher();
    assert(underlying);
}

class VerifyStoredVisitor : public HashTableVisitor {
public:
    std::vector<std::string> dirty;
    virtual void visit(StoredValue *v) {
        if (v->isDirty()) {
            dirty.push_back(v->getKey());
        }
    }
};

EventuallyPersistentStore::~EventuallyPersistentStore() {
    stopFlusher();
    dispatcher->stop();

    // Verify that we don't have any dirty objects!
    if (getenv("EP_VERIFY_SHUTDOWN_FLUSH") != NULL) {
        VerifyStoredVisitor walker;
        storage.visit(walker);
        if (!walker.dirty.empty()) {
            std::vector<std::string>::const_iterator iter;
            for (iter = walker.dirty.begin();
                 iter != walker.dirty.end();
                 ++iter) {
                std::cerr << "ERROR: Object dirty after flushing: "
                          << iter->c_str() << std::endl;
            }

            throw std::runtime_error("Internal error, objects dirty objects exists");
        }
    }

    delete flusher;
    delete dispatcher;
}

void EventuallyPersistentStore::startDispatcher() {
    dispatcher->start();
}


const Flusher* EventuallyPersistentStore::getFlusher() {
    return flusher;
}

void EventuallyPersistentStore::startFlusher() {
    flusher->start();
}

void EventuallyPersistentStore::stopFlusher() {
    bool rv = flusher->stop();
    if (rv) {
        flusher->wait();
    }
}

bool EventuallyPersistentStore::pauseFlusher() {
    flusher->pause();
    return true;
}

bool EventuallyPersistentStore::resumeFlusher() {
    flusher->resume();
    return true;
}

void EventuallyPersistentStore::set(const Item &item, Callback<bool> &cb) {
    mutation_type_t mtype = storage.set(item);
    bool rv = true;

    if (mtype == INVALID_CAS || mtype == IS_LOCKED) {
        rv = false;
    } else if (mtype == WAS_CLEAN || mtype == NOT_FOUND) {
        queueDirty(item.getKey());
        if (mtype == NOT_FOUND) {
            stats.curr_items.incr();
        }
    }

    cb.setStatus((int)mtype);
    cb.callback(rv);
}

void EventuallyPersistentStore::get(const std::string &key,
                                    Callback<GetValue> &cb) {
    int bucket_num = storage.bucket(key);
    LockHolder lh(storage.getMutex(bucket_num));
    StoredValue *v = storage.unlocked_find(key, bucket_num);

    if (v) {
        // return an invalid cas value if the item is locked
        GetValue rv(new Item(v->getKey(), v->getFlags(), v->getExptime(),
                             v->getValue(), v->isLocked(ep_current_time()) ? -1 : v->getCas()));
        cb.callback(rv);
    } else {
        GetValue rv(false);
        cb.callback(rv);
    }
    lh.unlock();
}

bool EventuallyPersistentStore::getLocked(const std::string &key,
                                          Callback<GetValue> &cb,
                                          rel_time_t currentTime,
                                          uint32_t lockTimeout) {

    int bucket_num = storage.bucket(key);
    LockHolder lh(storage.getMutex(bucket_num));
    StoredValue *v = storage.unlocked_find(key, bucket_num);

    if (v) {
        if (v->isLocked(currentTime)) {
            GetValue rv(false);
            cb.callback(rv);
            lh.unlock();
            return false;
        }

        // acquire lock and increment cas value

        v->lock(currentTime + lockTimeout);

        Item *it = new Item(v->getKey(), v->getFlags(), v->getExptime(),
                v->getValue(), v->getCas());

         it->setCas();
         v->setCas(it->getCas());

        GetValue rv(it);
        cb.callback(rv);

    } else {
        GetValue rv(false);
        cb.callback(rv);
    }
    lh.unlock();
    return true;
}

bool EventuallyPersistentStore::getKeyStats(const std::string &key,
                                            struct key_stats &kstats)
{
    bool found = false;
    int bucket_num = storage.bucket(key);
    LockHolder lh(storage.getMutex(bucket_num));
    StoredValue *v = storage.unlocked_find(key, bucket_num);

    found = (v != NULL);
    if (found) {
        kstats.dirty = v->isDirty();
        kstats.exptime = v->getExptime();
        kstats.flags = v->getFlags();
        kstats.cas = v->getCas();
        kstats.dirtied = v->getDirtied();
        kstats.data_age = v->getDataAge();
    }
    return found;
}

void EventuallyPersistentStore::setMinDataAge(int to) {
    stats.min_data_age.set(to);
}

void EventuallyPersistentStore::setQueueAgeCap(int to) {
    stats.queue_age_cap.set(to);
}

void EventuallyPersistentStore::resetStats(void) {
    stats.tooYoung.set(0);
    stats.tooOld.set(0);
    stats.dirtyAge.set(0);
    stats.dirtyAgeHighWat.set(0);
    stats.flushDuration.set(0);
    stats.flushDurationHighWat.set(0);
    stats.commit_time.set(0);
}

void EventuallyPersistentStore::del(const std::string &key, Callback<bool> &cb) {
    bool existed = storage.del(key);
    if (existed) {
        queueDirty(key);
        stats.curr_items.decr();
    }
    cb.callback(existed);
}

std::queue<std::string>* EventuallyPersistentStore::beginFlush() {
    std::queue<std::string> *rv(NULL);
    if (towrite.empty() && writing.empty()) {
        stats.dirtyAge = 0;
    } else {
        assert(underlying);
        towrite.getAll(writing);
        stats.flusher_todo.set(writing.size());
        stats.queue_size.set(towrite.size());
        getLogger()->log(EXTENSION_LOG_DEBUG, NULL,
                         "Flushing %d items with %d still in queue\n",
                         writing.size(), towrite.size());
        rv = &writing;
    }
    return rv;
}

void EventuallyPersistentStore::completeFlush(std::queue<std::string> *rej,
                                              rel_time_t flush_start) {
    // Requeue the rejects.
    stats.queue_size += rej->size();
    while (!rej->empty()) {
        writing.push(rej->front());
        rej->pop();
    }

    stats.queue_size.set(towrite.size() + writing.size());
    rel_time_t complete_time = ep_current_time();
    stats.flushDuration.set(complete_time - flush_start);
    stats.flushDurationHighWat.set(std::max(stats.flushDuration.get(),
                                            stats.flushDurationHighWat.get()));
}

int EventuallyPersistentStore::flushSome(std::queue<std::string> *q,
                                         std::queue<std::string> *rejectQueue) {
    int tsz = getTxnSize();
    underlying->begin();
    int oldest = stats.min_data_age;
    for (int i = 0; i < tsz && !q->empty(); i++) {
        int n = flushOne(q, rejectQueue);
        if (n != 0 && n < oldest) {
            oldest = n;
        }
    }
    rel_time_t cstart = ep_current_time();
    while (!underlying->commit()) {
        sleep(1);
        stats.commitFailed.incr();
    }
    rel_time_t complete_time = ep_current_time();

    stats.commit_time.set(complete_time - cstart);

    return oldest;
}


// This class exists to create a closure around a few variables within
// EventuallyPersistentStore::flushOne so that an object can be
// requeued in case of failure to store in the underlying layer.

class Requeuer : public Callback<bool> {
public:

    Requeuer(const std::string k, std::queue<std::string> *q,
             StoredValue *v, rel_time_t qd, rel_time_t d, struct EPStats *s) :
        key(k), rq(q), sval(v), queued(qd), dirtied(d), stats(s) {
        assert(rq);
        assert(s);
    }

    void callback(bool &value) {
        if (!value) {
            stats->flushFailed.incr();
            if (sval != NULL) {
                sval->reDirty(queued, dirtied);
            }
            rq->push(key);
        }
    }
private:
    const std::string key;
    std::queue<std::string> *rq;
    StoredValue *sval;
    rel_time_t queued;
    rel_time_t dirtied;
    struct EPStats *stats;
    DISALLOW_COPY_AND_ASSIGN(Requeuer);
};

int EventuallyPersistentStore::flushOne(std::queue<std::string> *q,
                                        std::queue<std::string> *rejectQueue) {

    std::string key = q->front();
    q->pop();

    int bucket_num = storage.bucket(key);
    LockHolder lh(storage.getMutex(bucket_num));
    StoredValue *v = storage.unlocked_find(key, bucket_num);

    bool found = v != NULL;
    bool isDirty = (found && v->isDirty());
    Item *val = NULL;
    rel_time_t queued(0), dirtied(0);

    int ret = 0;

    if (isDirty) {
        v->markClean(&queued, &dirtied);
        assert(dirtied > 0);
        // Calculate stats if this had a positive time.
        rel_time_t now = ep_current_time();
        int dataAge = now - dirtied;
        int dirtyAge = now - queued;
        bool eligible = true;

        if (dirtyAge > stats.queue_age_cap.get()) {
            stats.tooOld.incr();
        } else if (dataAge < stats.min_data_age.get()) {
            eligible = false;
            // Skip this one.  It's too young.
            ret = stats.min_data_age.get() - dataAge;
            isDirty = false;
            stats.tooYoung.incr();
            v->reDirty(queued, dirtied);
            rejectQueue->push(key);
        }

        if (eligible) {
            assert(dirtyAge < (86400 * 30));
            assert(dataAge <= dirtyAge);
            stats.dirtyAge.set(dirtyAge);
            stats.dataAge.set(dataAge);
            stats.dirtyAgeHighWat.set(std::max(stats.dirtyAge.get(),
                                               stats.dirtyAgeHighWat.get()));
            stats.dataAgeHighWat.set(std::max(stats.dataAge.get(),
                                              stats.dataAgeHighWat.get()));
            // Copy it for the duration.
            val = new Item(key, v->getFlags(), v->getExptime(), v->getValue(),
                           v->getCas());

            // Consider this persisted as it is our intention, though
            // it may fail and be requeued later.
            stats.totalPersisted.incr();
        }
    }
    stats.flusher_todo.decr();
    lh.unlock();

    if (found && isDirty) {
        Requeuer cb(key, rejectQueue, v, queued, dirtied, &stats);
        underlying->set(*val, cb);
    } else if (!found) {
        Requeuer cb(key, rejectQueue, v, queued, dirtied, &stats);
        underlying->del(key, cb);
    }

    if (val != NULL) {
        delete val;
    }

    return ret;
}

void EventuallyPersistentStore::queueDirty(const std::string &key) {
    if (doPersistence) {
        // Assume locked.
        towrite.push(key);
        stats.totalEnqueued++;
        stats.queue_size = towrite.size();
    }
}
