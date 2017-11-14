## Code notes
 * An important simplifying insight is that all functions invoked by the event
   loop run to completion, i.e., we cannot be preempted, leaving objects in
   an invalid state.
 * Major types
   * Request type: `uint8_t`. 256 request types should be enough.
   * App TIDs: `uint8_t`. We need one per thread, so 256 is enough.
   * Session number: `uint16_t`. We need one per session, and we don't expect
     to support over 65,536 sessions per thread. More sessions can be supported
     with multiple threads.
   * Sequence numbers: `uint64_t` - not `size_t`
   * Numa nodes: `size_t`. These are not transferred over the network, so no
     need to shrink.
 * Use exceptions in constructors. No exceptions in destructors.
 * If a function throws an exception, its documentation should say so.
 * Do not append integers directly to string streams. If the integer is
   `uint8_t`, it will get interpreted as a character.
 * Do not initialize struct/class members in the struct/class definition
   (except maybe in major classes like Rpc/Nexus/Session). This only causes
   confusion.

## Misc notes
 * The Rpc object cannot be destroyed by a background thread or while the
   foreground thread is in the event loop (i.e., by the request handler or
   continuation user code). This means that an initially valid Rpc object will
   stay valid for an interation of the event loop.
 * A continuation must call `release_response()` before returning. However,
   a request handler may or may not call `enqueue_response()` before returning;
   this is required to support nested RPCs, where the response cannot be
   immediately generated by the request handler.

## MsgBuffer ownership notes
 * The correctness of this reasoning depends on restrictions on the request
   handler and continuation functions. These functions can only enqueue requests
   and responses, but cannot invoke the event loop. The event loop can modify
   RX ring memory, which is unsafe if a fake buffer's ownership has been passed
   to the application.
 * The two interesting cases first:
   * Request MsgBuffer ownership at server: The request handler loses ownership
     of the request MsgBuffer when it returns, or when it calls
     `enqueue_response()`, whichever is first.
      * When `enqueue_response()` is called, we allow the client to send us
        a new request. So we cannot wait until the request handler returns to
        bury the request MsgBuffer. If we do so, we can bury a newer request's
        MsgBuffer when a background request handler returns.
      * The app loses ownership of the request MsgBuffer when the request
        handler returns, even though eRPC may delay burying it until
        `enqueue_response()` is called. This constraint is required because we
        execute foreground handlers for short requests off the RX ring.
      * Request handle ownership is released on `enqueue_response()`, which may
        happen after the request handler returns.
   * Response MsgBuffer ownership at client: The response MsgBuffer is owned by
     the user.
 * The two less interesting cases:
   * Request MsgBuffer ownership at client: Request MsgBuffers are owned and
     allocated by apps and are never freed by eRPC. The client temporarily loses
     ownership of the request MsgBuffer until the continuation for the request
     is invoked, at which point it is no longer needed for retransmission.
   * Response MsgBuffer ownership at server: The request handler loses ownership
     of the response MsgBuffer when it calls `enqueue_response()`. eRPC will
     free it when the response is no longer needed for retransmission.
 * Burying invariants:
   * At clients, the request MsgBuffer is buried (nullified, since the request
     MsgBuffer is app-owned) on receiving the complete response, and before
     invoking the continuation. So, a null value of `sslot->tx_msgbuf` indicates
     that the sslot is not waiting for a response. Similarly, a non-null value
     of `sslot->tx_msgbuf` indicates that the sslot is waiting for a response.
     This is used to invoke failure continuations during session resets.

## RPC failures
 * On session reset, the client may get continuation-with-failure callbacks.
   These are identical to non-failure continuations, but the user-owned response
   MsgBuffer is temporarily resized to 0. 

## General session management notes
 * Applications generate session management packets using eRPC API calls
   `create_session()` and `destroy_session()`. `create_session()` sends a
   connect request, and `destroy_session()` sends a disconnect request.
 * The Nexus runs a session management thread that listens for session
   management packets. This thread enqueues received packets into the session
   management queue of the Rpc specified by the packet's destination application
   TID.
 * To handle session management requests, an Rpc must enter its event loop.
   Although session management packets can be generated outside the event loop,
   they can only be handled inside an event loop.
 * On entering the event loop, the Rpc checks its Nexus hook for new session
   management packets. If there are new packets, it invokes SM handlers.

## Session management invariants
 * XXX: The RPC's session connect/disconnect request/response handlers are
   guaranteed to see a non-resetted session.
 * XXX: A server's reset handler always sees a connected session. This is
   because a server session is immediately destroyed on processing an SM
   disconnect request.
 * XXX: A client's reset handler may see a session in the connected or
   disconnect-in-progress state.

## Session management retries
 * A session management operation is retried by the client in these cases:
   * XXX: Add cases here
   * A session connect request fails because the server does not have the
     requested RPC ID running, and `retry_connect_on_invalid_rpc_id` is set.

## Compile-time optimization notes:
 * Optimizations for `small_rpc_tput`:
   * Each of these optimizations can help a fair bit (5-10%) individually, but
     the benefits do not stack.
   * Set optlevel to extreme.
   * Disable datapath checks and datapath stats.
   * Use O2 intead of O3. Try profile-guided optimization.
   * Use power-of-two number of sessions and avoid Lemire's trick in app.
   * In the continuation function, reduce frequency of scanning for stagnated
     batches.
 * Optimizations for `consensus`:
   * Set session request window to 1, or implement Rpc flush.
   * Set transport max inline size to 120 bytes for ConnectX-3.
 * Setting `small_rpc_optlevel` to `small_rpc_optlevel_extreme` will disable
   support for large messages and background threads.
 * Setting `FAULT_INJECTION` to off will disable support to inject eRPC faults
   at runtime.

## Short-term TODOs
 * Use TX flush on client-side retransmission
 * Destroy session test fails with `kSessionCredits = 1`, `kSessionReqWindow = 1`
 * Make apps use BasicAppContext and `basic_sm_handler`.
 * Use `rt_assert` in src and apps
 * In IBTransport, check if MLX environment vars are set. Do it in constructor.
 * RFR sending needs to be paced, so we cannot use `send_rfr_now`.
 * Handle `poll_cq` and `post_send` failures in IBTransport. Do it by moving
   RpcDatapathErrCode from rpc.h to common.h, and using it in IBTransport.
 * Do we need separate `rx_burst()` and `post_recvs()` functions in Transport?

## Long-term TODOs
 * Enable marking an Rpc object as server-only. Such an Rpc object can bypass
   packet loss detection code in the event loop.
 * Optimize mem-copies using `rte_memcpy`.
 * Create session objects from a hugepage-backed pool to reduce TLB misses.
 * The first packet size limit should be much smaller than MTU to improve RTT
   measurement accuracy (e.g., it could be around 256 bytes). This will need
   many changes, mostly to code that uses TTr::kMaxDataPerPkt.
   * We must ensure that a small response message packet or a credit return
     packet is never delayed by TX queueing (even by congestion control--related
     TX queueing) or we'll mess up RTT measurement.
   * Credit return packets will need the packet number field so that the
     receiver can match RTT.
 * Handle MsgBuffer allocation failures in eRPC.

## Longer-term TODOs
 * Replace exit(-1) in non-destructor code with `rt_assert` IF it doesn't
   reduce perf (`rt_assert` takes a string argument - does that cause overhead?)
 * Less frequent use of `rdtsc()`
 * Optimize `pkthdr_0` filling using preconstructed headers.
 * Are we losing some performance by using `size_t` instead of `uint32_t` in
   in-memory structs like Buffer and MsgBuffer?
 * Need to have a test for session management request timeouts.
