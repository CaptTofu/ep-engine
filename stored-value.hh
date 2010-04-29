/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
#ifndef STORED_VALUE_H
#define STORED_VALUE_H 1

#include "locks.hh"

extern "C" {
    extern rel_time_t (*ep_current_time)();
}

// Forward declaration for StoredValue
class HashTable;

class StoredValue {
public:
    StoredValue() : dirtied(0), next(NULL) {}

    StoredValue(shared_ptr<const Item> itm, StoredValue *n) :
        item(itm), dirtied(0), next(n)
    {
        markDirty();
    }

    StoredValue(shared_ptr<const Item> itm, StoredValue *n, bool setDirty) :
        item(itm), dirtied(0), next(n)
    {
        if (setDirty) {
            markDirty();
        } else {
            markClean(NULL, NULL);
        }
    }

    ~StoredValue() {
    }

    void markDirty() {
        data_age = ep_current_time();
        if (!isDirty()) {
            dirtied = data_age;
        }
    }

    void reDirty(rel_time_t dirtyAge, rel_time_t dataAge) {
        data_age = dataAge;
        dirtied = dirtyAge;
    }

    // returns time this object was dirtied.
    void markClean(rel_time_t *dirtyAge, rel_time_t *dataAge) {
        if (dirtyAge) {
            *dirtyAge = dirtied;
        }
        if (dataAge) {
            *dataAge = data_age;
        }
        dirtied = 0;
        data_age = 0;
    }

    bool isDirty() const {
        return dirtied != 0;
    }

    bool isClean() const {
        return dirtied == 0;
    }

    const shared_ptr<const Item> getItem() const {
        return item;
    }

    void setItem(shared_ptr<const Item> itm) {
        item = itm;
        markDirty();
    }

private:

    friend class HashTable;

    shared_ptr<const Item> item;
    rel_time_t dirtied;
    rel_time_t data_age;
    StoredValue *next;
    DISALLOW_COPY_AND_ASSIGN(StoredValue);
};

typedef enum {
    NOT_FOUND, INVALID_CAS, WAS_CLEAN, WAS_DIRTY
} mutation_type_t;

class HashTableVisitor {
public:
    virtual ~HashTableVisitor() {}
    virtual void visit(StoredValue *v) = 0;
};

class HashTable {
public:

    // Construct with number of buckets and locks.
    HashTable(size_t s = 196613, size_t l = 193) {
        size = s;
        n_locks = l;
        active = true;
        values = (StoredValue**)calloc(s, sizeof(StoredValue*));
        mutexes = new Mutex[l];
    }

    ~HashTable() {
        clear();
        delete []mutexes;
        free(values);
    }

    void clear() {
        assert(active);
        for (int i = 0; i < (int)size; i++) {
            LockHolder lh(getMutex(i));
            while (values[i]) {
                StoredValue *v = values[i];
                values[i] = v->next;
                delete v;
            }
        }
    }

    StoredValue *find(std::string &key) {
        assert(active);
        int bucket_num = bucket(key);
        LockHolder lh(getMutex(bucket_num));
        return unlocked_find(key, bucket_num);
    }

    mutation_type_t set(shared_ptr<const Item> val) {
        assert(active);
        mutation_type_t rv = NOT_FOUND;
        int bucket_num = bucket(val->getKey());
        LockHolder lh(getMutex(bucket_num));
        Item &itm = const_cast<Item&>(*val);
        StoredValue *v = unlocked_find(val->getKey(), bucket_num);
        if (v) {
            if (val->getCas() != 0 && val->getCas() != v->getItem()->getCas()) {
                return INVALID_CAS;
            }
            itm.setCas();
            rv = v->isClean() ? WAS_CLEAN : WAS_DIRTY;
            v->setItem(val);
        } else {
            if (val->getCas() != 0) {
                return INVALID_CAS;
            }
            itm.setCas();
            v = new StoredValue(val, values[bucket_num]);
            values[bucket_num] = v;
        }
        return rv;
    }

    bool add(shared_ptr<const Item> val, bool isDirty = true) {
        assert(active);
        int bucket_num = bucket(val->getKey());
        LockHolder lh(getMutex(bucket_num));
        StoredValue *v = unlocked_find(val->getKey(), bucket_num);
        if (v) {
            return false;
        } else {
            Item &itm = const_cast<Item&>(*val);
            itm.setCas();
            v = new StoredValue(val, values[bucket_num], isDirty);
            values[bucket_num] = v;
        }

        return true;
    }

    StoredValue *unlocked_find(const std::string &key, int bucket_num) {
        StoredValue *v = values[bucket_num];
        while (v) {
            if (key.compare(v->item->getKey()) == 0) {
                return v;
            }
            v = v->next;
        }
        return NULL;
    }

    inline int bucket(const std::string &key) {
        assert(active);
        int h=5381;
        int i=0;
        const char *str = key.c_str();

        for(i=0; str[i] != 0x00; i++) {
            h = ((h << 5) + h) ^ str[i];
        }

        return abs(h) % (int)size;
    }

    // Get the mutex for a bucket (for doing your own lock management)
    inline Mutex &getMutex(int bucket_num) {
        assert(active);
        assert(bucket_num < (int)size);
        assert(bucket_num >= 0);
        int lock_num = bucket_num % (int)n_locks;
        assert(lock_num < (int)n_locks);
        assert(lock_num >= 0);
        return mutexes[lock_num];
    }

    // True if it existed
    bool del(const std::string &key) {
        assert(active);
        int bucket_num = bucket(key);
        LockHolder lh(getMutex(bucket_num));

        StoredValue *v = values[bucket_num];

        // Special case empty bucket.
        if (!v) {
            return false;
        }

        // Special case the first one
        if (key.compare(v->getItem()->getKey()) == 0) {
            values[bucket_num] = v->next;
            delete v;
            return true;
        }

        while (v->next) {
            if (key.compare(v->next->getItem()->getKey()) == 0) {
                StoredValue *tmp = v->next;
                v->next = v->next->next;
                delete tmp;
                return true;
            } else {
                v = v->next;
            }
        }

        return false;
    }

    void visit(HashTableVisitor &visitor) {
        for (int i = 0; i < (int)size; i++) {
            LockHolder lh(getMutex(i));
            StoredValue *v = values[i];
            while (v) {
                visitor.visit(v);
                v = v->next;
            }
        }
    }

private:
    size_t            size;
    size_t            n_locks;
    bool              active;
    StoredValue     **values;
    Mutex            *mutexes;

    DISALLOW_COPY_AND_ASSIGN(HashTable);
};

#endif /* STORED_VALUE_H */
