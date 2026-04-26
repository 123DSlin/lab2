/* Copyright (c) 2021 Xie Meiyi(xiemeiyi@hust.edu.cn) and OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "storage/table/lsm_table_engine.h"
#include "common/log/log.h"
#include "storage/db/db.h"
#include "storage/record/lsm_record_scanner.h"
#include "storage/common/codec.h"

RC LsmTableEngine::insert_record(Record &record)
{
  RC rc = RC::SUCCESS;
  // TODO: set auto increment id, and keep durability.
  // TODO: support set primary key as a part of lsm_key.
  bytes lsm_key;
  Codec::encode(table_->table_id(), inc_id_.fetch_add(1), lsm_key);
  rc = lsm_->put(string_view((char *)lsm_key.data(), lsm_key.size()), string_view(record.data(), record.len()));
  return rc;
}

RC LsmTableEngine::delete_record(const Record &record)
{
  if (lsm_ == nullptr) {
    LOG_ERROR("lsm engine is null");
    return RC::INTERNAL;
  }
  // Deletion is represented as a tombstone: empty value.
  // The RecordScanner for LSM sets Record::key() to the encoded LSM key.
  const string &k = record.key();
  if (k.empty()) {
    LOG_WARN("lsm delete requires record key");
    return RC::INVALID_ARGUMENT;
  }
  return lsm_->put(string_view(k.data(), k.size()), string_view());
}

RC LsmTableEngine::get_record_scanner(RecordScanner *&scanner, Trx *trx, ReadWriteMode mode)
{
  scanner = new LsmRecordScanner(table_, db_->lsm(), trx);
  RC rc = scanner->open_scan();
  if (rc != RC::SUCCESS) {
    LOG_ERROR("failed to open scanner. rc=%s", strrc(rc));
  }
  return rc;
}

RC LsmTableEngine::open()
{
  if (lsm_ == nullptr) {
    LOG_ERROR("lsm engine is null");
    return RC::INTERNAL;
  }
  inc_id_.store(0);
  return RC::SUCCESS;
}
