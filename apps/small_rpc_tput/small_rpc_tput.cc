#include <arpa/inet.h>
#include <gflags/gflags.h>
#include <inttypes.h>
#include <linux/types.h>
#include <malloc.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cstring>
#include <string>

#include "../apps_common.h"
#include "rpc.h"
#include "util/autorun_helpers.h"
#include "util/latency.h"
#include "util/numautils.h"

#define STORE 0
#define LOCK_FASST 1
#define APP LOCK_FASST

static constexpr size_t kAppEvLoopMs = 1000;  // Duration of event loop
static constexpr size_t kAppReqType = 1;      // eRPC request type
static constexpr size_t kMaxClientsPerTh = 128;
#if APP == LOCK_FASST
// maximum number of locks
constexpr uint32_t kLockHashSize = 36000000;

// maximum number of locks clients will query
constexpr uint32_t kQueryedLockSize = 24000000;
#endif

DEFINE_uint64(num_threads, 0, "Number of foreground threads per machine");
DEFINE_uint64(num_clients, 0, "Number of simulated clients in total");
DEFINE_uint64(is_client, 0, "Whether this is the load generator");
DEFINE_uint64(num_dst_threads, 0,
              "Number of foreground threads for the destination");

thread_local size_t num_clients_per_th = 0;
thread_local size_t tg_seed = 0;

#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

