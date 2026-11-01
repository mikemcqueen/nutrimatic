#ifndef NUTRIMATIC_RUN_THREADS_H
#define NUTRIMATIC_RUN_THREADS_H

#include <cstddef>
#include <thread>
#include <vector>

// Runs work(t) for each t from 0 to `threads` - 1: 0 on the calling thread,
// the others each on a thread of its own, started before it. A t whose thread
// can't be started runs on the calling thread after work(0). Returns how many
// threads ran work at once: 1 plus those started.
template <typename Work>
size_t run_threads(size_t threads, Work const& work) {
    std::vector<std::thread> workers;
    size_t started = 1;
    try {
        if (threads > 1) workers.reserve(threads - 1);
        for (; started < threads; ++started)
            workers.emplace_back(work, started);
    } catch (...) {
    }
    work(size_t(0));
    for (size_t t = started; t < threads; ++t) work(t);
    for (std::thread& worker : workers) worker.join();
    return workers.size() + 1;
}

#endif
