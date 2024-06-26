#include <google/protobuf/text_format.h>
#include <protos/experiment.pb.h>
#include <protos/workloaddriver.pb.h>
#include <vector>

#include <remus/logging/logging.h>
#include <remus/util/cli.h>
#include <remus/rdma/rdma.h>

#include "common.h"
#include "role_client.h"
#include "iht_ds.h"

using namespace remus::util;
using namespace remus::rdma;

auto ARGS = {
    BOOL_ARG_OPT("--send_bulk", "If to run test operations multithreaded"),
    BOOL_ARG_OPT("--send_test", "If to test the functionality of the methods"),
    BOOL_ARG_OPT("-v", "If to be verbose in testing output"),
};

#define PATH_MAX 4096
#define PORT_NUM 18000

// The optimial number of memory pools is mp=min(t, MAX_QP/n) where n is the number of nodes and t is the number of threads
// To distribute mp (memory pools) across t threads, it is best for t/mp to be a whole number

typedef RdmaIHT<int, int, CNF_ELIST_SIZE, CNF_PLIST_SIZE> IHT;

int main(int argc, char** argv){
    REMUS_INIT_LOG();

    ArgMap args;
    // import_args will validate that the newly added args don't conflict with
    // those already added.
    auto res = args.import_args(ARGS);
    if (res) {
      REMUS_FATAL(res.value());
    }
    // NB: Only call parse_args once.  If it fails, a mandatory arg was skipped
    res = args.parse_args(argc, argv);
    if (res) {
      args.usage();
      REMUS_FATAL(res.value());
    }

    // Extract the args to variables
    bool bulk_operations = args.bget("--send_bulk");
    bool test_operations = args.bget("--send_test");
    bool verbose = args.bget("-v");
    REMUS_ASSERT((bulk_operations ^ test_operations) == 1, "Assert one flag (bulk or test) is used"); // assert one or the other

    // Create a single peer
    volatile bool done = false; // (Should be atomic?)
    Peer host = Peer(0, "node0", PORT_NUM + 1);
    std::vector<Peer> peer_list = std::vector<Peer>(0);
    peer_list.push_back(host);
    // Initialize a memory pool
    std::vector<std::thread> mempool_threads;
    std::shared_ptr<rdma_capability> pool = std::make_shared<rdma_capability>(host);
    uint32_t block_size = 1 << 24;
    pool->init_pool(block_size, peer_list);

    // Create an iht
    std::unique_ptr<IHT> iht_ = std::make_unique<IHT>(host, CacheDepth::None, pool);
    iht_->InitAsFirst(pool);
    if (test_operations){
      pool->RegisterThread();
      REMUS_INFO("Starting basic test cases.");
      test_output(true, iht_->contains(pool, 5), std::nullopt, "Contains 5");
      test_output(true, iht_->contains(pool, 4), std::nullopt, "Contains 4");
      test_output(true, iht_->insert(pool, 5, 10), std::nullopt, "Insert 5");
      test_output(true, iht_->insert(pool, 5, 11),  std::make_optional(10), "Insert 5 again should fail");
      test_output(true, iht_->contains(pool, 5),  std::make_optional(10), "Contains 5");
      test_output(true, iht_->contains(pool, 4), std::nullopt, "Contains 4");
      test_output(true, iht_->remove(pool, 5),  std::make_optional(10), "Remove 5");
      test_output(true, iht_->remove(pool, 4), std::nullopt, "Remove 4");
      test_output(true, iht_->contains(pool, 5), std::nullopt, "Contains 5");
      test_output(true, iht_->contains(pool, 4), std::nullopt, "Contains 4");
      int scale_size = (CNF_PLIST_SIZE * CNF_ELIST_SIZE) * 4;
      REMUS_INFO("All basic test cases finished, starting bulk tests. Scale is {}", scale_size);
      for(int i = 0; i < scale_size; i++){
        test_output(verbose, iht_->contains(pool, i), std::nullopt, std::string("Contains ") + std::to_string(i) + std::string(" false"));
        test_output(verbose, iht_->insert(pool, i, i), std::nullopt, std::string("Insert ") + std::to_string(i));
        test_output(verbose, iht_->contains(pool, i), std::make_optional(i), std::string("Contains ") + std::to_string(i) + std::string(" true"));
      }
      REMUS_INFO(" = 25% Finished = ");
      for(int i = 0; i < scale_size; i++){
        test_output(verbose, iht_->contains(pool, i), std::make_optional(i), std::string("Contains ") + std::to_string(i) + std::string(" maintains true"));
      }
      REMUS_INFO(" = 50% Finished = ");
      for(int i = 0; i < scale_size; i++){
        test_output(verbose, iht_->remove(pool, i), std::make_optional(i), std::string("Removes ") + std::to_string(i));
        test_output(verbose, iht_->contains(pool, i), std::nullopt, std::string("Contains ") + std::to_string(i) + std::string(" false"));
      }
      REMUS_INFO(" = 75% Finished = ");
      for(int i = 0; i < scale_size; i++){
        test_output(verbose, iht_->contains(pool, i), std::nullopt, std::string("Contains ") + std::to_string(i) + std::string(" maintains false"));
      }
      REMUS_INFO("All test cases finished");
    } else if (bulk_operations) {
      int THREAD_COUNT = 10;
      std::vector<std::thread> threads(0);
      std::barrier<> barr(THREAD_COUNT);
      for (int t = 0; t < THREAD_COUNT; t++){
          threads.push_back(std::thread([&](int id){
              pool->RegisterThread();
              barr.arrive_and_wait();
              auto start = chrono::high_resolution_clock::now();
              if (id == 0) REMUS_INFO("Starting populating");
              for (int ops = 0; ops < 50000; ops++){
                  if (verbose && ops % 5000 == 0){
                    REMUS_INFO("Progress Update: (Thread {}) {} ops", id, ops);
                  }
                  // Everybody is trying to insert the same data.
                  iht_->insert(pool, ops, ops * 2);
              }
              barr.arrive_and_wait();
              if (id == 0) REMUS_INFO("Done populating, start workload");
              auto populate_checkpoint = chrono::high_resolution_clock::now();
              for (int ops = 0; ops < 100000; ops++){
                  auto res = iht_->contains(pool, ops);
                  if (verbose && ops % 5000 == 0){
                    REMUS_INFO("Progress Update: (Thread {}) {} ops", id, ops);
                  }
                  // assert bottom half is present and top half is absent
                  if (ops < 50000){
                      assert(res.has_value() && res.value() == ops * 2);
                  } else {
                      assert(!res.has_value());
                  }
              }
              barr.arrive_and_wait();
              if (id == 0){
                auto end = chrono::high_resolution_clock::now();
                auto start_to_checkpoint = chrono::duration_cast<chrono::milliseconds>(populate_checkpoint - start);
                auto checkpoint_to_end = chrono::duration_cast<chrono::milliseconds>(end - populate_checkpoint);
                auto total_dur = chrono::duration_cast<chrono::milliseconds>(end - start);
                REMUS_WARN("This test used for correctness, not to be used for benchmarking, use --send_exp");
                REMUS_INFO("Inserts:{}ms Contains:{}ms Total:{}ms", start_to_checkpoint.count(), checkpoint_to_end.count(), total_dur.count());
              }
          }, t));
      }
      // Join threads
      for(int t = 0; t < THREAD_COUNT; t++){
          threads.at(t).join();
      }
    } else {
      REMUS_INFO("Use main executable not test");
    }

    REMUS_INFO("[EXPERIMENT] -- End of execution; -- ");
    return 0;
}
