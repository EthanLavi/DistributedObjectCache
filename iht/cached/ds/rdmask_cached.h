#pragma once

#include <cstddef>
#include <cstdint>
#include <ostream>
#include <random>
#include <remus/logging/logging.h>
#include <remus/rdma/memory_pool.h>
#include <remus/rdma/rdma.h>

#include <dcache/cache_store.h>
#include <dcache/cached_ptr.h>
#include <dcache/mark_ptr.h>

#include "ebr.h"
#include "skiplist_shared.h"
#include "../../common.h"

#include <optional>
#include <thread>
#include <random>

using namespace remus::rdma;

template <class T>
inline bool is_marked_del(rdma_ptr<T> ptr){
    return ptr.raw() & 0x1;
}

template <class T>
inline rdma_ptr<T> marked_del(rdma_ptr<T> ptr){
    return rdma_ptr<T>(ptr.raw() | 0x1);
}

/// A ptr without the marking (ptr "sans" marking)
template <class T>
inline rdma_ptr<T> sans(rdma_ptr<T> ptr){
    return rdma_ptr<T>(ptr.raw() & ~0x1);
}

/// SIZE is DEGREE * 2
template <class K, int MAX_HEIGHT, K MINKEY, uint64_t DELETE_SENTINEL, uint64_t UNLINK_SENTINEL, class capability> class RdmaSkipList {
private:
    using RemoteCache = RemoteCacheImpl<capability>;

    Peer self_;
    int cache_floor_;

    typedef node<K, MAX_HEIGHT> Node;
    typedef rdma_ptr<Node> nodeptr;

    template <typename T> inline bool is_local(rdma_ptr<T> ptr) {
        return ptr.id() == self_.id;
    }

    nodeptr root;
    RemoteCache* cache;
  
    // preallocated memory for RDMA operations (avoiding frequent allocations)
    rdma_ptr<Node> prealloc_node_w;
    rdma_ptr<Node> prealloc_fill_node1;
    rdma_ptr<Node> prealloc_fill_node2;
    rdma_ptr<Node> prealloc_find_node1;
    rdma_ptr<Node> prealloc_find_node2;
    rdma_ptr<Node> prealloc_helper_node;
    rdma_ptr<Node> prealloc_count_node;
    // shared EBRObjectPool has thread_local internals so its all thread safe
    EBRObjectPool<Node, 100, capability>* ebr;

    inline rdma_ptr<uint64_t> get_value_ptr(nodeptr node) {
        return rdma_ptr<uint64_t>(unmark_ptr(node).id(), unmark_ptr(node).address() + offsetof(Node, value));
    }

    inline rdma_ptr<uint64_t> get_link_height_ptr(nodeptr node) {
        return rdma_ptr<uint64_t>(unmark_ptr(node).id(), unmark_ptr(node).address() + offsetof(Node, link_level));
    }

    inline rdma_ptr<uint64_t> get_level_ptr(nodeptr node, int level) {          
        Node* tmp = (Node*) 0x0;
        return rdma_ptr<uint64_t>(unmark_ptr(node).id(), unmark_ptr(node).address() + (uint64_t) &tmp->next[level]);
    }

    CachedObject<Node> fill(K key, rdma_ptr<Node> preds[MAX_HEIGHT], rdma_ptr<Node> succs[MAX_HEIGHT], bool found[MAX_HEIGHT], K prev_keys[MAX_HEIGHT]){
        // first node is a sentinel, it will always be linked in the data structure
        CachedObject<Node> curr = cache->template Read<Node>(root, prealloc_fill_node1, -1); // root is never deleted, let's say curr is never a deleted node
        CachedObject<Node> next_curr;
        bool use_node1 = false;
        for(int height = MAX_HEIGHT - 1; height != -1; height--){
            // iterate on this level until we find the last node that <= the key
            while(true){
                if (sans(curr->next[height]) == nullptr) {
                    preds[height] = curr.remote_origin();
                    prev_keys[height] = curr->key;
                    succs[height] = nullptr;
                    found[height] = false;
                    // if next is the END, descend a level
                    break;
                }
                next_curr = cache->template Read<Node>(sans(curr->next[height]), use_node1 ? prealloc_fill_node1 : prealloc_fill_node2, MAX_HEIGHT - curr->height);
                if (next_curr->key < key) {
                    curr = std::move(next_curr);
                    use_node1 = !use_node1; // swap the prealloc
                    continue; // move right
                } else if (next_curr->key == key){
                    preds[height] = curr.remote_origin();
                    prev_keys[height] = curr->key;
                    succs[height] = next_curr->next[height];
                    found[height] = true;
                } else {
                    preds[height] = curr.remote_origin();
                    prev_keys[height] = curr->key;
                    succs[height] = next_curr.remote_origin();
                    found[height] = false;
                }
                // descend a level
                break;
            }
        }
        return next_curr;
    }

    /// Try to physically unlink a node that we know exists and we have the responsibility of unlinking
    void unlink_node(capability* pool, K key, int retries = 10000){
        if (retries == 0) {
            REMUS_ERROR("Too many retries. Ignore the node {} forever?", key);
            return;
        }
        if (key == MINKEY) return;
        // REMUS_ASSERT_DEBUG(key != MINKEY, "Removing the root node shouldn't happen");
        rdma_ptr<Node> preds[MAX_HEIGHT];
        rdma_ptr<Node> succs[MAX_HEIGHT];
        bool found[MAX_HEIGHT];
        K prev_keys[MAX_HEIGHT];
        CachedObject<Node> node = fill(key, preds, succs, found, prev_keys);
        // if (!found[0]) return; // cannot find the node, (someone else might have removed for me too!)
        
        for(int height = MAX_HEIGHT - 1; height != -1; height--){
            if (!found[height]) continue;
            // if not marked, try to mark
            if (!is_marked_del(succs[height])) {
                rdma_ptr<uint64_t> level_ptr = get_level_ptr(node.remote_origin(), height);
                // mark for deletion to prevent inserts after and also delete issues?
                uint64_t old_ptr = pool->template CompareAndSwap<uint64_t>(level_ptr, succs[height].raw(), marked_del(succs[height]).raw());
                if (old_ptr != succs[height].raw()){
                    // cas failed, retry
                    return unlink_node(pool, key, retries - 1); // retry
                }
                cache->Invalidate(node.remote_origin());
            }

            // physically unlink
            REMUS_ASSERT(!is_marked_del(preds[height]), "Shouldn't be marked");
            REMUS_ASSERT(!is_marked_del(node.remote_origin()), "Shouldn't be marked");
            rdma_ptr<uint64_t> level_ptr = get_level_ptr(preds[height], height);
            // only remove the level if prev is not marked
            uint64_t old_ptr = pool->template CompareAndSwap<uint64_t>(level_ptr, sans(node.remote_origin()).raw(), sans(succs[height]).raw());
            if (old_ptr != node.remote_origin().raw()){
                return unlink_node(pool, key, retries - 1); // retry
            }
            cache->Invalidate(preds[height]);
        }
    }

    /// Try to raise a node to the level
    void raise_node(capability* pool, K key, int goal_height){
        rdma_ptr<Node> preds[MAX_HEIGHT];
        rdma_ptr<Node> succs[MAX_HEIGHT];
        bool found[MAX_HEIGHT];
        K prev_keys[MAX_HEIGHT];
        CachedObject<Node> node = fill(key, preds, succs, found, prev_keys);

        for(int height = 0; height < goal_height; height++){
            if (found[height]) continue; // if reachable from a height, continue

            REMUS_ASSERT(!is_marked_del(node->next[height]), "Shouldn't be marked");
            rdma_ptr<uint64_t> level_ptr = get_level_ptr(node.remote_origin(), height);
            uint64_t old_ptr = pool->template CompareAndSwap<uint64_t>(level_ptr, node->next[height].raw(), succs[height].raw());
            if (old_ptr != node->next[height].raw()){
                return raise_node(pool, key, goal_height); // retry b/c cas failed
            }
            cache->Invalidate(node.remote_origin()); // invalidate the raising node

            // Update previous node
            REMUS_ASSERT(!is_marked_del(preds[height]), "Shouldn't be marked");
            REMUS_ASSERT(!is_marked_del(node.remote_origin()), "Shouldn't be marked");
            level_ptr = get_level_ptr(preds[height], height);
            old_ptr = pool->template CompareAndSwap<uint64_t>(level_ptr, sans(succs[height]).raw(), node.remote_origin().raw());
            if (old_ptr != sans(succs[height]).raw()){
                return raise_node(pool, key, goal_height); // retry
            }
            cache->Invalidate(preds[height]);
        }

        // raise link level
        pool->template CompareAndSwap<uint64_t>(get_link_height_ptr(node.remote_origin()), 1, goal_height);
        cache->Invalidate(node.remote_origin());
    }

public:
    RdmaSkipList(Peer& self, int cache_floor, RemoteCache* cache, capability* pool, vector<Peer> peers, EBRObjectPool<Node, 100, capability>* ebr) 
    : self_(std::move(self)), cache_floor_(cache_floor), cache(cache), ebr(ebr) {
        REMUS_INFO("SENTINEL is MIN? {}", MINKEY);
        prealloc_node_w = pool->template Allocate<Node>();
        prealloc_count_node = pool->template Allocate<Node>();
        prealloc_fill_node1 = pool->template Allocate<Node>();
        prealloc_fill_node2 = pool->template Allocate<Node>();
        prealloc_find_node1 = pool->template Allocate<Node>();
        prealloc_find_node2 = pool->template Allocate<Node>();
        prealloc_helper_node = pool->template Allocate<Node>();
    }

    void helper_thread(std::atomic<bool>* do_cont, capability* pool, EBRObjectPool<Node, 100, capability>* ebr_helper, vector<LimboLists<Node>*> qs){
        CachedObject<Node> curr;
        int i = 0;
        while(do_cont->load()){ // endlessly traverse and maintain the index
            curr = cache->template Read<Node>(root, prealloc_helper_node, -1);
            while(sans(curr->next[0]) != nullptr && do_cont->load()){
                curr = cache->template Read<Node>(sans(curr->next[0]), prealloc_helper_node, MAX_HEIGHT - curr->height);
                if (curr->value == DELETE_SENTINEL && curr->link_level == curr->height){
                    rdma_ptr<uint64_t> ptr = get_value_ptr(curr.remote_origin());
                    uint64_t last = pool->template CompareAndSwap<uint64_t>(ptr, DELETE_SENTINEL, UNLINK_SENTINEL);
                    if (last != DELETE_SENTINEL) continue; // something changed, gonna skip this node
                    cache->Invalidate(curr.remote_origin());
                    unlink_node(pool, curr->key); // physically unlink (has guarantee of unlink)
                    
                    cache->Invalidate(curr.remote_origin()); // invalidate since curr has now changed
                    curr = cache->template Read<Node>(curr.remote_origin(), nullptr, MAX_HEIGHT - curr->height); // refresh curr

                    // equivalent to ebr->deallocate unlinked node
                    LimboLists<Node>* q = qs.at(i);
                    q->free_lists[2].load()->push(unmark_ptr(curr.remote_origin())); // todo: allow it to be marked?

                    // cycle the index
                    i++;
                    i %= qs.size();
                } else if (curr->value == UNLINK_SENTINEL){
                    continue; // someone else is unlinking
                } else {
                    // Hasn't been raised yet and isn't in the process or raising (Test-Test-And-Set)
                    if (curr->link_level == 0 && curr->height > 1){
                        // Raise the node
                        int old_height = pool->template CompareAndSwap<uint64_t>(get_link_height_ptr(curr.remote_origin()), 0, 1);
                        if (old_height == 0){
                            cache->Invalidate(curr.remote_origin());
                            raise_node(pool, curr->key, curr->height);
                            
                            // refresh curr, now that it has changed
                            cache->Invalidate(curr.remote_origin());
                            curr = cache->template Read<Node>(curr.remote_origin(), nullptr, MAX_HEIGHT - curr->height); 
                        }
                    } else {
                        int old_height = pool->template CompareAndSwap<uint64_t>(get_link_height_ptr(curr.remote_origin()), 0, 1);
                        if (old_height == curr->link_level){
                            cache->Invalidate(curr.remote_origin());
                        }
                    }
                }
            }
            ebr_helper->match_version(pool, true); // indicate done with epoch
        }
    }

    /// Free all the resources associated with the data structure
    void destroy(capability* pool, bool delete_as_first = true) {
        pool->template Deallocate<Node>(prealloc_count_node);
        pool->template Deallocate<Node>(prealloc_fill_node1);
        pool->template Deallocate<Node>(prealloc_fill_node2);
        pool->template Deallocate<Node>(prealloc_node_w);
        pool->template Deallocate<Node>(prealloc_find_node1);
        pool->template Deallocate<Node>(prealloc_find_node2);
        pool->template Deallocate<Node>(prealloc_helper_node);
        if (delete_as_first) pool->template Deallocate<Node>(root);
    }

    /// @brief Create a fresh iht
    /// @param pool the capability to init the IHT with
    /// @return the iht root pointer
    rdma_ptr<anon_ptr> InitAsFirst(capability* pool){
        this->root = pool->template Allocate<Node>();
        this->root->key = MINKEY;
        this->root->value = 0;
        for(int i = 0; i < MAX_HEIGHT; i++)
            this->root->next[i] = nullptr;
        this->root = this->root;
        if (cache_floor_ < MAX_HEIGHT) this->root = mark_ptr(this->root);
        return static_cast<rdma_ptr<anon_ptr>>(this->root);
    }

    /// @brief Initialize an IHT from the pointer of another IHT
    /// @param root_ptr the root pointer of the other iht from InitAsFirst();
    void InitFromPointer(rdma_ptr<anon_ptr> root_ptr){
        this->root = static_cast<nodeptr>(root_ptr);
        if (cache_floor_ < MAX_HEIGHT) this->root = mark_ptr(this->root);
    }

    private: void nonblock_unlink_node(capability* pool, K key){
        rdma_ptr<Node> preds[MAX_HEIGHT];
        rdma_ptr<Node> succs[MAX_HEIGHT];
        bool found[MAX_HEIGHT];
        K prev_keys[MAX_HEIGHT];
        CachedObject<Node> node = fill(key, preds, succs, found, prev_keys);
        if (!found[1] && found[0] && node->value == UNLINK_SENTINEL) {
            // physically unlink
            REMUS_ASSERT(!is_marked_del(preds[0]), "Shouldn't be marked");
            rdma_ptr<uint64_t> level_ptr = get_level_ptr(preds[0], 0);
            // only remove the level if prev is not marked
            uint64_t old_ptr = pool->template CompareAndSwap<uint64_t>(level_ptr, sans(node.remote_origin()).raw(), sans(succs[0]).raw());
            if (old_ptr == sans(node.remote_origin()).raw()) {
                cache->Invalidate(preds[0]);
            } else if (old_ptr == marked_del(node.remote_origin()).raw()) {
                // unlink failed because previous was not deleted yet
                REMUS_ASSERT_DEBUG(prev_keys[0] != key, "prev key shouldn't be the current key");
                nonblock_unlink_node(pool, prev_keys[0]);
            }
        }
    } public:

    /// Search for a node where it's result->key is <= key
    /// Will unlink nodes only at the data level leaving them indexable
    private: CachedObject<Node> find(capability* pool, K key, bool is_insert){
        // first node is a sentinel, it will always be linked in the data structure
        CachedObject<Node> curr = cache->template Read<Node>(root, prealloc_find_node1, -1); // root is never deleted, let's say curr is never a deleted node
        bool use_node1 = false;
        CachedObject<Node> next_curr;
        for(int height = MAX_HEIGHT - 1; height != -1; height--){
            // iterate on this level until we find the last node that <= the key
            K last_key = MINKEY;
            while(true){
                if (last_key >= curr->key && last_key != MINKEY) {
                    // REMUS_WARN("Infinite loop height={} prev={} curr={}", height, last_key, curr->key);
                    // abort();
                }
                last_key = curr->key;

                if (curr->key == key) return curr; // stop early if we find the right key
                if (sans(curr->next[height]) == nullptr) break; // if next is the END, descend a level
                next_curr = cache->template Read<Node>(sans(curr->next[height]), use_node1 ? prealloc_find_node1 : prealloc_find_node2, MAX_HEIGHT - curr->height);
                if (is_insert && height == 0 && is_marked_del(curr->next[height]) && next_curr->key >= key && curr->value == UNLINK_SENTINEL){
                    // REMUS_ASSERT_DEBUG(curr->value == UNLINK_SENTINEL, "Should be unlink sentinel if we are removing curr");
                    // we found a node that we are inserting directly after. Lets help unlink
                    nonblock_unlink_node(pool, curr->key);
                    return find(pool, key, is_insert); // recursively retry
                }
                if (next_curr->key <= key) {
                    curr = std::move(next_curr); // next_curr is eligible, continue with it
                    use_node1 = !use_node1;
                }
                else break; // otherwise descend a level since next_curr is past the limit
            }
        }
        return curr;
    } public:

    /// @brief Gets a value at the key.
    /// @param pool the capability providing one-sided RDMA
    /// @param key the key to search on
    /// @return an optional containing the value, if the key exists
    std::optional<uint64_t> contains(capability* pool, K key) {
        CachedObject<Node> node = find(pool, key, false);
        ebr->match_version(pool);
        if (key == node->key && node->value != DELETE_SENTINEL && node->value != UNLINK_SENTINEL) {
            return make_optional(node->value);
        }
        return std::nullopt;
    }

    /// @brief Insert a key and value into the iht. Result will become the value
    /// at the key if already present.
    /// @param pool the capability providing one-sided RDMA
    /// @param key the key to insert
    /// @param value the value to associate with the key
    /// @return an empty optional if the insert was successful. Otherwise it's the value at the key.
    std::optional<uint64_t> insert(capability* pool, K key, uint64_t value) {
        while(true){
            CachedObject<Node> curr = find(pool, key, true);
            if (curr->key == key) {
                if (curr->value == UNLINK_SENTINEL){
                    // if it is in the process of unlinking, help unlink it
                    // unlink_node(pool, key);
                    continue;
                }
                if (curr->value == DELETE_SENTINEL){
                    rdma_ptr<uint64_t> curr_remote_value = get_value_ptr(curr.remote_origin());
                    uint64_t old_value = pool->template CompareAndSwap<uint64_t>(curr_remote_value, DELETE_SENTINEL, value);
                    if (old_value == DELETE_SENTINEL){
                        // cas succeeded, reinstantiated the node
                        cache->Invalidate(curr.remote_origin());
                        ebr->match_version(pool);
                        return std::nullopt;
                    } else if (old_value == UNLINK_SENTINEL) {
                        // cas failed, either someone else inserted or is being unlinked
                        continue;
                    } else {
                        // Someone else re-inserted instead of us
                        ebr->match_version(pool);
                        return make_optional(old_value);
                    }
                } else {
                    // kv already exists
                    ebr->match_version(pool);
                    return make_optional(curr->value);
                }
            }

            // allocates a node
            nodeptr new_node_ptr = ebr->allocate(pool);
            int height = 1;
            if (pool->is_local(new_node_ptr)){
                *new_node_ptr = Node(key, value);
                height = new_node_ptr->height;
                new_node_ptr->next[0] = curr->next[0];
            } else {
                Node new_node = Node(key, value);
                new_node.next[0] = curr->next[0];
                height = new_node.height;
                pool->template Write<Node>(new_node_ptr, new_node, prealloc_node_w);
            }
            // invalidate the new node to ensure the old value isn't in there!
            cache->Invalidate(mark_ptr(new_node_ptr));
            // todo: cache invalidate BEFORE linking into the structure

            // if the next is a deleted node, we need to physically delete
            rdma_ptr<uint64_t> dest = get_level_ptr(curr.remote_origin(), 0);
            nodeptr new_node_ptr_marked = new_node_ptr;
            if (height > cache_floor_){
                new_node_ptr_marked = mark_ptr(new_node_ptr_marked);
            }
            // will fail if the ptr is marked for unlinking
            uint64_t old = pool->template CompareAndSwap<uint64_t>(dest, sans(curr->next[0]).raw(), new_node_ptr_marked.raw());
            if (old == sans(curr->next[0]).raw()){ // if our CAS was successful, invalidate the object we modified
                cache->Invalidate(curr.remote_origin());
                ebr->match_version(pool);
                return std::nullopt;
            } else {
                // the insert failed (either an insert or unlink occured), retry the operation
                ebr->requeue(new_node_ptr); // recycle the data
                continue;
            }
        }
    }

    /// @brief Will remove a value at the key. Will stored the previous value in
    /// result.
    /// @param pool the capability providing one-sided RDMA
    /// @param key the key to remove at
    /// @return an optional containing the old value if the remove was successful. Otherwise an empty optional.
    std::optional<uint64_t> remove(capability* pool, K key) {
        CachedObject<Node> curr = find(pool, key, false);
        if (curr->key != key) {
            // Couldn't find the key, return
            ebr->match_version(pool);
            return std::nullopt;
        }
        if (curr->value == DELETE_SENTINEL || curr->value == UNLINK_SENTINEL) {
            // already removed
            ebr->match_version(pool);
            return std::nullopt;
        }

        // if the next is a deleted node, we need to physically delete
        rdma_ptr<uint64_t> dest = get_value_ptr(curr.remote_origin());
        uint64_t old = pool->template CompareAndSwap<uint64_t>(dest, curr->value, DELETE_SENTINEL); // mark for deletion
        if (old == curr->value){ // if our CAS was successful, invalidate the object we modified
            cache->Invalidate(curr.remote_origin());
            ebr->match_version(pool);
            return make_optional(curr->value);
        } else {
            // the remove failed (a different delete occured), return nullopt
            ebr->match_version(pool);
            return std::nullopt;
        }
    }

    /// @brief Populate only works when we have numerical keys. Will add data
    /// @param pool the capability providing one-sided RDMA
    /// @param op_count the number of values to insert. Recommended in total to do key_range / 2
    /// @param key_lb the lower bound for the key range
    /// @param key_ub the upper bound for the key range
    /// @param value the value to associate with each key. Currently, we have
    /// asserts for result to be equal to the key. Best to set value equal to key!
    int populate(capability* pool, int op_count, K key_lb, K key_ub, std::function<K(uint64_t)> value) {
        // Populate only works when we have numerical keys
        K key_range = key_ub - key_lb;
        // Create a random operation generator that is
        // - evenly distributed among the key range
        int success_count = 0;
        std::uniform_real_distribution<double> dist = std::uniform_real_distribution<double>(0.0, 1.0);
        std::default_random_engine gen(std::chrono::system_clock::now().time_since_epoch().count() * self_.id);
        while (success_count != op_count) {
            int k = (dist(gen) * key_range) + key_lb;
            if (insert(pool, k, value(k)) == std::nullopt) success_count++;
            // Wait some time before doing next insert...
            std::this_thread::sleep_for(std::chrono::nanoseconds(10));
        }
        return success_count;
    }

    /// Single threaded, local print
    void debug(){
        for(int height = MAX_HEIGHT - 1; height != 0; height--){
            int counter = 0;
            Node curr = *root;
            std::cout << height << " SENT -> ";
            while(sans(curr.next[height]) != nullptr){
                std::string marked_next = "";
                if (is_marked(curr.next[height])) marked_next = "!";
                curr = *sans(curr.next[height]);
                std::cout << curr.key << marked_next << " -> ";
                counter++;
            }
            std::cout << "END{" << counter << "}" << std::endl;
        }

        int counter = 0;
        Node curr = *root;
        std::cout << 0 << " SENT -> ";
        while(sans(curr.next[0]) != nullptr){
            std::string marked_next = "";
            if (is_marked(curr.next[0])) marked_next = "!";
            curr = *sans(curr.next[0]);
            if (curr.value == DELETE_SENTINEL) std::cout << "DELETED(" << curr.key << marked_next << ") -> ";
            else if (curr.value == UNLINK_SENTINEL) std::cout << "UNLINKED(" << curr.key << marked_next << ") -> ";
            else std::cout << curr.key << marked_next << " -> ";
            counter++;
        }
        std::cout << "END{" << counter << "}" << std::endl;
    }

    /// No concurrent or thread safe (if everyone is readonly, then its fine). Counts the number of elements in the IHT
    int count(capability* pool){
        // Get leftmost leaf by wrapping SENTINEL
        int count = 0;
        int counter[MAX_HEIGHT];
        int total_counter[MAX_HEIGHT];
        memset(counter, 0, sizeof(int) * MAX_HEIGHT);
        memset(total_counter, 0, sizeof(int) * MAX_HEIGHT);
        Node curr = *cache->template Read<Node>(unmark_ptr(root), prealloc_count_node, -1);
        int cached_nodes = 0;
        while(curr.next[0] != nullptr){
            if (is_marked(curr.next[0])){
                cached_nodes++;
            }
            curr = *cache->template Read<Node>(unmark_ptr(curr.next[0]), prealloc_count_node, MAX_HEIGHT - curr.height);
            counter[curr.height - 1]++;
            for(int i = 0; i <= curr.height - 1; i++){
                total_counter[i]++;
            }
            if (curr.value != DELETE_SENTINEL && curr.value != UNLINK_SENTINEL)
                count++;
        }
        for(int i = 0; i < MAX_HEIGHT; i++){
            REMUS_INFO("nodes with height {} = {}, cumulative={}", i, counter[i], total_counter[i]);
        }
        REMUS_INFO("{} nodes are marked", cached_nodes);
        return count;
    }
};
