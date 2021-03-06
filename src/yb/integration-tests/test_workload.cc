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


#include "yb/client/client.h"
#include "yb/client/client-test-util.h"
#include "yb/client/schema-internal.h"
#include "yb/client/table_handle.h"
#include "yb/client/yb_op.h"
#include "yb/common/schema.h"
#include "yb/common/wire_protocol-test-util.h"
#include "yb/gutil/stl_util.h"
#include "yb/gutil/strings/substitute.h"
#include "yb/integration-tests/mini_cluster.h"
#include "yb/integration-tests/test_workload.h"
#include "yb/util/env.h"
#include "yb/util/net/sockaddr.h"
#include "yb/util/random.h"
#include "yb/util/thread.h"

using namespace std::literals;

namespace yb {

using client::YBClient;
using client::YBClientBuilder;
using client::YBColumnSchema;;
using client::YBSchema;
using client::YBSchemaBuilder;
using client::YBSchemaFromSchema;
using client::YBSession;
using client::YBTable;
using client::YBTableCreator;
using client::YBTableType;
using client::YBTableName;
using std::shared_ptr;

const YBTableName TestWorkloadOptions::kDefaultTableName("my_keyspace", "test-workload");

class TestWorkload::State {
 public:
  explicit State(MiniClusterBase* cluster) : cluster_(cluster) {}

  void Start(const TestWorkloadOptions& options);
  void Setup(YBTableType table_type, const TestWorkloadOptions& options);

  void Stop() {
    should_run_.store(false, std::memory_order_release);
    start_latch_.Reset(0);
  }

  void Join() {
    for (const auto& thr : threads_) {
      CHECK_OK(ThreadJoiner(thr.get()).Join());
    }
    threads_.clear();
  }

  int64_t rows_inserted() const {
    return rows_inserted_.load(std::memory_order_acquire);
  }

  int64_t batches_completed() const {
    return batches_completed_.load(std::memory_order_acquire);
  }

 private:
  void WriteThread(const TestWorkloadOptions& options);

  MiniClusterBase* cluster_;
  std::shared_ptr<client::YBClient> client_;
  CountDownLatch start_latch_{0};
  std::atomic<bool> should_run_{false};
  std::atomic<int64_t> pathological_one_row_counter_{0};
  std::atomic<bool> pathological_one_row_inserted_{false};
  std::atomic<int64_t> rows_inserted_{0};
  std::atomic<int64_t> batches_completed_{0};
  std::atomic<int32_t> next_key_{0};

