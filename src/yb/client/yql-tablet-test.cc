//
// Copyright (c) YugaByte, Inc.
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
//

#include <shared_mutex>
#include <thread>

#include <boost/filesystem.hpp>

#include <boost/optional/optional.hpp>

#include "yb/client/yql-dml-test-base.h"

#include "yb/master/catalog_manager.h"
#include "yb/master/master.h"

#include "yb/sql/util/statement_result.h"

#include "yb/tserver/mini_tablet_server.h"
#include "yb/tserver/tablet_server.h"
#include "yb/tserver/tserver_service.proxy.h"

using namespace std::literals;

DECLARE_uint64(initial_seqno);

namespace yb {
namespace client {

using sql::RowsResult;

namespace {

const std::string kKey = "key"s;
const std::string kValue = "value"s;
const YBTableName kTable1Name("my_keyspace", "yql_client_test_table1");
const YBTableName kTable2Name("my_keyspace", "yql_client_test_table2");

int32_t ValueForKey(int32_t key) {
  return key * 2;
}

const int kTotalKeys = 250;
const int kBigSeqNo = 100500;

} // namespace

class YqlTabletTest : public YqlDmlTestBase {
 protected:
  void CreateTables(uint64_t initial_seqno1, uint64_t initial_seqno2) {
    google::FlagSaver saver;
    FLAGS_initial_seqno = initial_seqno1;
    CreateTable(kTable1Name, &table1_);
    FLAGS_initial_seqno = initial_seqno2;
    CreateTable(kTable2Name, &table2_);
  }

  void SetValue(const YBSessionPtr& session, int32_t key, int32_t value, TableHandle* table) {
    const auto op = table->NewWriteOp(YQLWriteRequestPB::YQL_STMT_INSERT);
    auto* const req = op->mutable_request();
    YBPartialRow *prow = op->mutable_row();
    table->SetInt32ColumnValue(req->add_hashed_column_values(), kKey, key, prow, 0);
    table->SetInt32ColumnValue(req->add_column_values(), kValue, value);
    ASSERT_OK(session->Apply(op));
    ASSERT_EQ(YQLResponsePB::YQL_STATUS_OK, op->response().status());
  }

  boost::optional<int32_t> GetValue(const YBSessionPtr& session, int32_t key, TableHandle* table) {
    const auto op = CreateReadOp(key, table);
    EXPECT_OK(session->Apply(op));
    auto rowblock = RowsResult(op.get()).GetRowBlock();
    if (rowblock->row_count() == 0) {
      return boost::none;
    }
    EXPECT_EQ(1, rowblock->row_count());
    return rowblock->row(0).column(0).int32_value();
  }

  std::shared_ptr<YBqlReadOp> CreateReadOp(int32_t key, TableHandle* table) {
    auto op = table->NewReadOp();
    auto req = op->mutable_request();
    auto prow = op->mutable_row();
    table->SetInt32ColumnValue(req->add_hashed_column_values(), kKey, key, prow, 0);
    auto value_column_id = table->ColumnId(kValue);
    req->add_column_ids(value_column_id);
    req->mutable_column_refs()->add_ids(value_column_id);
    return op;
  }

  void CreateTable(const YBTableName& table_name, TableHandle* table) {
    YBSchemaBuilder builder;
    builder.AddColumn(kKey)->Type(INT32)->HashPrimaryKey()->NotNull();
    builder.AddColumn(kValue)->Type(INT32);

    table->Create(table_name, client_.get(), &builder);
  }

  void FillTable(int begin, int end, TableHandle* table) {
    {
      auto session = client_->NewSession(false /* read_only */);
      for (int i = begin; i != end; ++i) {
        SetValue(session, i, ValueForKey(i), table);
      }
    }
    VerifyTable(begin, end, table);
    ASSERT_OK(WaitSync(begin, end, table));
  }

  void VerifyTable(int begin, int end, TableHandle* table) {
    auto session = client_->NewSession(true /* read_only */);
    for (int i = begin; i != end; ++i) {
      auto value = GetValue(session, i, table);
      ASSERT_TRUE(value.is_initialized()) << "i: " << i << ", table: " << table->name().ToString();
      ASSERT_EQ(ValueForKey(i), *value) << "i: " << i << ", table: " << table->name().ToString();
    }
  }

