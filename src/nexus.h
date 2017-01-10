#ifndef ERPC_NEXUS_H
#define ERPC_NEXUS_H

#include <signal.h>
#include <mutex>
#include <queue>
#include <vector>

#include "common.h"
#include "session.h"
using namespace std;

namespace ERpc {

class Nexus {
 public:
  /**
   * @brief Construct the one-per-process Nexus object
   *
   * @param port The UDP port used by all Nexus-es in the cluster to listen
   * for session management packets.
   */
  Nexus(uint16_t global_udp_port);
  ~Nexus();

  void register_hook(SessionMgmtHook *hook);
  void unregister_hook(SessionMgmtHook *hook);

  void install_sigio_handler();
  void session_mgnt_handler();

  inline double get_freq_ghz() { return freq_ghz; }

  char hostname[kMaxHostnameLen]; /* The local host's network hostname */

  /*
   * The Nexus is shared among all Rpc threads. This lock must be held while
   * calling Nexus functions from Rpc threads.
   */
  std::mutex nexus_lock;

  /* Hooks into session management objects registered by RPC objects */
  std::vector<SessionMgmtHook *> reg_hooks;

  /*
   * The UDP port used by all Nexus-es in the cluster to listen on for
   * session management
   */
  const uint16_t global_udp_port;
  int nexus_sock_fd; /* The file descriptor of the UDP socket */

 private:
  /**
   * @brief Compute the frequency of rdtsc and set @freq_ghz
   */
  void compute_freq_ghz();
  double freq_ghz;
};

static Nexus *nexus_object; /* The one per-process Nexus object */

/**
 * @brief The static signal handler, which executes the actual signal handler
 * with the one Nexus object.
 */
static void sigio_handler(int sig_num) {
  erpc_dprintf("eRPC Nexus: SIGIO handler called with nexus_object = %p\n",
               (void *)nexus_object);
  assert(sig_num == SIGIO);
  _unused(sig_num);
  nexus_object->session_mgnt_handler();
}

}  // End ERpc

#endif  // ERPC_RPC_H
