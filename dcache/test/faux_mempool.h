#pragma once
#include <cstdint>
#include <cstdlib>

#include <cstring>
#include <remus/logging/logging.h>
#include <remus/rdma/rdma.h>

using namespace remus::rdma;
using namespace std;

class CountingPool {
    struct AsyncJob {
        rdma_ptr<uint64_t> org;
        uint64_t new_val;
        AsyncJob(rdma_ptr<uint64_t> org, uint64_t new_val) : org(org), new_val(new_val){}
    };
    unordered_map<void*, int> allocat; // ptr to size
    unordered_map<int, std::vector<AsyncJob>*> async_jobs;
    int total_allocations;
    int total_deallocations;
    bool locality;
    mutex mu;

public:
    CountingPool(bool all_local) : locality(all_local), total_allocations(0), total_deallocations(0) {}

    template <typename T>
    rdma_ptr<T> Allocate(int size = 1){
        mu.lock();
        total_allocations++;
        int bytes = sizeof(T) * size;
        T* p = (T*) malloc(bytes);
        allocat[(void*) p] = bytes;
        mu.unlock();
        return rdma_ptr<T>(0, p);
    }

    template <typename T>
    void Deallocate(rdma_ptr<T> p, int size = 1){
        mu.lock();
        total_deallocations++;
        REMUS_ASSERT(p != nullptr, "Deallocating a nullptr");
        int bytes = sizeof(T) * size;
        void* add = (void*) p.address();
        REMUS_ASSERT(allocat.find(add) != allocat.end(), "Found double free with ptr {}", p);
        REMUS_ASSERT(allocat.at(add) == bytes, "Found free with ptr {} with wrong size (actual={} != freed={}) {}/{}", p, allocat.at(add), bytes, sizeof(T), size);
        allocat.erase(add);
        free((T*) p.address());
        mu.unlock();
    }

    template <typename T>
    rdma_ptr<T> Read(rdma_ptr<T> p, rdma_ptr<T> prealloc = nullptr){
        return ExtendedRead(p, 1, prealloc);
    }

    template <typename T>
    rdma_ptr<T> ExtendedRead(rdma_ptr<T> p, int size, rdma_ptr<T> prealloc = nullptr){
        if (p == nullptr) {
            REMUS_ERROR("p is a nullptr");
            abort();
        }
        if (prealloc == p){
            REMUS_ERROR("prealloc == p (read)");
            abort();
        }
        if (prealloc == nullptr){
            rdma_ptr<T> p_new = Allocate<T>(size);
            mu.lock();
            memcpy(p_new.get(), p.get(), sizeof(T) * size);
            mu.unlock();
            return p_new;
        } else {
            // read into prealloc instead of internally allocating
            mu.lock();
            memcpy(prealloc.get(), p.get(), sizeof(T) * size);
            mu.unlock();
            return prealloc;
        }        
    }

    template <typename T>
    void Write(rdma_ptr<T> ptr, const T& val, rdma_ptr<T> prealloc = nullptr, internal::RDMAWriteBehavior write_behavior = internal::RDMAWriteWithAck){
        if (prealloc == ptr){
            REMUS_ERROR("prealloc == p (write)");
            abort();
        }
        mu.lock();
        if (prealloc == nullptr){
            // removed the un-necessary allocate and deallocate step...
            *ptr = val;
        } else {
            // might rely on property that prealloc is written val before it gets written to the "remote" ptr
            *prealloc = val;
            *ptr = *prealloc;
        }
        mu.unlock();
    }

    template <typename T>
    T CompareAndSwap(rdma_ptr<T> ptr, uint64_t expected, uint64_t swap) {
        mu.lock();
        uint64_t prev = *ptr;
        if (prev == expected){
            *ptr = swap;
        }
        mu.unlock();
        return prev;
    }

    void CompareAndSwapAsync(rdma_ptr<uint64_t> ptr, rdma_ptr<uint64_t> result, uint64_t expected, uint64_t swap){
        mu.lock();
        if (async_jobs.find(ptr.id()) == async_jobs.end()){
            async_jobs[ptr.id()] = new std::vector<AsyncJob>();
        }        
        uint64_t prev = *ptr;
        async_jobs[ptr.id()]->push_back(AsyncJob(result, prev));
        if (prev == expected){
            *ptr = swap;
        }
        mu.unlock();
    }

    void Await(uint64_t id){
        mu.lock();
        // update the ptrs for sake of catching bugs if I try to use prealloc result immediately
        if (async_jobs.find(id) != async_jobs.end()){
            AsyncJob job = async_jobs[id]->back();
            *job.org = job.new_val;
            async_jobs[id]->pop_back();
        }        
        mu.unlock();
    }

    template <typename T>
    bool is_local(rdma_ptr<T> p){
        return locality;
    }

    bool HasNoLeaks(){
        mu.lock();
        int size = allocat.size();
        mu.unlock();
        return size == 0;
    }

    void debug(){
        REMUS_WARN("Total allocations {}", total_allocations);
        REMUS_WARN("Total deallocations {}", total_deallocations);
        for(auto it = allocat.begin(); it != allocat.end(); it++){
            REMUS_WARN("{} was not freed", it->first);
        }
    }
};