  CHECKED_STATUS WaitSync(int begin, int end, TableHandle* table) {
    auto deadline = MonoTime::FineNow() + MonoDelta::FromSeconds(30);

    master::GetTableLocationsRequestPB req;
    master::GetTableLocationsResponsePB resp;
    req.set_max_returned_locations(std::numeric_limits<uint32_t>::max());
    table->name().SetIntoTableIdentifierPB(req.mutable_table());
    RETURN_NOT_OK(
        cluster_->mini_master()->master()->catalog_manager()->GetTableLocations(&req, &resp));
    std::vector<master::TabletLocationsPB> tablets;
    std::unordered_set<std::string> replicas;
    for (const auto& tablet : resp.tablet_locations()) {
      tablets.emplace_back(tablet);
      for (const auto& replica : tablet.replicas()) {
        replicas.insert(replica.ts_info().permanent_uuid());
      }
    }
    for (const auto& replica : replicas) {
      RETURN_NOT_OK(DoWaitSync(deadline, tablets, replica, begin, end, table));
    }
    return Status::OK();
  }

  CHECKED_STATUS DoWaitSync(const MonoTime& deadline,
                            const std::vector<master::TabletLocationsPB>& tablets,
                            const std::string& replica,
                            int begin,
                            int end,
                            TableHandle* table) {
    auto tserver = cluster_->find_tablet_server(replica);
    if (!tserver) {
      return STATUS_FORMAT(NotFound, "Tablet server for $0 not found", replica);
    }
    auto endpoint = tserver->server()->rpc_server()->GetBoundAddresses().front();
    auto proxy = std::make_unique<tserver::TabletServerServiceProxy>(
        tserver->server()->messenger(), endpoint);

    auto condition = [&]() -> Result<bool> {
      // int total_rows = 0;
      for (int i = begin; i != end; ++i) {
        bool found = false;
        for (const auto& tablet : tablets) {
          tserver::ReadRequestPB req;
          {
            std::string partition_key;
            auto op = CreateReadOp(i, table);
            RETURN_NOT_OK(op->GetPartitionKey(&partition_key));
            auto* yql_batch = req.add_yql_batch();
            *yql_batch = op->request();
            yql_batch->set_hash_code(PartitionSchema::DecodeMultiColumnHashValue(partition_key));
          }

          tserver::ReadResponsePB resp;
          rpc::RpcController controller;
          controller.set_timeout(MonoDelta::FromSeconds(1));
          req.set_tablet_id(tablet.tablet_id());
          req.set_consistency_level(YBConsistencyLevel::CONSISTENT_PREFIX);
          proxy->Read(req, &resp, &controller);

          const auto& yql_batch = resp.yql_batch(0);
          if (yql_batch.status() != YQLResponsePB_YQLStatus_YQL_STATUS_OK) {
            return STATUS_FORMAT(RemoteError,
                                 "Bad resp status: $0",
                                 YQLResponsePB_YQLStatus_Name(yql_batch.status()));
          }
          std::vector<ColumnSchema> columns = { table->schema().columns()[1] };
          Slice data;
          RETURN_NOT_OK(controller.GetSidecar(yql_batch.rows_data_sidecar(), &data));
          yb::sql::RowsResult result(table->name(), columns, data.ToBuffer());
          auto row_block = result.GetRowBlock();
          if (row_block->row_count() == 1) {
            if (found) {
              return STATUS_FORMAT(Corruption, "Key found twice: $0", i);
            }
            auto value = row_block->row(0).column(0).int32_value();
            if (value != ValueForKey(i)) {
              return STATUS_FORMAT(Corruption,
                                   "Wrong value for key: $0, expected: $1",
                                   value,
                                   ValueForKey(i));
            }
            found = true;
          }
        }
        if (!found) {
          return STATUS_FORMAT(NotFound, "Key not found: $0", i);
        }
      }
      return true;
    };

    return Wait(condition, deadline, "Waiting for replication");
  }

