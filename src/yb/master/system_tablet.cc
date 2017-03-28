// Copyright (c) YugaByte, Inc.

#include "yb/master/system_tablet.h"
#include "yb/server/hybrid_clock.h"

namespace yb {
namespace master {

SystemTablet::SystemTablet(const Schema& schema, common::YQLStorageIf* yql_storage,
                           const TabletId& tablet_id)
    : schema_(schema),
      yql_storage_(yql_storage),
      tablet_id_(tablet_id) {
}

const Schema& SystemTablet::SchemaRef() const {
  return schema_;
}

const common::YQLStorageIf& SystemTablet::YQLStorage() const {
  return *yql_storage_;
}

TableType SystemTablet::table_type() const {
  return TableType::YQL_TABLE_TYPE;
}

const TabletId& SystemTablet::tablet_id() const {
  return tablet_id_;
}

void SystemTablet::RegisterReaderTimestamp(HybridTime read_point) {
  // NOOP.
}

void SystemTablet::UnregisterReader(HybridTime read_point) {
  // NOOP.
}

HybridTime SystemTablet::SafeTimestampToRead() const {
  // HybridTime doesn't matter for SystemTablets.
  return HybridTime::kMax;
}

CHECKED_STATUS SystemTablet::HandleRedisReadRequest(
    HybridTime timestamp, const RedisReadRequestPB& redis_read_request,
    RedisResponsePB* response) {
  return STATUS(NotSupported, "RedisReadRequest is not supported for system tablets!");
}

CHECKED_STATUS SystemTablet::CreatePagingStateForRead(const YQLReadRequestPB& yql_read_request,
                                                      const YQLRowBlock& rowblock,
                                                      YQLResponsePB* response) const {
  // We don't support pagination for system tablets. Although we need to return an OK() status
  // here since we don't want to raise this as an error to the client, but just want to avoid
  // populating any paging state here for the client.
  return Status::OK();
}

}  // namespace master
}  // namespace yb
