#pragma once

#include <barrier>
#include <chrono>
#include <optional>
#include <random>
#include <utility>

#include <remus/workload/workload_driver.h>
#include <remus/logging/logging.h>
#include <remus/util/tcp/tcp.h>
#include <remus/rdma/rdma.h>

#include "common.h"
#include "experiment.h"
#include "tcp_barrier.h"
#include "common.h"
#include "zipfian_int_distribution.h"
// #include "structures/hashtable.h"
// #include "structures/test_map.h"

using remus::WorkloadDriver;
using namespace remus::rdma;
using namespace remus::util;
using namespace std;

// Function to run a test case (will return a success code)
inline bool test_output(bool show_passing, optional<int> actual, optional<int> expected, string message) {
  if (actual.has_value() != expected.has_value() && actual.value_or(0) != expected.value_or(0)) {
    REMUS_INFO("[-] {} func():(Has Value {}=>{}) != expected:(Has Value {}=>{})", message, actual.has_value(), actual.value_or(0),
              expected.has_value(), expected.value_or(0));
    return false;
  } else if (show_passing) {
    REMUS_INFO("[+] Test Case {} Passed!", message);
  }
  return true;
}

enum MapCodes {
  Insert, Get, Remove, Prepare
};

/// Capture the API for a map
/// This is to standardize the map api to allow for different maps to be passed
class MapAPI {
  public:
    function<optional<int>(MapCodes, int, int, int)> conditions;

    /// First function is insert(key, value)
    /// Second function is get(key)
    /// Third function is remove(key)
    /// Fourth function is prepare(op_count, key_lb, key_ub), which is used to register the thread and populate the map
    MapAPI(function<optional<int>(MapCodes, int, int, int)> conditions) : conditions(std::move(conditions)) {}

    optional<int> get(int key){
      return conditions(Get, key, 0, 0);
    }

    optional<int> remove(int key){
      return conditions(Remove, key, 0, 0);
    }

    void prepare(int op_count, int key_lb, int key_ub){
      conditions(Prepare, op_count, key_lb, key_ub);
    }

    optional<int> insert(int key, int value){
      return conditions(Insert, key, value, 0);
    }
};