  std::vector<scoped_refptr<Thread> > threads_;
};

TestWorkload::TestWorkload(MiniClusterBase* cluster)
  : state_(new State(cluster)) {}

TestWorkload::~TestWorkload() {
  StopAndJoin();
}

TestWorkload::TestWorkload(TestWorkload&& rhs)
    : options_(rhs.options_), state_(std::move(rhs.state_)) {}

void TestWorkload::operator=(TestWorkload&& rhs) {
  options_ = rhs.options_;
  state_ = std::move(rhs.state_);
}

void TestWorkload::State::WriteThread(const TestWorkloadOptions& options) {
  Random r(Env::Default()->gettid());

  client::TableHandle table;
  // Loop trying to open up the table. In some tests we set up very
  // low RPC timeouts to test those behaviors, so this might fail and
  // need retrying.
  while (should_run_.load(std::memory_order_acquire)) {
    Status s = table.Open(options.table_name, client_.get());
    if (s.ok()) {
      break;
    }
    if (options.timeout_allowed && s.IsTimedOut()) {
      SleepFor(MonoDelta::FromMilliseconds(50));
      continue;
    }
    CHECK_OK(s);
  }

  shared_ptr<YBSession> session = client_->NewSession();
  session->SetTimeout(options.write_timeout);
  CHECK_OK(session->SetFlushMode(YBSession::MANUAL_FLUSH));

  // Wait for all of the workload threads to be ready to go. This maximizes the chance
  // that they all send a flood of requests at exactly the same time.
  //
  // This also minimizes the chance that we see failures to call OpenTable() if
  // a late-starting thread overlaps with the flood of outbound traffic from the
  // ones that are already writing data.
  start_latch_.CountDown();
  start_latch_.Wait();

  std::string test_payload("hello world");
  if (options.payload_bytes != 11) {
    // We fill with zeros if you change the default.
    test_payload.assign(options.payload_bytes, '0');
  }

  bool inserting_one_row = false;
  while (should_run_.load(std::memory_order_acquire)) {
    std::vector<client::YBqlOpPtr> ops;
    for (int i = 0; i < options.write_batch_size; i++) {
      if (options.pathological_one_row_enabled) {
        if (!pathological_one_row_inserted_) {
          if (++pathological_one_row_counter_ != 1) {
            continue;
          }
        } else {
          inserting_one_row = true;
          auto update = table.NewUpdateOp();
          auto req = update->mutable_request();
          QLAddInt32HashValue(req, 0);
          table.AddInt32ColumnValue(req, table.schema().columns()[1].name(), r.Next());
          ops.push_back(update);
          CHECK_OK(session->Apply(update));
          break;
        }
      }
      auto insert = table.NewInsertOp();
      auto req = insert->mutable_request();
      int32_t key;
      if (options.sequential_write) {
        key = ++next_key_;
      } else {
        key = options.pathological_one_row_enabled ? 0 : r.Next();
      }
      QLAddInt32HashValue(req, key);
      table.AddInt32ColumnValue(req, table.schema().columns()[1].name(), r.Next());
      table.AddStringColumnValue(req, table.schema().columns()[2].name(), test_payload);
      ops.push_back(insert);
      CHECK_OK(session->Apply(insert));
    }

    Status s = session->Flush();

    int inserted;
    inserted = 0;
    for (const auto& op : ops) {
      if (op->response().status() == QLResponsePB::YQL_STATUS_OK) {
        ++inserted;
      } else if (options.insert_failures_allowed) {
        VLOG(1) << "Op failed: " << op->ToString() << ": " << op->response().ShortDebugString();
      } else {
        LOG(FATAL) << "Op failed: " << op->ToString() << ": " << op->response().ShortDebugString();
      }
    }

    rows_inserted_.fetch_add(inserted, std::memory_order_release);
    if (inserted > 0) {
      batches_completed_.fetch_add(1, std::memory_order_release);
    }
    if (inserting_one_row && inserted <= 0) {
      pathological_one_row_counter_ = 0;
    }
  }
}

void TestWorkload::Setup(YBTableType table_type) {
  state_->Setup(table_type, options_);
}

void TestWorkload::State::Setup(YBTableType table_type, const TestWorkloadOptions& options) {
  client::YBClientBuilder client_builder;
  client_builder.default_rpc_timeout(options.default_rpc_timeout);
  CHECK_OK(cluster_->CreateClient(&client_builder, &client_));
  CHECK_OK(client_->CreateNamespaceIfNotExists(options.table_name.namespace_name()));

  bool table_exists = false;

  // Retry YBClient::TableExists() until we make that call retry reliably.
  // See KUDU-1074.
  MonoTime deadline(MonoTime::Now());
  deadline.AddDelta(MonoDelta::FromSeconds(10));
  Status s;
  while (true) {
    s = client_->TableExists(options.table_name, &table_exists);
    if (s.ok() || deadline.ComesBefore(MonoTime::Now())) break;
    SleepFor(MonoDelta::FromMilliseconds(10));
  }
  CHECK_OK(s);

  if (!table_exists) {
    YBSchema client_schema(YBSchemaFromSchema(GetSimpleTestSchema()));

    std::unique_ptr<YBTableCreator> table_creator(client_->NewTableCreator());
    CHECK_OK(table_creator->table_name(options.table_name)
             .schema(&client_schema)
             .num_replicas(options.num_replicas)
             .num_tablets(options.num_tablets)
             // NOTE: this is quite high as a timeout, but the default (5 sec) does not
             // seem to be high enough in some cases (see KUDU-550). We should remove
             // this once that ticket is addressed.
             .timeout(MonoDelta::FromSeconds(20))
             .table_type(table_type)
             .Create());
  } else {
    LOG(INFO) << "TestWorkload: Skipping table creation because table "
              << options.table_name.ToString() << " already exists";
  }
}

void TestWorkload::Start() {
  state_->Start(options_);
}

void TestWorkload::State::Start(const TestWorkloadOptions& options) {
  bool expected = false;
  should_run_.compare_exchange_strong(expected, true, std::memory_order_acq_rel);
  CHECK(!expected) << "Already started";
  should_run_.store(true, std::memory_order_release);
  start_latch_.Reset(options.num_write_threads);
  for (int i = 0; i < options.num_write_threads; ++i) {
    scoped_refptr<yb::Thread> new_thread;
    CHECK_OK(yb::Thread::Create("test", strings::Substitute("test-writer-$0", i),
                                &State::WriteThread, this, options,
                                &new_thread));
    threads_.push_back(new_thread);
  }
}

void TestWorkload::Stop() {
  state_->Stop();
}

void TestWorkload::Join() {
  state_->Join();
}

void TestWorkload::StopAndJoin() {
  Stop();
  Join();
}

void TestWorkload::WaitInserted(int64_t required) {
  while (rows_inserted() < required) {
    std::this_thread::sleep_for(100ms);
  }
}

int64_t TestWorkload::rows_inserted() const {
  return state_->rows_inserted();
}

int64_t TestWorkload::batches_completed() const {
  return state_->batches_completed();
}

}  // namespace yb
