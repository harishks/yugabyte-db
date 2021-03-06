// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.
//
// The following only applies to changes made to this file as part of YugaByte development.
//
// Portions Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//
#ifndef YB_RPC_REACTOR_H_
#define YB_RPC_REACTOR_H_

#include <stdint.h>

#include <functional>
#include <list>
#include <map>
#include <memory>
#include <set>
#include <string>

#include <ev++.h> // NOLINT

#include <boost/intrusive/list.hpp>
#include <boost/utility.hpp>

#include "yb/gutil/ref_counted.h"

#include "yb/rpc/outbound_call.h"

#include "yb/util/thread.h"
#include "yb/util/locks.h"
#include "yb/util/monotime.h"
#include "yb/util/net/socket.h"
#include "yb/util/status.h"

namespace yb {
namespace rpc {

// When compiling on Mac OS X, use 'kqueue' instead of the default, 'select', for the event loop.
// Otherwise we run into problems because 'select' can't handle connections when more than 1024
// file descriptors are open by the process.
#if defined(__APPLE__)
constexpr unsigned int kDefaultLibEvFlags = ev::KQUEUE;
#else
constexpr unsigned int kDefaultLibEvFlags = ev::AUTO;
#endif

typedef std::list<ConnectionPtr> ConnectionList;

class DumpRunningRpcsRequestPB;
class DumpRunningRpcsResponsePB;
class Messenger;
class MessengerBuilder;
class Reactor;

// Simple metrics information from within a reactor.
struct ReactorMetrics {
  // Number of client RPC connections currently connected.
  int32_t num_client_connections_;
  // Number of server RPC connections currently connected.
  int32_t num_server_connections_;
};

// A task which can be enqueued to run on the reactor thread.
class ReactorTask : public std::enable_shared_from_this<ReactorTask> {
 public:
  ReactorTask();
  ReactorTask(const ReactorTask&) = delete;
  void operator=(const ReactorTask&) = delete;

  // Run the task. 'reactor' is guaranteed to be the current thread.
  virtual void Run(Reactor *reactor) = 0;

  // Abort the task, in the case that the reactor shut down before the task could be processed. This
  // may or may not run on the reactor thread itself.  If this is run not on the reactor thread,
  // then reactor thread should have already been shut down.
  //
  // The Reactor guarantees that the Reactor lock is free when this method is called.
  virtual void Abort(const Status &abort_status) {}

  virtual ~ReactorTask();
};

template <class F>
class FunctorReactorTask : public ReactorTask {
 public:
  explicit FunctorReactorTask(const F& f) : f_(f) {}

  void Run(Reactor* reactor) override  {
    f_(reactor);
  }
 private:
  F f_;
};

template <class F>
std::shared_ptr<ReactorTask> MakeFunctorReactorTask(const F& f) {
  return std::make_shared<FunctorReactorTask<F>>(f);
}

template <class F, class Object>
class FunctorReactorTaskWithWeakPtr : public ReactorTask {
 public:
  explicit FunctorReactorTaskWithWeakPtr(const F& f, const std::weak_ptr<Object>& ptr)
      : f_(f), ptr_(ptr) {}

  void Run(Reactor* reactor) override  {
    auto shared_ptr = ptr_.lock();
    if (shared_ptr) {
      f_(reactor);
    }
  }
 private:
  F f_;
  std::weak_ptr<Object> ptr_;
};

template <class F, class Object>
std::shared_ptr<ReactorTask> MakeFunctorReactorTask(const F& f,
                                                    const std::weak_ptr<Object>& ptr) {
  return std::make_shared<FunctorReactorTaskWithWeakPtr<F, Object>>(f, ptr);
}

template <class F, class Object>
std::shared_ptr<ReactorTask> MakeFunctorReactorTask(const F& f,
                                                    const std::shared_ptr<Object>& ptr) {
  return std::make_shared<FunctorReactorTaskWithWeakPtr<F, Object>>(f, ptr);
}

// A ReactorTask that is scheduled to run at some point in the future.
//
// Semantically it works like RunFunctionTask with a few key differences:
// 1. The user function is called during Abort. Put another way, the
//    user function is _always_ invoked, even during reactor shutdown.
// 2. To differentiate between Abort and non-Abort, the user function
//    receives a Status as its first argument.
class DelayedTask : public ReactorTask {
 public:
  DelayedTask(std::function<void(const Status&)> func, MonoDelta when, int64_t id,
              const std::shared_ptr<Messenger> messenger);