#define panic(fmt, ...)                  \
  do {                                   \
    fprintf(stderr, fmt, ##__VA_ARGS__); \
    exit(EXIT_FAILURE);                  \
  } while (0)

// Compression function for Merkle-Damgard construction.
// This function is generated using the framework provided.
static inline uint64_t fasthash_mix(uint64_t h) {
  h ^= h >> 23;
  h *= 0x2127599bf4325c37ULL;
  h ^= h >> 47;
  return h;
}

static inline uint64_t fasthash64(const void *buf, uint64_t len,
                                  uint64_t seed) {
  const uint64_t m = 0x880355f21e6d1965ULL;
  const uint64_t *pos = (const uint64_t *)buf;
  const uint64_t *end = pos + (len / 8);
  const unsigned char *pos2;
  uint64_t h = seed ^ (len * m);
  uint64_t v;

  while (pos != end) {
    v = *pos++;
    h ^= fasthash_mix(v);
    h *= m;
  }

  pos2 = (const unsigned char *)pos;
  v = 0;

  switch (len & 7) {
    case 7: v ^= (uint64_t)pos2[6] << 48;
    case 6: v ^= (uint64_t)pos2[5] << 40;
    case 5: v ^= (uint64_t)pos2[4] << 32;
    case 4: v ^= (uint64_t)pos2[3] << 24;
    case 3: v ^= (uint64_t)pos2[2] << 16;
    case 2: v ^= (uint64_t)pos2[1] << 8;
    case 1:
      v ^= (uint64_t)pos2[0];
      h ^= fasthash_mix(v);
      h *= m;
  }

  return fasthash_mix(h);
}

static inline uint32_t fasthash32(const void *buf, uint64_t len,
                                  uint32_t seed) {
  // the following trick converts the 64-bit hashcode to Fermat
  // residue, which shall retain information from both the higher
  // and lower parts of hashcode.
  uint64_t h = fasthash64(buf, len, seed);
  return h - (h >> 32);
}

constexpr int kValSize = 40;
constexpr int kKeysPerEntry = 4;

struct kvs_entry {
  uint64_t key[kKeysPerEntry];
  uint8_t val[kKeysPerEntry][kValSize];
  uint32_t ver[kKeysPerEntry];
  uint8_t valid[kKeysPerEntry];
  kvs_entry *next;
};

struct kvs {
  int hash_size;
  kvs_entry **bucket_heads;
  int *locks;
};

static inline void kvs_init(kvs *kvs, int hash_size) {
  kvs->hash_size = hash_size;
  kvs->bucket_heads = (kvs_entry **)calloc(hash_size, sizeof(kvs_entry *));
  kvs->locks = (int *)calloc(hash_size, sizeof(int));
}

static inline int kvs_hash(kvs *kvs, uint64_t key) {
  return (int)(fasthash64(&key, sizeof(key), 0xdeadbeef) %
               (uint64_t)kvs->hash_size);
}

static inline int kvs_get(kvs *kvs, uint64_t key, uint8_t *val, uint32_t *ver) {
  uint32_t hash = kvs_hash(kvs, key);
  while (__sync_lock_test_and_set(&kvs->locks[hash], 1))
    ;

  kvs_entry *head = kvs->bucket_heads[hash];
  while (head) {
    for (int i = 0; i < kKeysPerEntry; i++) {
      if (head->key[i] == key && head->valid[i]) {
        memcpy(val, head->val[i], kValSize);
        *ver = head->ver[i];
        __sync_val_compare_and_swap(&kvs->locks[hash], 1, 0);
        return 0;
      }
    }
    head = head->next;
  }
  __sync_val_compare_and_swap(&kvs->locks[hash], 1, 0);
  return 1;
}

static inline int kvs_set(kvs *kvs, uint64_t key, uint8_t *val) {
  uint32_t hash = kvs_hash(kvs, key);
  while (__sync_lock_test_and_set(&kvs->locks[hash], 1))
    ;

  kvs_entry *head = kvs->bucket_heads[hash];
  while (head) {
    for (int i = 0; i < kKeysPerEntry; i++) {
      if (head->key[i] == key && head->valid[i]) {
        memcpy(head->val[i], val, kValSize);
        head->ver[i]++;
        __sync_val_compare_and_swap(&kvs->locks[hash], 1, 0);
        return 0;
      }
    }
    head = head->next;
  }
  __sync_val_compare_and_swap(&kvs->locks[hash], 1, 0);
  return 1;
}

static inline void kvs_insert(kvs *kvs, uint64_t key, uint8_t *val) {
  uint32_t hash = kvs_hash(kvs, key);
  while (__sync_lock_test_and_set(&kvs->locks[hash], 1))
    ;

  kvs_entry *head = kvs->bucket_heads[hash];
  while (head) {
    for (int i = 0; i < kKeysPerEntry; i++) {
      if (!head->valid[i]) {
        head->key[i] = key;
        memcpy(head->val[i], val, kValSize);
        head->ver[i] = 0;
        head->valid[i] = 1;
        __sync_val_compare_and_swap(&kvs->locks[hash], 1, 0);
        return;
      }
    }
    head = head->next;
  }
  kvs_entry *e = new kvs_entry;
  e->key[0] = key;
  memcpy(e->val[0], val, kValSize);
  e->ver[0] = 0;
  memset(e->valid, 0, sizeof(e->valid));
  e->valid[0] = 1;
  e->next = kvs->bucket_heads[hash];
  kvs->bucket_heads[hash] = e;
  __sync_val_compare_and_swap(&kvs->locks[hash], 1, 0);
}

static inline void kvs_delete(kvs *kvs, uint64_t key) {
  uint32_t hash = kvs_hash(kvs, key);
  while (__sync_lock_test_and_set(&kvs->locks[hash], 1))
    ;

  kvs_entry *head = kvs->bucket_heads[hash], *prev = nullptr;
  while (head) {
    for (int i = 0; i < kKeysPerEntry; i++) {
      if (head->key[i] == key && head->valid[i]) {
        head->valid[i] = 0;
        // if all keys in this entry are invalid, delete the entry
        bool all_invalid = true;
        for (int j = 0; j < kKeysPerEntry; j++) {
          if (head->valid[j]) {
            all_invalid = false;
            break;
          }
        }
        if (all_invalid) {
          if (prev)
            prev->next = head->next;
          else
            kvs->bucket_heads[hash] = head->next;
          delete head;
        }
        __sync_val_compare_and_swap(&kvs->locks[hash], 1, 0);
        return;
      }
    }
    prev = head;
    head = head->next;
  }
  panic("kvs_delete: key not found");
}

constexpr int kSubscriberNum = 8000000;
constexpr int A = 1048575;
constexpr uint8_t kValMagic = 0x5a;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
union store_key_t {
  struct {
    uint32_t s_id;
    uint8_t sf_type;
    uint8_t start_time;
    uint8_t unused[2];
  };
  uint64_t key;
  store_key_t() : key(0) {}
};
#pragma GCC diagnostic pop

struct store_val_t {
  uint8_t end_time;
  char numberx[39];
};

// random number generator
static inline uint32_t fastrand(uint64_t *seed) {
  *seed = *seed * 1103515245 + 12345;
  return (uint32_t)(*seed >> 32);
}

// get a non-uniform-random distributed subscriber ID according to spec
// To get a non-uniformly random number between 0 and y:
// NURand(A, 0, y) = (get_random(0, A) | get_random(0, y)) % (y + 1)
static inline uint32_t tatp_nurand(uint64_t *tg_seed) {
  return ((fastrand(tg_seed) % kSubscriberNum) | (fastrand(tg_seed) & A)) %
         kSubscriberNum;
}

// populate table
static inline void populate_table(kvs *table, std::string populate_mode) {
  std::vector<uint8_t> sf_type_values = {1, 2, 3, 4};

  uint64_t tmp_seed = 0xdeadbeef;

  for (uint32_t s_id = 0; s_id < kSubscriberNum; s_id++) {
    for (uint8_t &sf_type : sf_type_values) {
      for (size_t start_time = 0; start_time <= 16; start_time += 8) {
        if (populate_mode.compare("half") == 0) {
          if (fastrand(&tmp_seed) % 2 == 0) continue;
        }

        store_key_t store_key;
        store_key.s_id = s_id;
        store_key.sf_type = sf_type;
        store_key.start_time = start_time;

        store_val_t val;
        val.end_time = (fastrand(&tmp_seed) % 24) + 1;
        val.numberx[0] = kValMagic;

        kvs_insert(table, store_key.key, (uint8_t *)&val);
      }  // loop start_time
    }    // loop sf_type
  }      // loop s_id
}

#pragma pack(push, 1)
#if APP == STORE

// packet types
enum PktType {
  // client
  kRead = 0,
  kSet = 1,
  kInsert = 2,

  // server
  kGrantRead = 3,
  kRejectRead = 4,
  kSetAck = 5,
  kRejectSet = 6,
  kNotExist = 7,
  kInsertAck = 8,
  kRejectInsert = 9,
};

struct message {
  uint8_t type;           // packet type
  uint64_t key;           // key
  uint8_t val[kValSize];  // value
  uint32_t ver;           // version
};

// the main table
kvs *table;

// populate mode
std::string populate_mode = "all";

#elif APP == LOCK_FASST

enum PktType {
  kRead = 0,
  kAcquireLock = 1,
  kAbort = 2,
  kCommit = 3,
  kGrantRead = 4,
  kGrantLock = 5,
  kRejectLock = 6,
  kAbortAck = 7,
  kCommitAck = 8,
};

struct message {
  uint8_t type;
  uint32_t lid;
  uint32_t ver;
};

// transaction locks
volatile int locks[kLockHashSize];

// version table
uint32_t ver_table[kLockHashSize];

#endif
#pragma pack(pop)

constexpr uint32_t kStatsPollIntv = 1;
constexpr int kStatsStartSec = 10;
constexpr int kStatsEndSec = 20;

template <class T>
T Percentile(std::vector<T> &vectorIn, double percent) {
  if (vectorIn.size() == 0) return (T)0;
  auto nth = vectorIn.begin() + (percent * vectorIn.size()) / 100;
  std::nth_element(vectorIn.begin(), nth, vectorIn.end());
  return *nth;
}

// statistics
std::vector<std::vector<uint64_t>> lat_samples;
std::vector<uint64_t> pkt_cnt, suc_pkt_cnt;
bool stat_started = false;

void CollectStat() {
  std::vector<uint64_t> lat_aggr;

  uint64_t total_pkt = std::accumulate(pkt_cnt.begin(), pkt_cnt.end(), 0UL);
  uint64_t total_suc_pkt =
      std::accumulate(suc_pkt_cnt.begin(), suc_pkt_cnt.end(), 0UL);
  uint64_t total_lat = std::accumulate(lat_aggr.begin(), lat_aggr.end(), 0UL);

  for (size_t i = 0; i < FLAGS_num_threads; i++)
    lat_aggr.insert(lat_aggr.end(), lat_samples[i].begin(),
                    lat_samples[i].end());

  printf("throughput: %lu\n", total_pkt / (kStatsEndSec - kStatsStartSec));
  printf("goodput: %lu\n", total_suc_pkt / (kStatsEndSec - kStatsStartSec));
  printf("average latency: %lu\n", total_lat / lat_aggr.size());
  printf("median latency: %lu\n", Percentile(lat_aggr, 50));
  printf("99th percentile latency: %lu\n", Percentile(lat_aggr, 99));
  printf("99.9th percentile latency: %lu\n", Percentile(lat_aggr, 99.9));
  fflush(stdout);
}

// print throughput
void PrintTput(uint32_t poll_cnt) {
  printf("%u throughput\n", poll_cnt);

  static uint64_t last_pkt = 0, last_suc_pkt = 0;
  uint64_t total_pkt = std::accumulate(pkt_cnt.begin(), pkt_cnt.end(), 0UL);
  uint64_t total_suc_pkt =
      std::accumulate(suc_pkt_cnt.begin(), suc_pkt_cnt.end(), 0UL);
  printf("pkt: %lu\n", total_pkt - last_pkt);
  printf("suc_pkt: %lu\n", total_suc_pkt - last_suc_pkt);
  last_pkt = total_pkt;
  last_suc_pkt = total_suc_pkt;
}

volatile sig_atomic_t ctrl_c_pressed = 0;
void ctrl_c_handler(int) { ctrl_c_pressed = 1; }

union tag_t {
  struct {
    uint64_t client_i : 32;
    uint64_t padding : 32;
  } s;

  void *_tag;

  tag_t(uint64_t client_i, uint64_t padding) {
    s.client_i = client_i;
    s.padding = padding;
  }
  tag_t(void *_tag) : _tag(_tag) {}
};

static_assert(sizeof(tag_t) == sizeof(void *), "");

// Per-client context
class ClientContext {
 public:
  size_t req_tsc;  // Timestamp when request was issued
  erpc::MsgBuffer req_msgbuf;
  erpc::MsgBuffer resp_msgbuf;
};

// Per-thread application context
class PerThContext : public BasicAppContext {
 public:
  std::array<ClientContext, kMaxClientsPerTh> client_arr;  // Per-client context
  ~PerThContext() {}
};

void app_cont_func(void *, void *);  // Forward declaration

// Send all requests for a client
void send_reqs(PerThContext *c, size_t client_i) {
  assert(client_i < num_clients_per_th);
  ClientContext &bc = c->client_arr[client_i];

  message *msg = (message *)bc.req_msgbuf.buf_;

#if APP == STORE
  // transaction parameters
  uint32_t s_id = tatp_nurand(&tg_seed);
  uint8_t sf_type = (fastrand(&tg_seed) % 4) + 1;
  uint8_t start_time = (fastrand(&tg_seed) % 3) * 8;

  // read the call forwarding record
  store_key_t store_key;
  store_key.s_id = s_id;
  store_key.sf_type = sf_type;
  store_key.start_time = start_time;

  msg->type = PktType::kRead;
  msg->key = store_key.key;
#elif APP == LOCK_FASST
  // A simplified version of ./trace_init.sh 24000000 0.8 2000
  auto type_rnd = fastrand(&tg_seed) % 100;
  auto lid = fastrand(&tg_seed) % kQueryedLockSize;

  if (type_rnd < 80) {
    // Read lock;
    *msg = {PktType::kRead, lid, 0};
  } else {
    // Write lock;
    if (fastrand(&tg_seed) % 2) {
      *msg = {PktType::kAcquireLock, lid, 0};
    } else {
      *msg = {PktType::kCommit, lid, 0};
    }
  }
#endif

  bc.req_tsc = erpc::rdtsc();

  tag_t tag(client_i, 0);
  c->rpc_->enqueue_request(c->session_num_vec_[0], kAppReqType, &bc.req_msgbuf,
                           &bc.resp_msgbuf, app_cont_func,
                           reinterpret_cast<void *>(tag._tag));
}

void req_handler(erpc::ReqHandle *req_handle, void *_context) {
  auto *c = static_cast<PerThContext *>(_context);

  const erpc::MsgBuffer *req_msgbuf = req_handle->get_req_msgbuf();
  assert(req_msgbuf->get_data_size() == sizeof(message));

  // Preallocated response optimization knob
  erpc::Rpc<erpc::CTransport>::resize_msg_buffer(&req_handle->pre_resp_msgbuf_,
                                                 sizeof(message));

  message *msg = (message *)req_msgbuf->buf_;
  message *resp_msg = (message *)req_handle->pre_resp_msgbuf_.buf_;

  int ret;

#if APP == STORE
  switch (msg->type) {
    case PktType::kRead:
      resp_msg->key = msg->key;
      ret = kvs_get(table, msg->key, resp_msg->val, &msg->ver);
      if (ret == 0)
        resp_msg->type = PktType::kGrantRead;
      else
        resp_msg->type = PktType::kNotExist;
      break;

    case PktType::kSet:
      resp_msg->key = msg->key;
      ret = kvs_set(table, msg->key, msg->val);
      if (ret == 0)
        resp_msg->type = PktType::kSetAck;
      else
        resp_msg->type = PktType::kNotExist;
      break;

    default: panic("unknown operation %d", msg->type);
  }
#elif APP == LOCK_FASST
  uint64_t hash = fasthash64(&msg->lid, sizeof(msg->lid), 0xdeadbeef);
  uint32_t lock_hash = (uint32_t)(hash % (uint64_t)kLockHashSize);

  switch (msg->type) {
    case PktType::kRead:
      resp_msg->type = PktType::kGrantRead;
      resp_msg->ver = ver_table[lock_hash];
      break;

    case PktType::kAcquireLock:
      ret = __sync_val_compare_and_swap(&locks[lock_hash], 0, 1);
      if (ret == 0) {
        resp_msg->type = PktType::kGrantLock;
      } else if (ret == 1) {
        resp_msg->type = PktType::kRejectLock;
      } else
        panic("unknown lock state");
      break;

    case PktType::kAbort:
      __sync_val_compare_and_swap(&locks[lock_hash], 1, 0);
      resp_msg->type = PktType::kAbortAck;
      break;

    case PktType::kCommit:
      ver_table[lock_hash]++;
      __sync_val_compare_and_swap(&locks[lock_hash], 1, 0);
      resp_msg->type = PktType::kCommitAck;
      break;

    default: panic("unknown packet type %d", msg->type);
  }
#endif

  c->rpc_->enqueue_response(req_handle, &req_handle->pre_resp_msgbuf_);
}

void app_cont_func(void *_context, void *_tag) {
  auto *c = static_cast<PerThContext *>(_context);
  auto tag = static_cast<tag_t>(_tag);

  ClientContext &bc = c->client_arr[tag.s.client_i];
  const erpc::MsgBuffer &resp_msgbuf = bc.resp_msgbuf;
  if (resp_msgbuf.get_data_size() != sizeof(message)) {
    printf("resp data_size error\n");
  }

  if (stat_started) {
    size_t req_tsc = bc.req_tsc;
    double req_lat_us =
        erpc::to_usec(erpc::rdtsc() - req_tsc, c->rpc_->get_freq_ghz());

    pkt_cnt[c->thread_id_]++;
    lat_samples[c->thread_id_].push_back(static_cast<size_t>(req_lat_us));
  }

#if APP == STORE
  message *msg = (message *)bc.resp_msgbuf.buf_;
  if (msg->type == PktType::kNotExist) {
  } else {
    assert(msg->type == PktType::kGrantRead);
    if (stat_started) suc_pkt_cnt[c->thread_id_]++;
  }
#elif APP == LOCK_FASST
  // we make simplification that assumes all lock acquire is successfully, based
  // on our benchmarking on caladan.
  if (stat_started) suc_pkt_cnt[c->thread_id_]++;
#endif

  send_reqs(c, tag.s.client_i);
}

void connect_sessions(PerThContext &c) {
  if (!FLAGS_is_client) return;  // server does not initiate sessions to clients

  // Create a session to each thread in the cluster except to:
  // (a) all threads on this machine if DPDK is used (because no loopback), or
  // (b) this thread if a non-DPDK transport is used.
  size_t p_i = 0;
  erpc::rt_assert(p_i != FLAGS_process_id, "connect_sessions error");

  std::string remote_uri = erpc::get_uri_for_process(p_i);

  if (FLAGS_sm_verbose == 1) {
    printf("Process %zu, thread %zu: Creating sessions to %s.\n",
           FLAGS_process_id, c.thread_id_, remote_uri.c_str());
  }

  size_t t_i = c.fastrand_.next_u32() % FLAGS_num_dst_threads;
  int session_num = c.rpc_->create_session(remote_uri, t_i);
  erpc::rt_assert(session_num >= 0, "Failed to create session");
  c.session_num_vec_.push_back(session_num);

  while (c.num_sm_resps_ != c.session_num_vec_.size()) {
    c.rpc_->run_event_loop(kAppEvLoopMs);
    if (unlikely(ctrl_c_pressed == 1)) return;
  }
}

// The function executed by each thread in the cluster
void thread_func(size_t thread_id, erpc::Nexus *nexus) {
  if (FLAGS_is_client) {
    num_clients_per_th = FLAGS_num_clients / FLAGS_num_threads;
    if (thread_id < FLAGS_num_clients % FLAGS_num_threads) num_clients_per_th++;
    erpc::rt_assert(num_clients_per_th <= kMaxClientsPerTh,
                    "Invalid num_clients_per_th.");

    auto wrkr_gid = FLAGS_process_id * FLAGS_num_clients + thread_id;
    int wrkr_lid = wrkr_gid % FLAGS_num_clients;
    tg_seed = 0xdeadbeef + wrkr_gid;
    printf("worker %d started\n", wrkr_lid);
  }

  PerThContext c;
  c.thread_id_ = thread_id;

  std::vector<size_t> port_vec = flags_get_numa_ports(FLAGS_numa_node);
  erpc::rt_assert(port_vec.size() > 0);
  uint8_t phy_port = port_vec.at(thread_id % port_vec.size());

  erpc::Rpc<erpc::CTransport> rpc(nexus, static_cast<void *>(&c),
                                  static_cast<uint8_t>(thread_id),
                                  basic_sm_handler, phy_port);

  rpc.retry_connect_on_invalid_rpc_id_ = true;
  c.rpc_ = &rpc;

  // Pre-allocate request and response MsgBuffers for each client
  for (size_t i = 0; i < num_clients_per_th; i++) {
    ClientContext &bc = c.client_arr[i];
    bc.req_msgbuf = rpc.alloc_msg_buffer_or_die(sizeof(message));
    bc.resp_msgbuf = rpc.alloc_msg_buffer_or_die(sizeof(message));
  }

  connect_sessions(c);

  printf("Process %zu, thread %zu: All sessions connected. Starting work.\n",
         FLAGS_process_id, thread_id);

  // Start work
  if (FLAGS_is_client) {
    for (size_t i = 0; i < num_clients_per_th; i++) send_reqs(&c, i);
  }

  if (FLAGS_is_client) {
    uint32_t poll_cnt = 0;
    while (1) {
      rpc.run_event_loop(kAppEvLoopMs);  // 1 second
      if (ctrl_c_pressed == 1) break;

      if (stat_started && thread_id == 0) PrintTput(poll_cnt);

      poll_cnt++;
      if (unlikely(poll_cnt > kStatsStartSec && !stat_started))
        stat_started = true;

      if (unlikely(poll_cnt > kStatsEndSec && thread_id == 0)) {
        stat_started = false;
        CollectStat();
        exit(0);
      }
    }
  } else {
    for (size_t i = 0;; i += 1000) {
      rpc.run_event_loop(kAppEvLoopMs);  // 1 second
      if (ctrl_c_pressed == 1) break;
    }
  }
}

int main(int argc, char **argv) {
  signal(SIGINT, ctrl_c_handler);
  gflags::ParseCommandLineFlags(&argc, &argv, true);

  erpc::rt_assert(FLAGS_numa_node <= 1, "Invalid NUMA node");

#if APP == STORE
  int hash_size = kSubscriberNum * 18 / kKeysPerEntry;

  if (!FLAGS_is_client) {
    table = new kvs();
    kvs_init(table, hash_size);
    populate_table(table, populate_mode);
  }
#endif

  pkt_cnt.resize(FLAGS_num_threads, 0);
  suc_pkt_cnt.resize(FLAGS_num_threads, 0);
  lat_samples.resize(FLAGS_num_threads);

  printf("finish initialization\n");

  // We create a bit fewer sessions
  const size_t num_sessions = 2 * FLAGS_num_processes * FLAGS_num_threads;
  erpc::rt_assert(num_sessions * erpc::kSessionCredits <=
                      erpc::Transport::kNumRxRingEntries,
                  "Too few ring buffers");

  erpc::Nexus nexus(erpc::get_uri_for_process(FLAGS_process_id),
                    FLAGS_numa_node, 0);
  nexus.register_req_func(kAppReqType, req_handler);
  nexus.register_req_func(kPingReqHandlerType, ping_req_handler);

  std::vector<std::thread> threads(FLAGS_num_threads);

  for (size_t i = 0; i < FLAGS_num_threads; i++) {
    threads[i] = std::thread(thread_func, i, &nexus);
    erpc::bind_to_core(threads[i], FLAGS_numa_node, i);
  }

  for (auto &thread : threads) thread.join();
}
