/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
#ifndef DISPATCHER_H
#define DISPATCHER_H

#include <stdexcept>
#include <queue>

#include "common.hh"
#include "mutex.hh"
#include "syncobject.hh"

#if defined(HAVE_MEMORY)
# include <memory>
#endif
#if defined(HAVE_TR1_MEMORY)
# include <tr1/memory>
#endif
#if defined(HAVE_BOOST_SHARED_PTR_HPP)
# include <boost/shared_ptr.hpp>
#endif

#if defined(SHARED_PTR_NAMESPACE)
using SHARED_PTR_NAMESPACE::shared_ptr;
using SHARED_PTR_NAMESPACE::weak_ptr;
#else
# error No shared pointer implementation found!
#endif

class Dispatcher;

extern "C" {
    static void* launch_dispatcher_thread(void* arg);
}

enum task_state {
    task_dead,
    task_running,
    task_sleeping
};

enum dispatcher_state {
    dispatcher_running,
    dispatcher_stopping,
    dispatcher_stopped
};

static bool less_tv(const struct timeval &tv1, const struct timeval &tv2) {
    if (tv1.tv_sec == tv2.tv_sec) {
        return tv1.tv_usec < tv2.tv_usec;
    } else {
        return tv1.tv_sec < tv2.tv_sec;
    }
}

class Task;

typedef weak_ptr<Task> TaskId;

class DispatcherCallback {
public:
    virtual ~DispatcherCallback() {}
    virtual bool callback(Dispatcher &d, TaskId t) = 0;
};

class Task {
public:
    ~Task() { }

private:
    Task(DispatcherCallback *cb, int p=0, double sleeptime=0) :
         callback(cb), priority(p) {
        if (sleeptime > 0) {
            snooze(sleeptime);
        } else {
            state = task_running;
        }
    }

    Task(const Task &task) {
        priority = task.priority;
        waketime = task.waketime;
        state = task_running;
        callback = task.callback;
    }

    bool operator<(const Task &other) const {
        if (state == task_running) {
            if (other.state == task_running) {
                return priority < other.priority;
            } else if (other.state == task_sleeping) {
                return true;
            }
        } else if (state == task_sleeping && other.state == task_sleeping) {
            return less_tv(waketime, other.waketime);
        }
        return false;
    }

    void snooze(const double secs) {
        LockHolder lh(mutex);
        gettimeofday(&waketime, NULL);
        double ip, fp;
        fp = modf(secs, &ip);
        int usec = (fp * 1e6) + waketime.tv_usec;
        div_t d = div(usec, 1000000);
        waketime.tv_sec += ip + d.quot;
        waketime.tv_usec = d.rem;
        state = task_sleeping;
    }

    bool run(Dispatcher &d, TaskId t) {
        return callback->callback(d, t);
    }

    void kill() {
        LockHolder lh(mutex);
        state = task_dead;
    }

    friend class Dispatcher;
    std::string name;
    struct timeval waketime;
    DispatcherCallback *callback;
    int priority;
    enum task_state state;
    Mutex mutex;
};

class Dispatcher {
public:
    Dispatcher() : state(dispatcher_running) { }

    ~Dispatcher() {
        stop();
    }

    TaskId schedule(DispatcherCallback *callback, int priority=0,
                    double sleeptime=0) {
        LockHolder lh(mutex);
        shared_ptr<Task> task(new Task(callback, priority, sleeptime));
        queue.push(task);
        mutex.notify();
        return TaskId(task);
    }

    TaskId schedule(TaskId task) {
        kill(task);
        shared_ptr<Task> oldTask(task);
        shared_ptr<Task> newTask(new Task(*oldTask));
        queue.push(newTask);
        mutex.notify();
        return TaskId(newTask);
    }

    void start() {
        if(pthread_create(&thread, NULL, launch_dispatcher_thread, this) != 0) {
            throw std::runtime_error("Error initializing dispatcher thread");
        }
    }

    void run() {
        getLogger()->log(EXTENSION_LOG_DEBUG, NULL, "Dispatcher starting\n");
        while (state == dispatcher_running) {
            getLogger()->log(EXTENSION_LOG_DEBUG, NULL, "Entering dispatcher loop\n");
            LockHolder lh(mutex);
            if (queue.empty()) {
                // Wait forever
                mutex.wait();
            } else {
                shared_ptr<Task> task = queue.top();
                LockHolder tlh(task->mutex);
                switch (task->state) {
                case task_sleeping:
                    {
                        struct timeval tv;
                        gettimeofday(&tv, NULL);
                        if (less_tv(tv, task->waketime)) {
                            tlh.unlock();
                            mutex.wait(task->waketime);
                        } else {
                            task->state = task_running;
                        }
                    }
                    break;
                case task_running:
                    queue.pop();
                    lh.unlock();
                    tlh.unlock();
                    try {
                        if(task->run(*this, TaskId(task))) {
                            schedule(task);
                        }
                    } catch (std::exception& e) {
                        std::cerr << "exception caught in task " << task->name << ": " << e.what() << std::endl;
                    } catch(...) {
                        std::cerr << "Caught a fatal exception in task" << task->name <<std::endl;
                    }
                    break;
                case task_dead:
                    queue.pop();
                    break;
                default:
                    throw std::runtime_error("Unexpected state for task");
                }
            }
        }

        state = dispatcher_stopped;
        mutex.notify();
        getLogger()->log(EXTENSION_LOG_DEBUG, NULL, "Dispatcher exited\n");
    }

    void stop() {
        LockHolder lh(mutex);
        if (state == dispatcher_stopped) {
            return;
        }
        getLogger()->log(EXTENSION_LOG_DEBUG, NULL, "Stopping dispatcher\n");
        state = dispatcher_stopping;
        mutex.notify();
        while (state != dispatcher_stopped) {
            assert(state == dispatcher_stopping);
            mutex.wait();
        }
        getLogger()->log(EXTENSION_LOG_DEBUG, NULL, "Dispatcher stopped\n");
    }

    void snooze(TaskId t, double sleeptime) {
        shared_ptr<Task>task(t);
        task->snooze(sleeptime);
    }

    void kill(TaskId t) {
        shared_ptr<Task> task = t.lock();
        if (task) {
            task->kill();
        }
    }

private:
    pthread_t thread;
    SyncObject mutex;
    std::priority_queue<shared_ptr<Task>, std::deque<shared_ptr<Task> > > queue;
    enum dispatcher_state state;
};

static void* launch_dispatcher_thread(void *arg) {
    Dispatcher *dispatcher = (Dispatcher*) arg;
    try {
        dispatcher->run();
    } catch (std::exception& e) {
        getLogger()->log(EXTENSION_LOG_WARNING, NULL, "dispatcher exception caught: %s\n",
                         e.what());
    } catch(...) {
        getLogger()->log(EXTENSION_LOG_WARNING, NULL,
                         "Caught a fatal exception in the dispatcher thread\n");
    }
    return NULL;
}

#endif