  // Schedules the task for running later but doesn't actually run it yet.
  virtual void Run(Reactor* reactor) override;

  // Behaves like ReactorTask::Abort.
  virtual void Abort(const Status& abort_status) override;

  // Could be called from non-reactor thread even before reactor thread shutdown.
  void AbortTask(const Status& abort_status);

 private:
  // Set done_ to true if not set and return true. If done_ is already set, return false.
  bool MarkAsDone();

  // libev callback for when the registered timer fires.
  void TimerHandler(ev::timer& rwatcher, int revents); // NOLINT

  // User function to invoke when timer fires or when task is aborted.
  const std::function<void(const Status&)> func_;

  // Delay to apply to this task.
  const MonoDelta when_;

  // Link back to registering reactor thread.
  Reactor* reactor_ = nullptr;

  // libev timer. Set when Run() is invoked.
  ev::timer timer_;

  // This task's id.
  const int64_t id_;

  std::shared_ptr<Messenger> messenger_;

  // Set to true whenever a Run or Abort methods are called.
  // Guarded by lock_.
  bool done_ = false;

  typedef simple_spinlock LockType;
  mutable LockType lock_;
};

class Reactor {
 public:
  // Client-side connection map.
  typedef std::unordered_map<const ConnectionId, ConnectionPtr, ConnectionIdHash> ConnectionMap;

  Reactor(const std::shared_ptr<Messenger>& messenger,
          int index,
          const MessengerBuilder &bld);

  Reactor(const Reactor&) = delete;
  void operator=(const Reactor&) = delete;

  // This may be called from another thread.
  CHECKED_STATUS Init();

  // Add any connections on this reactor thread into the given status dump.
  // May be called from another thread.
  CHECKED_STATUS DumpRunningRpcs(const DumpRunningRpcsRequestPB& req,
                         DumpRunningRpcsResponsePB* resp);

  // Block until the Reactor thread is shut down
  //
  // This must be called from another thread.
  void Shutdown();

  // This method is thread-safe.
  void WakeThread();

  // libev callback for handling async notifications in our epoll thread.
  void AsyncHandler(ev::async &watcher, int revents); // NOLINT

  // libev callback for handling timer events in our epoll thread.
  void TimerHandler(ev::timer &watcher, int revents); // NOLINT

  // This may be called from another thread.
  const std::string &name() const { return name_; }

  Messenger *messenger() const { return messenger_.get(); }

  CoarseMonoClock::TimePoint cur_time() const { return cur_time_; }

  // Drop all connections with remote address. Used in tests with broken connectivity.
  void DropWithRemoteAddress(const IpAddress& address);

  // Return true if this reactor thread is the thread currently
  // running. Should be used in DCHECK assertions.
  bool IsCurrentThread() const;

  // Indicates whether the reactor is shutting down.
  //
  // This method is thread-safe.
  bool closing() const;

  // Shut down the given connection, removing it from the connection tracking
  // structures of this reactor.
  //
  // The connection is not explicitly deleted -- shared_ptr reference counting
  // may hold on to the object after this, but callers should assume that it
  // _may_ be deleted by this call.
  void DestroyConnection(Connection *conn, const Status &conn_status);

  // Queue a new call to be sent. If the reactor is already shut down, marks
  // the call as failed.
  void QueueOutboundCall(OutboundCallPtr call);

  // Collect metrics.
  // Must be called from the reactor thread.
  CHECKED_STATUS GetMetrics(ReactorMetrics *metrics);

  void Join() { thread_->Join(); }

  // Queues a server event on all the connections, such that every client receives it.
  void QueueEventOnAllConnections(ServerEventListPtr server_event);

  // Queue a new incoming connection. Takes ownership of the underlying fd from
  // 'socket', but not the Socket object itself.
  // If the reactor is already shut down, takes care of closing the socket.
  void RegisterInboundSocket(Socket *socket, const Endpoint& remote);