/// N.B. I can't change the template of the Client without breaking in the WorkloadDriver
/// So I pass in a capture object "MapAPI" to get around this limitation.
template <class Operation> class Client {
  // static_assert(remus::IsClientAdapter<Client, Operation>);

public:
  // [mfs]  Here and in Server, I don't understand the factory pattern.  It's
  //        not really adding any value.
  // [esl]  I think Jacob was trying to force the users of Client to make a unique_ptr? 
  //        It was the pattern I observed, so it felt safer to just follow it haha
  /// @brief Force the creation of a unique ptr to a client instance
  /// @param server the "server"-peer that is responsible for coordination among clients
  /// @param ep a EndpointManager instance that can be owned by the client.
  /// @param params the experiment parameters
  /// @param barr a barrier to synchonize local clients
  /// @param map a map interface
  /// @return a unique ptr
  static unique_ptr<Client>
  Create(const Peer &server, tcp::EndpointManager* ep, BenchmarkParams& params, barrier<> *barr, MapAPI* map, function<void()> do_stop) {
    return unique_ptr<Client>(new Client(server, ep, params, barr, map, do_stop));
  }

  /// @brief Run the client
  /// @param client the client instance to run with
  /// @param thread_id a thread index to use for seeding the random number generation
  /// @param frac if 0, won't populate. Otherwise, will do this fraction of the
  /// population
  /// @return the resultproto
  static StatusVal<WorkloadDriverResult> Run(unique_ptr<Client> client, int thread_id, double frac) {
    // [mfs]  I was hopeful that this code was going to actually populate the
    //        data structure from *multiple nodes* simultaneously.  It should,
    //        or else all of the initial elists and plists are going to be on
    //        the same machine, which probably means all of the elists and
    //        plists will always be on the same machine.
    // [esl]  A remote barrier is defintely needed to make sure this all happens at the same time...
    int key_lb = client->params_.key_lb, key_ub = client->params_.key_ub;
    int op_count = (key_ub - key_lb) * frac;
    REMUS_INFO("CLIENT :: ({}%) to populate (or {} items to insert)", frac * 100, op_count);
    // arrive at the barrier so we are populating in sync with local clients -- TODO: replace for remote barrier
    client->barrier_->arrive_and_wait();
    client->map_->prepare(op_count, key_lb, key_ub);
    REMUS_INFO("CLIENT :: Done with populate!");
    // TODO: Sleeping for 1 second to account for difference between remote
    // client start times. Must fix this in the future to a better solution
    // The idea is even though remote nodes won't be starting a workload at the same
    // time, at least the data structure is roughly guaranteed to be populated
    //
    // [mfs] Indeed, this indicates the need for a distributed barrier
    // [esl] I'm not sure what the design for a distributed barrier over RDMA would look like
    //       But I would be interested in creating one so everyone can use it.
    this_thread::sleep_for(chrono::seconds(1));

    // Ensuring each node has a different seed value
    default_random_engine gen(client->params_.node_id * client->params_.thread_count + thread_id);

    // Create a random operation generator that is
    // - follows the provided distribution (e.g. evenly distributed among the key range)
    // - within the specified ratios for operations
    uniform_int_distribution<int> op_dist = uniform_int_distribution<int>(1, 100);
    int contains = client->params_.contains;
    int insert = client->params_.insert;
    function<Operation(void)> generator;
    uniform_int_distribution<int> k_dist_uni = uniform_int_distribution<int>(key_lb, key_ub);
    zipfian_int_distribution<int> k_dist_90 = zipfian_int_distribution<int>(key_lb, key_ub, 0.90);
    zipfian_int_distribution<int> k_dist_95 = zipfian_int_distribution<int>(key_lb, key_ub, 0.95);
    zipfian_int_distribution<int> k_dist_99 = zipfian_int_distribution<int>(key_lb, key_ub, 0.99);
    
    if (client->params_.distribution == "uniform"){
      generator = [&]() {
        int rng = op_dist(gen);
        int k = k_dist_uni(gen);
        if (rng <= contains) {
          // between 0 and CONTAINS
          return Operation(CONTAINS, k, 0);
        } else if (rng <= contains + insert) { 
          // between CONTAINS and CONTAINS + INSERT
          return Operation(INSERT, k, k);
        } else {
          return Operation(REMOVE, k, 0);
        }
      };
    } else if (client->params_.distribution == "skew90"){
      generator = [&]() {
        int rng = op_dist(gen);
        int k = k_dist_90(gen);
        if (rng <= contains) return Operation(CONTAINS, k, 0);
        else if (rng <= contains + insert) return Operation(INSERT, k, k);
        else return Operation(REMOVE, k, 0);
      };
    } else if (client->params_.distribution == "skew95"){
      generator = [&]() {
        int rng = op_dist(gen);
        int k = k_dist_95(gen);
        if (rng <= contains) return Operation(CONTAINS, k, 0);
        else if (rng <= contains + insert) return Operation(INSERT, k, k);
        else return Operation(REMOVE, k, 0);
      };
    } else if (client->params_.distribution == "skew99"){
      generator = [&]() {
        int rng = op_dist(gen);
        int k = k_dist_99(gen);
        if (rng <= contains) return Operation(CONTAINS, k, 0);
        else if (rng <= contains + insert) return Operation(INSERT, k, k);
        else return Operation(REMOVE, k, 0);
      };
    } else {
      REMUS_FATAL("Cannot find distribution");
    }

    // Generate two streams based on what the user wants (operation count or
    // timed stream)
    unique_ptr<remus::Stream<Operation>> workload_stream;
    if (client->params_.unlimited_stream) {
      std::vector<Operation> stream_content;
      for(int i = 0; i < client->params_.runtime * 1000000; i++){
        stream_content.push_back(generator());
      }
      // Deliver a workload
      workload_stream = make_unique<remus::PrefilledStream<Operation>>(stream_content, stream_content.size());
    } else {
      std::vector<Operation> stream_content;
      for(int i = 0; i < client->params_.op_count; i++){
        stream_content.push_back(generator());
      }
      // Deliver a workload
      workload_stream = make_unique<remus::PrefilledStream<Operation>>(stream_content, stream_content.size());
    }

    // Create and start the workload driver (also starts client and lets it
    // run).
    int32_t runtime = client->params_.runtime;
    barrier<> *barr = client->barrier_;
    bool unlimited_stream = client->params_.unlimited_stream;

    WorkloadDriver driver = WorkloadDriver<Client, Operation>(std::move(client), std::move(workload_stream), chrono::milliseconds(10));
    driver.Run();
   
    REMUS_DEBUG("Done here, stop sequence");
    // Wait for all the clients to stop. Then set the done to true to release
    // the server
    if (barr != nullptr) {
      barr->arrive_and_wait();
    }
    REMUS_DEBUG("CLIENT :: Driver generated {}", driver.ToString());
    // [mfs]  It seems like these protos aren't being sent across machines.  Are
    //        they really needed?
    // [esl]  TODO: They are used by the workload driver. It was easier to live with
    //        then to spend the time to refactor, which is why they haven't been changed yet. 
    //        There probably needs to be a class for storing the result of an experiment.
    return {Status::Ok(), driver.ToMetrics()};
  }

  // Start the client
  Status Start() {
    REMUS_INFO("CLIENT :: Starting client...");
    // pool_->RegisterThread(); // TODO? REMOVE? PUT BACK?
    // [mfs]  The entire barrier infrastructure is odd.  Nobody is using it to
    //        know when to get time, and it's completely per-node.
    // [esl]  I think the Workload driver gets time, which is why I think its a good idea to synchronize the threads
    //        You make a good point, synchronizing among nodes would be good 
    if (barrier_ != nullptr)
      barrier_->arrive_and_wait();
    return Status::Ok();
  }

  // Runs the next operation
  Status Apply(const Operation &op) {
    count++;
    optional<int> res;
    switch (op.op_type) {
    case (CONTAINS):
      if (count % progression == 0) {
        REMUS_DEBUG("Running Operation {}: contains({})", count, op.key);
      }
      res = map_->get(op.key);
      if (res.has_value()) {
        REMUS_ASSERT(res.value() == op.key, "Invalid result of contains operation {}!={}", res.value(), op.key);
      }
      break;
    case (INSERT):
      if (count % progression == 0){
        REMUS_DEBUG("Running Operation {}: insert({}, {})", count, op.key, op.value);
      }
      res = map_->insert(op.key, op.value);
      if (res.has_value()){
        REMUS_ASSERT(res.value() == op.key, "Invalid result of insert operation {}!={}", res.value(), op.key);
      }
      break;
    case (REMOVE):
      if (count % progression == 0){
        REMUS_DEBUG("Running Operation {}: remove({})", count, op.key);
      }
      res = map_->remove(op.key);
      if (res.has_value()){
        REMUS_ASSERT(res.value() == op.key, "Invalid result of remove operation {}!={}", res.value(), op.key);
      }
      break;
    default:
      // if we get something other than a contains, insert, or remove, the program probably should die
      REMUS_FATAL("Expected CONTAINS, INSERT, or REMOVE operation.");
      break;
    }
    return Status::Ok();
  }

  // A function for communicating with the server that we are done. Will wait
  // until server says it is ok to shut down
  //
  // [mfs]  This is really just trying to create a Barrier over RPC.  There's
  //        nothing wrong with that, in principle, but if all we really need is
  //        a barrier, then why not just make a barrier?
  remus::util::Status Stop() {
    REMUS_DEBUG("CLIENT :: Stopping client...");
    do_stop_();
    ExperimentManager::ClientArriveBarrier(endpoint_);
    return Status::Ok();
  }

private:
  /// @brief Private constructor of client
  /// @param server the "server"-peer that is responsible for coordination among clients
  /// @param endpoint a EndpointManager instance that can be owned by the client.
  /// @param params the experiment parameters
  /// @param barrier a barrier to synchonize local clients
  /// @return a unique ptr
  Client(const Peer &host, tcp::EndpointManager* ep, BenchmarkParams &params, barrier<> *barr, MapAPI* map, function<void()> do_stop)
    : host_(host), endpoint_(ep), params_(params), barrier_(barr), map_(map), do_stop_(do_stop){
      if (params.unlimited_stream) progression = 100000;
      else progression = max(20.0, params_.op_count * params_.thread_count * 0.01);
    }

  int count = 0;

  /// @brief Represents the host peer
  const Peer host_;
  /// @brief Represents an endpoint to be used for communication with the host peer
  tcp::EndpointManager* endpoint_;
  /// @brief Experimental parameters
  const BenchmarkParams params_;
  /// @brief a barrier for syncing amount clients locally
  barrier<> *barrier_;
  /// @brief a map instance to use
  MapAPI* map_;
  /// @brief a function to run before ending
  function<void()> do_stop_;

  /// @brief The number of operations to do before debug-printing the number of completed operations
  /// This is useful in debugging since I can see around how many operations have been done (if at all) before crashing
  int progression;
};