  CHECKED_STATUS Import() {
    std::this_thread::sleep_for(1s); // Wait until all tablets a synced and flushed.
    cluster_->FlushTablets();

    auto source_infos = GetTabletInfos(kTable1Name);
    auto dest_infos = GetTabletInfos(kTable2Name);
    EXPECT_EQ(source_infos.size(), dest_infos.size());
    for (size_t i = 0; i != source_infos.size(); ++i) {
      std::string start1, end1, start2, end2;
      {
        auto& metadata = source_infos[i]->metadata();
        std::shared_lock<std::remove_reference<decltype(metadata)>::type> lock(metadata);
        const auto& partition = metadata.state().pb.partition();
        start1 = partition.partition_key_start();
        end1 = partition.partition_key_end();
      }
      {
        auto& metadata = dest_infos[i]->metadata();
        std::shared_lock<std::remove_reference<decltype(metadata)>::type> lock(metadata);
        const auto& partition = metadata.state().pb.partition();
        start2 = partition.partition_key_start();
        end2 = partition.partition_key_end();
      }
      EXPECT_EQ(start1, start2);
      EXPECT_EQ(end1, end2);
    }
    for (int i = 0; i != cluster_->num_tablet_servers(); ++i) {
      auto* tablet_manager = cluster_->mini_tablet_server(i)->server()->tablet_manager();
      for (size_t j = 0; j != source_infos.size(); ++j) {
        tablet::TabletPeerPtr source_peer, dest_peer;
        tablet_manager->LookupTablet(source_infos[j]->id(), &source_peer);
        EXPECT_NE(nullptr, source_peer);
        auto source_dir = source_peer->tablet()->metadata()->rocksdb_dir();
        tablet_manager->LookupTablet(dest_infos[j]->id(), &dest_peer);
        EXPECT_NE(nullptr, dest_peer);
        auto status = dest_peer->tablet()->ImportData(source_dir);
        if (!status.ok() && !status.IsNotFound()) {
          return status;
        }
      }
    }
    return Status::OK();
  }

  scoped_refptr<master::TableInfo> GetTableInfo(const YBTableName& table_name) {
    auto* catalog_manager = cluster_->leader_mini_master()->master()->catalog_manager();
    std::vector<scoped_refptr<master::TableInfo>> all_tables;
    catalog_manager->GetAllTables(&all_tables);
    scoped_refptr<master::TableInfo> table_info;
    for (auto& table : all_tables) {
      if (table->name() == table_name.table_name()) {
        return table;
      }
    }
    return nullptr;
  }

  std::vector<scoped_refptr<master::TabletInfo>> GetTabletInfos(const YBTableName& table_name) {
    auto table_info = GetTableInfo(table_name);
    EXPECT_NE(nullptr, table_info);
    std::vector<scoped_refptr<master::TabletInfo>> tablets;
    table_info->GetAllTablets(&tablets);
    return tablets;
  }

  TableHandle table1_;
  TableHandle table2_;
};

TEST_F(YqlTabletTest, ImportToEmpty) {
  CreateTables(0, kBigSeqNo);

  FillTable(0, kTotalKeys, &table1_);
  ASSERT_OK(Import());
  VerifyTable(0, kTotalKeys, &table1_);
  VerifyTable(0, kTotalKeys, &table2_);
}

TEST_F(YqlTabletTest, ImportToNonEmpty) {
  CreateTables(0, kBigSeqNo);

  FillTable(0, kTotalKeys, &table1_);
  FillTable(kTotalKeys, 2 * kTotalKeys, &table2_);
  ASSERT_OK(Import());
  VerifyTable(0, 2 * kTotalKeys, &table2_);
}

TEST_F(YqlTabletTest, ImportToEmptyAndRestart) {
  CreateTables(0, kBigSeqNo);

  FillTable(0, kTotalKeys, &table1_);
  ASSERT_OK(Import());
  VerifyTable(0, kTotalKeys, &table2_);

  ASSERT_OK(cluster_->RestartSync());
  VerifyTable(0, kTotalKeys, &table1_);
  VerifyTable(0, kTotalKeys, &table2_);
}

TEST_F(YqlTabletTest, ImportToNonEmptyAndRestart) {
  CreateTables(0, kBigSeqNo);

  FillTable(0, kTotalKeys, &table1_);
  FillTable(kTotalKeys, 2 * kTotalKeys, &table2_);

  ASSERT_OK(Import());
  VerifyTable(0, 2 * kTotalKeys, &table2_);

  ASSERT_OK(cluster_->RestartSync());
  VerifyTable(0, kTotalKeys, &table1_);
  VerifyTable(0, 2 * kTotalKeys, &table2_);
}

TEST_F(YqlTabletTest, LateImport) {
  CreateTables(kBigSeqNo, 0);

  FillTable(0, kTotalKeys, &table1_);
  ASSERT_NOK(Import());
}

TEST_F(YqlTabletTest, OverlappedImport) {
  CreateTables(kBigSeqNo - 2, kBigSeqNo);

  FillTable(0, kTotalKeys, &table1_);
  FillTable(kTotalKeys, 2 * kTotalKeys, &table2_);
  ASSERT_NOK(Import());
}

} // namespace client
} // namespace yb