  // Schedule the given task's Run() method to be called on the
  // reactor thread.
  // If the reactor shuts down before it is run, the Abort method will be
  // called.
  void ScheduleReactorTask(std::shared_ptr<ReactorTask> task);

  template<class F>
  void ScheduleReactorFunctor(const F& f) {
    ScheduleReactorTask(MakeFunctorReactorTask(f));
  }

 private:
  friend class Connection;
  friend class AssignOutboundCallTask;
  friend class DelayedTask;

  // Run the main event loop of the reactor.
  void RunThread();

  // Find or create a new connection to the given remote.
  // If such a connection already exists, returns that, otherwise creates a new one.
  // May return a bad Status if the connect() call fails.
  // The resulting connection object is managed internally by the reactor thread.
  // Deadline specifies latest time allowed for initializing the connection.
  CHECKED_STATUS FindOrStartConnection(const ConnectionId &conn_id,
                                       const MonoTime &deadline,
                                       ConnectionPtr* conn);

  // Scan any open connections for idle ones that have been idle longer than
  // connection_keepalive_time_
  void ScanIdleConnections();

  // Assign a new outbound call to the appropriate connection object.
  // If this fails, the call is marked failed and completed.
  ConnectionPtr AssignOutboundCall(const OutboundCallPtr &call);

  // Register a new connection.
  void RegisterConnection(const ConnectionPtr& conn);

  // Actually perform shutdown of the thread, tearing down any connections,
  // etc. This is called from within the thread.
  void ShutdownInternal();

  void ProcessOutboundQueue();

  void CheckReadyToStop();

  // If the Reactor is closing, returns false.
  // Otherwise, drains the pending_tasks_ queue into the provided list.
  bool DrainTaskQueue(std::vector<std::shared_ptr<ReactorTask>> *tasks);

  template<class F>
  CHECKED_STATUS RunOnReactorThread(const F& f);

  void CleanWaitingConnections();

  // parent messenger
  std::shared_ptr<Messenger> messenger_;

  const std::string name_;

  mutable simple_spinlock pending_tasks_lock_;

  // Whether the reactor is shutting down.
  // Guarded by pending_tasks_lock_.
  bool closing_ = false;

  // Tasks to be run within the reactor thread.
  // Guarded by lock_.
  std::vector<std::shared_ptr<ReactorTask>> pending_tasks_;

  scoped_refptr<yb::Thread> thread_;

  // our epoll object (or kqueue, etc).
  ev::dynamic_loop loop_;

  // Used by other threads to notify the reactor thread
  ev::async async_;

  // Handles the periodic timer.
  ev::timer timer_;

  // Scheduled (but not yet run) delayed tasks.
  std::set<std::shared_ptr<DelayedTask>> scheduled_tasks_;

  std::vector<std::shared_ptr<ReactorTask>> async_handler_tasks_;

  // The current monotonic time.  Updated every coarse_timer_granularity_.
  CoarseMonoClock::TimePoint cur_time_;

  // last time we did TCP timeouts.
  CoarseMonoClock::TimePoint last_unused_tcp_scan_;

  // Map of sockaddrs to Connection objects for outbound (client) connections.
  ConnectionMap client_conns_;

  // List of current connections coming into the server.
  ConnectionList server_conns_;

  // List of connections that should be completed before we could stop this thread.
  ConnectionList waiting_conns_;

  // If a connection has been idle for this much time, it is torn down.
  CoarseMonoClock::Duration connection_keepalive_time_;

  // Scan for idle connections on this granularity.
  CoarseMonoClock::Duration coarse_timer_granularity_;

  simple_spinlock outbound_queue_lock_;
  bool outbound_queue_stopped_ = false;
  // We found that should shutdown, but not all connections are ready for it.
  bool stopping_ = false;
  std::vector<OutboundCallPtr> outbound_queue_;
  std::vector<OutboundCallPtr> processing_outbound_queue_;
  std::vector<ConnectionPtr> processing_connections_;
  std::shared_ptr<ReactorTask> process_outbound_queue_task_;
};

}  // namespace rpc
}  // namespace yb

#endif // YB_RPC_REACTOR_H_
