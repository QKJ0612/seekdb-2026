/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define USING_LOG_PREFIX SQL_DAS
#include "sql/das/ob_das_insert_op.h"
#include "sql/das/ob_das_ai_column_task.h"
#include "share/schema/ob_multi_version_schema_service.h"
#include "share/schema/ob_schema_getter_guard.h"
#include "share/schema/ob_table_schema.h"
#include "storage/ob_table_dml_param.h"
#include "storage/ob_value_row_iterator.h"
#include "storage/access/ob_dml_param.h"
#include "storage/tx_storage/ob_access_service.h"
#include "storage/blocksstable/ob_datum_row.h"
#include "share/rc/ob_server_runtime.h"
#include "data_plane/api/data_plane/blocksstable/ob_datum_row_factory.h"
#include "lib/string/ob_sql_string.h"
#include "lib/utility/ob_macro_utils.h"
#include "share/ob_server_struct.h"
#include "data_plane/ob_i_dml_service.h"
#include "data_plane/ob_i_write_context_service.h"
#include "data_plane/transaction/ob_i_transaction_service.h"
#include "share/rc/ob_server_runtime.h"
#include "sql/engine/dml/ob_dml_service.h"
#include "sql/das/ob_das_domain_utils.h"

namespace oceanbase
{
namespace common
{
namespace serialization
{
template <>
struct EnumEncoder<false, const sql::ObDASInsCtDef *> : sql::DASCtRefEncoder<sql::ObDASInsCtDef>
{
};

template <>
struct EnumEncoder<false, sql::ObDASInsRtDef *> : sql::DASRtRefEncoder<sql::ObDASInsRtDef>
{
};
} // end namespace serialization
} // end namespace common

using namespace common;
using namespace storage;
using namespace share;
namespace sql
{
template <>
int ObDASIndexDMLAdaptor<DAS_OP_TABLE_INSERT, ObDASDMLIterator>::write_rows(const ObTabletID &tablet_id,
                                                                            const CtDefType &ctdef,
                                                                            RtDefType &rtdef,
                                                                            ObDASDMLIterator &iter,
                                                                            int64_t &affected_rows)
{
  int ret = OB_SUCCESS;
  data_plane::ObIDmlService *as = ::oceanbase::share::server_service<::oceanbase::data_plane::ObIDmlService>();
  if (rtdef.use_put_) {
    ret = as->put_rows(tablet_id,
                       *tx_desc_,
                       dml_execution_,
                       ctdef.column_ids_,
                       &iter,
                       affected_rows);
  } else {
    ret = as->insert_rows(tablet_id,
                          *tx_desc_,
                          dml_execution_,
                          ctdef.column_ids_,
                          &iter,
                          affected_rows);
  }
  if (OB_FAIL(ret)) {
    if (OB_TRY_LOCK_ROW_CONFLICT != ret) {
    }
  } else if (!(ctdef.is_ignore_ || ctdef.table_param_.get_data_table().is_domain_index())
      && 0 == affected_rows) {
    ret = OB_ERR_UNEXPECTED;
  }
  return ret;
}

ObDASInsertOp::ObDASInsertOp(ObIAllocator &op_alloc)
  : ObIDASTaskOp(op_alloc),
    ins_ctdef_(nullptr),
    ins_rtdef_(nullptr),
    result_(nullptr),
    affected_rows_(0),
    is_duplicated_(false)
{
}

int ObDASInsertOp::open_op()
{
  int ret = OB_SUCCESS;
  if (ins_rtdef_->need_fetch_conflict_ && OB_FAIL(insert_row_with_fetch())) {
  } else if (!ins_rtdef_->need_fetch_conflict_ && OB_FAIL(insert_rows())) {
  }

  return ret;
}

int ObDASInsertOp::insert_rows()
{
  int ret = OB_SUCCESS;
  int64_t affected_rows = 0;
  ObDASDMLIterator dml_iter(
      ins_ctdef_, insert_buffer_, op_alloc_, srs_provider_,
      lob_read_options_);
  ObDASIndexDMLAdaptor<DAS_OP_TABLE_INSERT, ObDASDMLIterator> ins_adaptor;
  ins_adaptor.tx_desc_ = trans_desc_;
  ins_adaptor.snapshot_ = snapshot_;
  ins_adaptor.write_branch_id_ = write_branch_id_;
  ins_adaptor.ctdef_ = ins_ctdef_;
  ins_adaptor.rtdef_ = ins_rtdef_;
  ins_adaptor.related_ctdefs_ = &related_ctdefs_;
  ins_adaptor.related_rtdefs_ = &related_rtdefs_;
  ins_adaptor.tablet_id_ = tablet_id_;
  ins_adaptor.related_tablet_ids_ = &related_tablet_ids_;
  ins_adaptor.use_snapshot_opt_ = das_snapshot_opt_info_.use_specify_snapshot_;
  ins_adaptor.das_allocator_ = &op_alloc_;
  if (OB_FAIL(ins_adaptor.write_tablet(dml_iter, affected_rows))) {
    if (OB_TRY_LOCK_ROW_CONFLICT != ret) {
    }
  } else {
    affected_rows_ = affected_rows;
    int tmp_ret = OB_SUCCESS;
    if (OB_TMP_FAIL(create_ai_column_tasks_for_insert(ins_ctdef_->table_param_.get_data_table().get_table_id(),
                                                      *ins_ctdef_,
                                                      insert_buffer_,
                                                      trans_desc_,
                                                      snapshot_,
                                                      op_alloc_))) {
      LOG_WARN("create ai column tasks failed", K(tmp_ret), K(ins_ctdef_->table_param_.get_data_table().get_table_id()));
    }
  }
  return ret;
}
int ObDASInsertOp::insert_index_with_fetch(data_plane::ObDmlExecution &execution,
                                           data_plane::ObIDmlService *as,
                                           blocksstable::ObDatumRowIterator &dml_iter,
                                           ObDASConflictIterator *result_iter,
                                           const ObDASInsCtDef *ins_ctdef,
                                           ObDASInsRtDef *ins_rtdef,
                                           data_plane::ObWriteContext &write_context,
                                           const UIntFixedArray *duplicated_column_ids,
                                           common::ObTabletID tablet_id,
                                           transaction::ObTxReadSnapshot *snapshot)
{
  int ret = OB_SUCCESS;
  int64_t affected_rows = 0;
  blocksstable::ObDatumRowIterator *duplicated_rows = NULL;
  if (OB_FAIL(ObDMLService::prepare_dml_execution(
          *ins_ctdef,
          *ins_rtdef,
          *snapshot,
          write_branch_id_,
          op_alloc_,
          write_context,
          execution,
          das_snapshot_opt_info_.use_specify_snapshot_))) {
  } else if (OB_FAIL(as->insert_rows_fetch_duplicates(
          tablet_id,
          *trans_desc_,
          execution,
          ins_ctdef->column_ids_,
          *duplicated_column_ids,
          &dml_iter,
          data_plane::ObDuplicateReturnMode::ALL,
          affected_rows,
          duplicated_rows))) {
    if (OB_ERR_PRIMARY_KEY_DUPLICATE == ret) {
      ret = OB_SUCCESS;
      bool is_local_index_table = ins_ctdef->table_param_.get_data_table().is_index_local_storage();
      bool is_unique_index = ins_ctdef->table_param_.get_data_table().is_unique_index();
      if (is_local_index_table && !is_unique_index) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_ISNULL(duplicated_rows)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(result_iter->get_duplicated_iter_array().push_back(duplicated_rows))) {
      } else {
        is_duplicated_ = true;
      }
    }
  }
  if (OB_FAIL(ret) && OB_NOT_NULL(duplicated_rows)) {
    as->free_duplicate_rows_iterator(duplicated_rows);
    duplicated_rows = NULL;
  }
  return ret;
}

int ObDASInsertOp::insert_row_with_fetch()
{
  int ret = OB_SUCCESS;
  int64_t affected_rows = 0;
  ObDASConflictIterator *result_iter = nullptr;
  void *buf = nullptr;
  data_plane::ObIDmlService *as = ::oceanbase::share::server_service<::oceanbase::data_plane::ObIDmlService>();
  data_plane::ObDmlExecution execution;
  ObDASDMLIterator dml_iter(
      ins_ctdef_, insert_buffer_, op_alloc_, srs_provider_,
      lob_read_options_);
  data_plane::ObWriteContext write_context;
  concurrent_control::ObWriteFlag write_flag;
  transaction::ObTxReadSnapshot *snapshot = snapshot_;

  // write_flag should be inited before get store ctx, as it will be used in the call function
  (void)ObDMLService::init_dml_write_flag(*ins_ctdef_, *ins_rtdef_, write_flag, das_snapshot_opt_info_.use_specify_snapshot_);
  if (das_snapshot_opt_info_.get_specify_snapshot()) {
    data_plane::ObITransactionService *txs = nullptr;
    if (das_snapshot_opt_info_.isolation_level_ != transaction::ObTxIsolationLevel::RC) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_ISNULL(txs = data_plane::query_transaction_service())) {
      ret = OB_ERR_UNEXPECTED;
      LOG_ERROR("get_tx_service", K(ret));
    } else if (OB_FAIL(txs->get_read_snapshot(*trans_desc_,
                                              das_snapshot_opt_info_.isolation_level_,
                                              THIS_WORKER.get_timeout_ts(),
                                              *das_snapshot_opt_info_.get_response_snapshot()))) {
    } else {
      snapshot = das_snapshot_opt_info_.get_response_snapshot();
    }
  }

  if (OB_FAIL(ret)) {
    // do nothing
  } else if (ins_ctdef_->table_rowkey_types_.empty()) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(::oceanbase::share::server_service<::oceanbase::data_plane::ObIWriteContextService>()->acquire_write_context(
          ins_rtdef_->timeout_ts_,
          *trans_desc_,
          *snapshot,
          write_branch_id_,
          write_flag,
          write_context))) {
  } else if (OB_ISNULL(buf = op_alloc_.alloc(sizeof(ObDASConflictIterator)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    result_iter = new(buf) ObDASConflictIterator(ins_ctdef_->table_rowkey_types_,
                                                 op_alloc_);
    result_ = result_iter;
  }

  // 1. insert primary table
  if (OB_FAIL(ret)) {
    // do nothing
  } else if (OB_FAIL(insert_index_with_fetch(execution,
                                             as,
                                             dml_iter,
                                             result_iter,
                                             ins_ctdef_,
                                             ins_rtdef_,
                                             write_context,
                                             &ins_ctdef_->table_rowkey_cids_,
                                             tablet_id_,
                                             snapshot))) {
  }

  // 2. insert unique index
  for (int64_t i = 0; OB_SUCC(ret) && i < related_ctdefs_.count(); ++i) {
    const ObDASInsCtDef *index_ins_ctdef = static_cast<const ObDASInsCtDef*>(related_ctdefs_.at(i));
    ObDASInsRtDef *index_ins_rtdef = static_cast<ObDASInsRtDef*>(related_rtdefs_.at(i));
    ObTabletID index_tablet_id = related_tablet_ids_.at(i);

    const bool is_local_unique_index = index_ins_ctdef->table_param_.get_data_table().is_unique_index();
    if (!is_local_unique_index) {
      // insert it later
    } else if (OB_FAIL(dml_iter.rewind(index_ins_ctdef, nullptr/*fts_doc_word_info*/))) {
    } else if (OB_FAIL(insert_index_with_fetch(execution,
                                               as,
                                               dml_iter,
                                               result_iter,
                                               index_ins_ctdef,
                                               index_ins_rtdef,
                                               write_context,
                                               &ins_ctdef_->table_rowkey_cids_,
                                               index_tablet_id,
                                               snapshot))) {
    }
  }

  // 3. insert non_unique index
  for (int64_t i = 0; OB_SUCC(ret) && i < related_ctdefs_.count(); ++i) {
    const ObDASInsCtDef *index_ins_ctdef = static_cast<const ObDASInsCtDef*>(related_ctdefs_.at(i));
    ObDASInsRtDef *index_ins_rtdef = static_cast<ObDASInsRtDef*>(related_rtdefs_.at(i));
    ObTabletID index_tablet_id = related_tablet_ids_.at(i);
    const bool is_local_unique_index = index_ins_ctdef->table_param_.get_data_table().is_unique_index();
    if (is_local_unique_index) {
      // insert it before
    } else if (is_duplicated_) {
    } else if (OB_FAIL(dml_iter.rewind(index_ins_ctdef, nullptr/*fts_doc_word_info*/))) {
    } else {
      if (OB_FAIL(insert_index_with_fetch(execution,
                                          as,
                                          dml_iter,
                                          result_iter,
                                          index_ins_ctdef,
                                          index_ins_rtdef,
                                          write_context,
                                          &(index_ins_ctdef->column_ids_),
                                          index_tablet_id,
                                          snapshot))) {
      }
    }
  }

  if (OB_SUCC(ret)) {
    result_iter->init_curr_iter();
  }

  return ret;
}

int ObDASInsertOp::release_op()
{
  int ret = OB_SUCCESS;
  if (result_ != NULL) {
    result_->reset();
    result_ = NULL;
  }
  return ret;
}

int ObDASInsertOp::record_task_result_to_rtdef()
{
  int ret = OB_SUCCESS;
  ins_rtdef_->affected_rows_ += affected_rows_;
  ins_rtdef_->is_duplicated_ |= is_duplicated_;
  return ret;
}
int ObDASInsertOp::assign_task_result(ObIDASTaskOp *other)
{
  int ret = OB_SUCCESS;
  if (other->get_type() != get_type()) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    ObDASInsertOp *ins_op = static_cast<ObDASInsertOp *>(other);
    affected_rows_ = ins_op->get_affected_rows();
    is_duplicated_ = ins_op->get_is_duplicated();
  }
  return ret;
}

int ObDASInsertOp::init_task_info(uint32_t row_extend_size)
{
  int ret = OB_SUCCESS;
  if (!insert_buffer_.is_inited()
      && OB_FAIL(insert_buffer_.init(op_alloc_, row_extend_size, "DASInsertBuffer"))) {
  }
  return ret;
}

int ObDASInsertOp::write_row(const ExprFixedArray &row,
                             ObEvalCtx &eval_ctx,
                             ObChunkDatumStore::StoredRow *&stored_row)
{
  int ret = OB_SUCCESS;
  bool added = false;
  if (!insert_buffer_.is_inited()) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(insert_buffer_.add_row(row, &eval_ctx, stored_row, true))) {
  }
  return ret;
}

OB_SERIALIZE_MEMBER((ObDASInsertOp, ObIDASTaskOp),
                    ins_ctdef_,
                    ins_rtdef_,
                    insert_buffer_);

void ObDASConflictIterator::reset()
{
  ObDuplicatedIterList::iterator iter = duplicated_iter_list_.begin();
  for (; iter != duplicated_iter_list_.end(); ++iter) {
    ::oceanbase::share::server_service<::oceanbase::data_plane::ObIDmlService>()->free_duplicate_rows_iterator(*iter);
  }
  duplicated_iter_list_.reset();
}

int ObDASConflictIterator::get_next_row(ObDatumRow *&row)
{
  int ret = OB_SUCCESS;
  bool find_next_iter = false;
  ObDatumRow *dup_row = NULL;
  do {
    if (find_next_iter) {
      ++curr_iter_;
      find_next_iter = false;
    }
    if (curr_iter_ == duplicated_iter_list_.end()) {
      ret = OB_ITER_END;
    } else {
      blocksstable::ObDatumRowIterator *dup_row_iter = *curr_iter_;
      if (OB_ISNULL(dup_row_iter)) {
        find_next_iter = true;
      } else if (OB_FAIL(dup_row_iter->get_next_row(dup_row))) {
        if (OB_ITER_END == ret) {
          ret = OB_SUCCESS;
          find_next_iter = true;
        } else {
        }
      } else if (OB_ISNULL(dup_row)) {
        ret = OB_INVALID_ARGUMENT;
      } else {
      }
    }
  } while (OB_SUCC(ret) && find_next_iter);

  if (OB_SUCC(ret)) {
    row = dup_row;
  }
  return ret;
}

}  // namespace sql
}  // namespace oceanbase

namespace oceanbase
{
namespace sql
{
thread_local bool g_ai_worker_write = false;

namespace ai_column_task
{
static const char *const REQ_SUFFIX = "__ai_request";

static bool name_ends_with(const common::ObString &name, const char *suffix)
{
  bool bret = false;
  const int64_t slen = static_cast<int64_t>(STRLEN(suffix));
  if (name.length() > slen) {
    bret = (0 == MEMCMP(name.ptr() + name.length() - slen, suffix, slen));
  }
  return bret;
}

static int collect_ai_columns(const share::schema::ObTableSchema &schema,
                              common::ObIArray<uint64_t> &col_ids,
                              common::ObIArray<common::ObString> &col_names,
                              common::ObIAllocator &alloc)
{
  int ret = common::OB_SUCCESS;
  const int64_t cnt = schema.get_column_count();
  for (int64_t i = 0; OB_SUCC(ret) && i < cnt; ++i) {
    const share::schema::ObColumnSchemaV2 *col = schema.get_column_schema_by_idx(i);
    if (NULL == col || !col->is_hidden()) {
      continue;
    }
    common::ObString name = col->get_column_name_str();
    if (!name_ends_with(name, REQ_SUFFIX)) {
      continue;
    }
    common::ObString base(static_cast<int32_t>(name.length() - STRLEN(REQ_SUFFIX)), name.ptr());
    const share::schema::ObColumnSchemaV2 *base_col = schema.get_column_schema(base);
    if (NULL == base_col) {
      continue;
    }
    common::ObString copy;
    if (OB_FAIL(ob_write_string(alloc, base, copy))) {
    } else if (OB_FAIL(col_ids.push_back(base_col->get_column_id()))) {
    } else if (OB_FAIL(col_names.push_back(copy))) {
    }
  }
  return ret;
}

static int escape_sql_literal(common::ObSqlString &out, const common::ObString &in)
{
  int ret = common::OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < in.length(); ++i) {
    const char c = in.ptr()[i];
    if ('\'' == c || '\\' == c) {
      ret = out.append(&c, 1);
    }
    if (OB_SUCC(ret)) {
      ret = out.append(&c, 1);
    }
  }
  return ret;
}

static int format_datum(common::ObSqlString &key, common::ObSqlString &pred,
                        const common::ObString &col_name, const common::ObDatum &datum,
                        const common::ObObjType type)
{
  int ret = common::OB_SUCCESS;
  if (common::ObIntType == type || common::ObInt32Type == type || common::ObUInt64Type == type) {
    if (OB_FAIL(key.append_fmt("%ld", datum.get_int()))) {
    } else if (OB_FAIL(pred.append_fmt("`%.*s` = %ld", col_name.length(), col_name.ptr(), datum.get_int()))) {
    }
  } else if (common::ObDoubleType == type || common::ObFloatType == type) {
    if (OB_FAIL(key.append_fmt("%.17g", datum.get_double()))) {
    } else if (OB_FAIL(pred.append_fmt("`%.*s` = %.17g", col_name.length(), col_name.ptr(), datum.get_double()))) {
    } else {
    }
  } else {
    common::ObString val = datum.get_string();
    if (OB_FAIL(key.append(val))) {
    } else if (OB_FAIL(pred.append_fmt("`%.*s` = '", col_name.length(), col_name.ptr()))) {
    } else if (OB_FAIL(escape_sql_literal(pred, val))) {
    } else if (OB_FAIL(pred.append("'"))) {
    }
  }
  return ret;
}

static int build_rowkey_by_cols(const share::schema::ObTableSchema &schema,
                                const common::ObIArray<uint64_t> &column_ids,
                                const blocksstable::ObDatumRow &row,
                                common::ObSqlString &key,
                                common::ObSqlString &pred)
{
  int ret = common::OB_SUCCESS;
  common::ObSEArray<uint64_t, 4> rowkey_cids;
  if (OB_FAIL(schema.get_rowkey_column_ids(rowkey_cids))) {
    LOG_WARN("get schema rowkey cids failed", K(ret));
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < rowkey_cids.count(); ++i) {
    const uint64_t cid = rowkey_cids.at(i);
    int64_t pos = -1;
    for (int64_t j = 0; j < column_ids.count(); ++j) {
      if (column_ids.at(j) == cid) {
        pos = j;
        break;
      }
    }
    const share::schema::ObColumnSchemaV2 *col = schema.get_column_schema(cid);
    if (pos < 0 || NULL == col) {
      ret = common::OB_ERR_UNEXPECTED;
      LOG_ERROR("DBG rowkey miss", K(ret), K(cid), K(pos), "n_cids", column_ids.count());
    } else if (row.storage_datums_[pos].is_null()) {
      ret = common::OB_ITEM_NOT_MATCH;
      LOG_WARN("rowkey is null, skip ai task", K(ret), K(cid));
    } else {
      if (i > 0) {
        if (OB_FAIL(key.append("&"))) {
        } else if (OB_FAIL(pred.append(" and "))) {
        }
      }
      if (OB_SUCC(ret) && OB_FAIL(format_datum(key, pred, col->get_column_name_str(),
                                               row.storage_datums_[pos], col->get_data_type()))) {
        LOG_ERROR("DBG format_datum failed", K(ret), K(cid));
      }
      LOG_ERROR("DBG rowkey part", K(i), K(cid), K(pos), "key_len", key.length(), "pred_len", pred.length());
    }
  }
  return ret;
}

static int build_rowkey(const share::schema::ObTableSchema &schema,
                        const ObDASInsCtDef &ctdef,
                        const blocksstable::ObDatumRow &row,
                        common::ObSqlString &key,
                        common::ObSqlString &pred)
{
  int ret = common::OB_SUCCESS;
  if (ctdef.table_rowkey_cids_.count() > 0) {
    common::ObSEArray<uint64_t, 4> cids;
    for (int64_t i = 0; OB_SUCC(ret) && i < ctdef.table_rowkey_cids_.count(); ++i) {
      if (OB_FAIL(cids.push_back(ctdef.table_rowkey_cids_.at(i)))) {
        LOG_WARN("push rowkey cid failed", K(ret));
      }
    }
    if (OB_SUCC(ret)) {
      ret = build_rowkey_by_cols(schema, cids, row, key, pred);
    }
  } else {
    ret = build_rowkey_by_cols(schema, ctdef.column_ids_, row, key, pred);
  }
  return ret;
}

static int write_task_rows(common::ObIAllocator &alloc,
                           transaction::ObTxDesc *tx_desc,
                           const transaction::ObTxReadSnapshot &snapshot,
                           const share::schema::ObTableSchema &task_schema,
                           const common::ObIArray<uint64_t> &column_ids,
                           common::ObIArray<blocksstable::ObDatumRow *> &rows)
{
  int ret = common::OB_SUCCESS;
  storage::ObAccessService *oas = ::oceanbase::share::server_service<storage::ObAccessService>();
  share::schema::ObTableDMLParam table_param(alloc);
  storage::ObDMLBaseParam dml_param;
  storage::ObStoreCtxGuard store_ctx_guard;
  const int64_t timeout_us = ObTimeUtility::current_time() + 10 * 1000 * 1000;
  if (OB_ISNULL(oas) || OB_ISNULL(tx_desc)) {
    ret = common::OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(table_param.convert(&task_schema, task_schema.get_schema_version(), column_ids))) {
    LOG_WARN("convert table dml param failed", K(ret));
  } else if (OB_FAIL([&]() -> int {
               int ret = OB_SUCCESS;
               share::schema::ObSchemaGetterGuard sguard;
               int64_t schema_version = 0;
               if (OB_FAIL(share::schema::ObMultiVersionSchemaService::get_instance().get_runtime_schema_guard(sguard))) {
               } else if (OB_FAIL(sguard.get_schema_version(schema_version))) {
               } else {
                 dml_param.runtime_schema_version_ = schema_version;
               }
               return ret;
             }())) {
  } else {
    dml_param.sql_mode_ = SMO_DEFAULT;
    dml_param.write_flag_.reset();
    dml_param.write_flag_.set_is_insert_up();
    dml_param.table_param_ = &table_param;
    dml_param.timeout_ = timeout_us;
    dml_param.snapshot_.assign(snapshot);
    dml_param.branch_id_ = 0;
    dml_param.store_ctx_guard_ = &store_ctx_guard;
    dml_param.schema_version_ = task_schema.get_schema_version();
    dml_param.dml_allocator_ = &alloc;
    storage::ObValueRowIterator row_iter;
    if (OB_FAIL(row_iter.init())) {
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < rows.count(); ++i) {
      if (OB_FAIL(row_iter.add_row(*rows.at(i)))) {
      }
    }
    ret = oas->get_write_store_ctx_guard(timeout_us, *tx_desc, snapshot, 0,
                                        dml_param.write_flag_, store_ctx_guard);
    if (OB_SUCC(ret)) {
      int64_t affected_rows = 0;
      // insert first: it participates in the user transaction, so the task is
      // visible inside the transaction and rolled back with it.
      ret = oas->insert_rows(task_schema.get_tablet_id(), *tx_desc, dml_param,
                             column_ids, &row_iter, affected_rows);
      if (common::OB_ERR_PRIMARY_KEY_DUPLICATE == ret) {
        // a pending task for the same rowkey already exists: refresh it in place
        common::ObSEArray<uint64_t, 8> updated_ids;
        for (int64_t ci = 3; OB_SUCC(ret) && ci < column_ids.count(); ++ci) {
          ret = updated_ids.push_back(column_ids.at(ci));
        }
        storage::ObValueRowIterator retry_iter;
        if (OB_SUCC(ret) && OB_FAIL(retry_iter.init())) {
          LOG_WARN("init retry iter failed", K(ret));
        }
        for (int64_t ri = 0; OB_SUCC(ret) && ri < rows.count(); ++ri) {
          if (OB_FAIL(retry_iter.add_row(*rows.at(ri)))) {
            LOG_WARN("add retry row failed", K(ret), K(ri));
          }
        }
        if (OB_SUCC(ret)) {
          // a pending task for this rowkey exists: replace it in place so the
          // newer request wins, still inside the user transaction.
          dml_param.write_flag_.reset();
          ret = oas->delete_rows(task_schema.get_tablet_id(), *tx_desc, dml_param,
                                 column_ids, &retry_iter, affected_rows);
          if (OB_SUCC(ret)) {
            dml_param.write_flag_.set_is_insert_up();
            storage::ObValueRowIterator ins_iter;
            if (OB_FAIL(ins_iter.init())) {
              LOG_WARN("init retry insert iter failed", K(ret));
            }
            for (int64_t ri = 0; OB_SUCC(ret) && ri < rows.count(); ++ri) {
              if (OB_FAIL(ins_iter.add_row(*rows.at(ri)))) {
                LOG_WARN("add retry insert row failed", K(ret), K(ri));
              }
            }
            if (OB_SUCC(ret)) {
              ret = oas->insert_rows(task_schema.get_tablet_id(), *tx_desc, dml_param,
                                     column_ids, &ins_iter, affected_rows);
            }
          }
        }
      }
    }
    store_ctx_guard.reset();
  }
  return ret;
}

} // namespace ai_column_task

// defined below; used by the update path to refresh pending tasks in place
static int delete_task_rows(common::ObIAllocator &alloc,
                            transaction::ObTxDesc *tx_desc,
                            const transaction::ObTxReadSnapshot &snapshot,
                            const share::schema::ObTableSchema &task_schema,
                            const common::ObIArray<uint64_t> &column_ids,
                            common::ObIArray<blocksstable::ObDatumRow *> &rows);

int create_ai_column_tasks_for_insert(const uint64_t table_id,
                                      const ObDASInsCtDef &ctdef,
                                      ObDASWriteBuffer &write_buffer,
                                      transaction::ObTxDesc *tx_desc,
                                      const transaction::ObTxReadSnapshot *snapshot,
                                      common::ObIAllocator &alloc)
{
  int ret = common::OB_SUCCESS;
  share::schema::ObSchemaGetterGuard guard;
  const share::schema::ObTableSchema *table_schema = NULL;
  const share::schema::ObTableSchema *task_schema = NULL;
  common::ObSEArray<uint64_t, 4> ai_col_ids;
  common::ObSEArray<common::ObString, 4> ai_col_names;
  common::ObSEArray<uint64_t, 8> task_column_ids;
  common::ObSEArray<blocksstable::ObDatumRow *, 16> task_rows;
  static const char *const ai_task_col_names[] = {"table_id", "column_id", "rowkey_text", "gmt_create",
                                                  "gmt_modified", "request_id", "rowkey_predicate", "state",
                                                  "retry_count", "eligible_ts"};
  const int64_t task_col_cnt = static_cast<int64_t>(sizeof(ai_task_col_names) / sizeof(ai_task_col_names[0]));
  if (OB_ISNULL(tx_desc) || OB_ISNULL(snapshot)) {
    ret = common::OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(share::schema::ObMultiVersionSchemaService::get_instance().get_runtime_schema_guard(guard))) {
  } else if (OB_FAIL(guard.get_table_schema(table_id, table_schema))) {
  } else if (OB_ISNULL(table_schema)) {
    ret = common::OB_TABLE_NOT_EXIST;
  } else if (OB_FAIL(ai_column_task::collect_ai_columns(*table_schema, ai_col_ids, ai_col_names, alloc))) {
  } else if (OB_FALSE_IT(true)) {
  } else if (ai_col_ids.empty()) {
  } else if (OB_FAIL(guard.get_table_schema(share::OB_ALL_AI_COLUMN_TASK_TID, task_schema))) {
  } else if (OB_ISNULL(task_schema)) {
    ret = common::OB_TABLE_NOT_EXIST;
    LOG_WARN("ai column task table not found", K(ret));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < task_col_cnt; ++i) {
      const share::schema::ObColumnSchemaV2 *col = task_schema->get_column_schema(ai_task_col_names[i]);
      if (NULL == col) {
        ret = common::OB_ERR_UNEXPECTED;
        LOG_WARN("task table column not found", K(ret), K(i));
      } else if (OB_FAIL(task_column_ids.push_back(col->get_column_id()))) {
      }
    }
  }
  if (OB_SUCC(ret) && !ai_col_ids.empty()) {
    ObDASWriteBuffer::NewRowIterator dml_iter;
    blocksstable::ObDatumRow *dml_row = NULL;
    int64_t row_idx = 0;
    const int64_t batch_id = ObTimeUtility::current_time();
    if (OB_FAIL(write_buffer.begin(dml_iter, ctdef.column_types_))) {
      LOG_WARN("begin write buffer iterate failed", K(ret));
    }
    while (OB_SUCC(ret) && OB_SUCC(dml_iter.get_next_row(dml_row))) {
      common::ObSqlString key;
      common::ObSqlString pred;
      if (OB_FAIL(ai_column_task::build_rowkey(*table_schema, ctdef, *dml_row, key, pred))) {
        if (common::OB_ITEM_NOT_MATCH == ret) {
          LOG_WARN("skip row for ai task: rowkey is null");
        } else {
          LOG_ERROR("build rowkey failed", K(ret));
        }
        ret = common::OB_SUCCESS;  // never fail the dml because of ai task
        continue;
      }
      for (int64_t c = 0; OB_SUCC(ret) && c < ai_col_ids.count(); ++c) {
        blocksstable::ObDatumRow *task_row = NULL;
        common::ObString key_text;
        common::ObString pred_text;
        common::ObString req_text;
        common::ObSqlString req_id;
        if (OB_FAIL(ob_write_string(alloc, key.string(), key_text))) {
        } else if (OB_FAIL(ob_write_string(alloc, pred.string(), pred_text))) {
        } else if (OB_FAIL(req_id.append_fmt("%ld-%ld", batch_id, row_idx))) {
        } else if (OB_FAIL(ob_write_string(alloc, req_id.string(), req_text))) {
        } else if (OB_FAIL(data_plane::create_datum_row(alloc, task_col_cnt, task_row))) {
        } else if (OB_ISNULL(task_row)) {
          ret = common::OB_ERR_UNEXPECTED;
        } else {
          task_row->row_flag_.set_flag(blocksstable::ObDmlFlag::DF_INSERT);
          const int64_t now_us = ObTimeUtility::current_time();
          task_row->storage_datums_[0].set_int(static_cast<int64_t>(table_id));
          task_row->storage_datums_[1].set_int(static_cast<int64_t>(ai_col_ids.at(c)));
          task_row->storage_datums_[2].set_string(key_text);
          task_row->storage_datums_[3].set_timestamp(now_us);
          task_row->storage_datums_[4].set_timestamp(now_us);
          task_row->storage_datums_[5].set_string(req_text);
          task_row->storage_datums_[6].set_string(pred_text);
          task_row->storage_datums_[7].set_int(0);
          task_row->storage_datums_[8].set_int(0);
          task_row->storage_datums_[9].set_timestamp(now_us);
          if (OB_FAIL(task_rows.push_back(task_row))) {
          }
        }
      }
      ++row_idx;
    }
    if (common::OB_ITER_END == ret) {
      ret = common::OB_SUCCESS;
    }
    if (OB_SUCC(ret) && !task_rows.empty()) {
      if (OB_FAIL(ai_column_task::write_task_rows(alloc, tx_desc, *snapshot, *task_schema,
                                                  task_column_ids, task_rows))) {
        LOG_ERROR("write ai column tasks failed", K(ret), K(task_rows.count()));
      }
    }
  }
  return ret;
}

int create_ai_column_tasks_for_update(const uint64_t table_id,
                                      const ObDASUpdCtDef &ctdef,
                                      ObDASWriteBuffer &write_buffer,
                                      transaction::ObTxDesc *tx_desc,
                                      const transaction::ObTxReadSnapshot *snapshot,
                                      common::ObIAllocator &alloc)
{
  int ret = common::OB_SUCCESS;
  share::schema::ObSchemaGetterGuard guard;
  const share::schema::ObTableSchema *table_schema = NULL;
  const share::schema::ObTableSchema *task_schema = NULL;
  common::ObSEArray<uint64_t, 4> touched_ai_ids;
  if (g_ai_worker_write) {
    // the worker writing a computed value back must not invalidate the column again
    return common::OB_SUCCESS;
  }
  common::ObSEArray<uint64_t, 4> task_column_ids;
  common::ObSEArray<uint64_t, 4> rowkey_ids;
  common::ObSEArray<int64_t, 4> rowkey_pos;
  common::ObSEArray<blocksstable::ObDatumRow *, 16> task_rows;
  static const char *const ai_task_col_names[] = {"table_id", "column_id", "rowkey_text", "gmt_create",
                                                  "gmt_modified", "request_id", "rowkey_predicate", "state",
                                                  "retry_count", "eligible_ts"};
  const int64_t task_col_cnt = static_cast<int64_t>(sizeof(ai_task_col_names) / sizeof(ai_task_col_names[0]));
  if (OB_ISNULL(tx_desc) || OB_ISNULL(snapshot)) {
    ret = common::OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(share::schema::ObMultiVersionSchemaService::get_instance().get_runtime_schema_guard(guard))) {
  } else if (OB_FAIL(guard.get_table_schema(table_id, table_schema))) {
  } else if (OB_ISNULL(table_schema)) {
    ret = common::OB_TABLE_NOT_EXIST;
  } else {
    // collect ai result columns of this table, then keep the touched ones
    common::ObSEArray<uint64_t, 4> all_ai_ids;
    common::ObSEArray<common::ObString, 4> all_ai_names;
    if (OB_FAIL(ai_column_task::collect_ai_columns(*table_schema, all_ai_ids, all_ai_names, alloc))) {
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < all_ai_ids.count(); ++i) {
      bool touched = false;
      for (int64_t k = 0; !touched && k < ctdef.updated_column_ids_.count(); ++k) {
        if (ctdef.updated_column_ids_.at(k) == all_ai_ids.at(i)) {
          touched = true;
        }
      }
      if (touched && OB_FAIL(touched_ai_ids.push_back(all_ai_ids.at(i)))) {
      }
    }
  }
  if (OB_SUCC(ret) && !touched_ai_ids.empty()) {
    if (OB_FAIL(guard.get_table_schema(share::OB_ALL_AI_COLUMN_TASK_TID, task_schema))) {
    } else if (OB_ISNULL(task_schema)) {
      ret = common::OB_TABLE_NOT_EXIST;
    } else if (OB_FAIL(table_schema->get_rowkey_column_ids(rowkey_ids))) {
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < task_col_cnt; ++i) {
        const share::schema::ObColumnSchemaV2 *col = task_schema->get_column_schema(ai_task_col_names[i]);
        if (NULL == col) {
          ret = common::OB_ERR_UNEXPECTED;
        } else if (OB_FAIL(task_column_ids.push_back(col->get_column_id()))) {
        }
      }
      for (int64_t r = 0; OB_SUCC(ret) && r < rowkey_ids.count(); ++r) {
        int64_t pos = -1;
        for (int64_t c = 0; -1 == pos && c < ctdef.column_ids_.count(); ++c) {
          if (ctdef.column_ids_.at(c) == rowkey_ids.at(r)) {
            pos = c;
          }
        }
        if (-1 == pos) {
          ret = common::OB_ITEM_NOT_MATCH;
        } else if (OB_FAIL(rowkey_pos.push_back(pos))) {
        }
      }
    }
  }
  if (OB_SUCC(ret) && !touched_ai_ids.empty()) {
    ObDASWriteBuffer::NewRowIterator dml_iter;
    blocksstable::ObDatumRow *dml_row = NULL;
    int64_t row_idx = 0;
    const int64_t batch_id = ObTimeUtility::current_time();
    if (OB_FAIL(write_buffer.begin(dml_iter, ctdef.column_types_))) {
    }
    while (OB_SUCC(ret) && OB_SUCC(dml_iter.get_next_row(dml_row))) {
      common::ObSqlString key;
      common::ObSqlString pred;
      bool skip = false;
      for (int64_t r = 0; OB_SUCC(ret) && !skip && r < rowkey_pos.count(); ++r) {
        const int64_t pos = rowkey_pos.at(r);
        const share::schema::ObColumnSchemaV2 *col = table_schema->get_column_schema(rowkey_ids.at(r));
        if (NULL == col) {
          ret = common::OB_ERR_UNEXPECTED;
        } else if (dml_row->storage_datums_[pos].is_null()) {
          skip = true;
        } else {
          if (r > 0) {
            if (OB_FAIL(key.append("&"))) {
            } else if (OB_FAIL(pred.append(" and "))) {
            }
          }
          if (OB_SUCC(ret) && OB_FAIL(ai_column_task::format_datum(key, pred, col->get_column_name_str(),
                                                                   dml_row->storage_datums_[pos],
                                                                   col->get_data_type()))) {
            LOG_WARN("format rowkey datum failed", K(ret));
          }
        }
      }
      if (OB_FAIL(ret)) {
        ret = common::OB_SUCCESS;
        continue;
      }
      if (skip) {
        continue;
      }
      for (int64_t c = 0; OB_SUCC(ret) && c < touched_ai_ids.count(); ++c) {
        blocksstable::ObDatumRow *task_row = NULL;
        // only an invalidated (set to NULL) ai column needs a new task; the
        // worker writing a real value back must not enqueue another task.
        int64_t ai_pos = -1;
        for (int64_t pc = 0; -1 == ai_pos && pc < ctdef.column_ids_.count(); ++pc) {
          if (ctdef.column_ids_.at(pc) == touched_ai_ids.at(c)) {
            ai_pos = pc;
          }
        }
        // The update row image carries the pre-update value for untouched columns,
        // so a NULL check cannot tell an invalidation from a normal write.  Never
        // create tasks from the worker's own write-back instead.
        if (-1 == ai_pos || g_ai_worker_write) {
          continue;
        }
        common::ObString key_text;
        common::ObString pred_text;
        common::ObString req_text;
        common::ObSqlString req_id;
        if (OB_FAIL(ob_write_string(alloc, key.string(), key_text))) {
        } else if (OB_FAIL(ob_write_string(alloc, pred.string(), pred_text))) {
        } else if (OB_FAIL(req_id.append_fmt("%ld-%ld", batch_id, row_idx))) {
        } else if (OB_FAIL(ob_write_string(alloc, req_id.string(), req_text))) {
        } else if (OB_FAIL(data_plane::create_datum_row(alloc, task_col_cnt, task_row))) {
        } else if (OB_ISNULL(task_row)) {
          ret = common::OB_ERR_UNEXPECTED;
        } else {
          task_row->row_flag_.set_flag(blocksstable::ObDmlFlag::DF_INSERT);
          const int64_t now_us = ObTimeUtility::current_time();
          task_row->storage_datums_[0].set_int(static_cast<int64_t>(table_id));
          task_row->storage_datums_[1].set_int(static_cast<int64_t>(touched_ai_ids.at(c)));
          task_row->storage_datums_[2].set_string(key_text);
          task_row->storage_datums_[3].set_timestamp(now_us);
          task_row->storage_datums_[4].set_timestamp(now_us);
          task_row->storage_datums_[5].set_string(req_text);
          task_row->storage_datums_[6].set_string(pred_text);
          task_row->storage_datums_[7].set_int(0);
          task_row->storage_datums_[8].set_int(0);
          // short settle window: keep a just-committed task visible to its own
          // transaction before the worker consumes it.
          task_row->storage_datums_[9].set_timestamp(now_us + 200 * 1000);
          if (OB_FAIL(task_rows.push_back(task_row))) {
          }
        }
      }
      ++row_idx;
    }
    if (common::OB_ITER_END == ret) {
      ret = common::OB_SUCCESS;
    }
    if (OB_SUCC(ret) && !task_rows.empty()) {
      // refresh: drop any pending task for the same (table, column, rowkey) so
      // the newest request wins instead of hitting a primary key conflict.
      common::ObSEArray<uint64_t, 4> refresh_pk_ids;
      common::ObSEArray<blocksstable::ObDatumRow *, 16> old_rows;
      for (int64_t pi = 0; OB_SUCC(ret) && pi < 3 && pi < task_column_ids.count(); ++pi) {
        if (OB_FAIL(refresh_pk_ids.push_back(task_column_ids.at(pi)))) {
          LOG_WARN("push refresh pk id failed", K(ret));
        }
      }
      for (int64_t ti = 0; OB_SUCC(ret) && ti < task_rows.count(); ++ti) {
        blocksstable::ObDatumRow *del_row = NULL;
        if (OB_FAIL(data_plane::create_datum_row(alloc, 3, del_row))) {
          LOG_WARN("create refresh del row failed", K(ret));
        } else if (OB_ISNULL(del_row)) {
          ret = common::OB_ERR_UNEXPECTED;
        } else {
          del_row->row_flag_.set_flag(blocksstable::ObDmlFlag::DF_DELETE);
          for (int64_t di = 0; di < 3; ++di) {
            del_row->storage_datums_[di] = task_rows.at(ti)->storage_datums_[di];
          }
          if (OB_FAIL(old_rows.push_back(del_row))) {
            LOG_WARN("push refresh del row failed", K(ret));
          }
        }
      }
      if (OB_SUCC(ret) && !old_rows.empty()) {
        delete_task_rows(alloc, tx_desc, *snapshot, *task_schema, refresh_pk_ids, old_rows);
      }
      if (OB_FAIL(ai_column_task::write_task_rows(alloc, tx_desc, *snapshot, *task_schema,
                                                  task_column_ids, task_rows))) {
        LOG_WARN("write ai column tasks for update failed", K(ret), K(task_rows.count()));
      }
    }
  }
  return ret;
}


static int delete_task_rows(common::ObIAllocator &alloc,
                            transaction::ObTxDesc *tx_desc,
                            const transaction::ObTxReadSnapshot &snapshot,
                            const share::schema::ObTableSchema &task_schema,
                            const common::ObIArray<uint64_t> &column_ids,
                            common::ObIArray<blocksstable::ObDatumRow *> &rows)
{
  int ret = common::OB_SUCCESS;
  storage::ObAccessService *oas = ::oceanbase::share::server_service<storage::ObAccessService>();
  share::schema::ObTableDMLParam table_param(alloc);
  storage::ObDMLBaseParam dml_param;
  storage::ObStoreCtxGuard store_ctx_guard;
  const int64_t timeout_us = ObTimeUtility::current_time() + 10 * 1000 * 1000;
  if (OB_ISNULL(oas) || OB_ISNULL(tx_desc)) {
    ret = common::OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(table_param.convert(&task_schema, task_schema.get_schema_version(), column_ids))) {
    LOG_WARN("convert task dml param failed", K(ret));
  } else {
    share::schema::ObSchemaGetterGuard sguard;
    int64_t schema_version = 0;
    if (OB_FAIL(share::schema::ObMultiVersionSchemaService::get_instance().get_runtime_schema_guard(sguard))) {
    } else if (OB_FAIL(sguard.get_schema_version(schema_version))) {
    } else {
      dml_param.runtime_schema_version_ = schema_version;
    }
    if (OB_SUCC(ret)) {
      dml_param.sql_mode_ = SMO_DEFAULT;
      dml_param.write_flag_.reset();
      dml_param.table_param_ = &table_param;
      dml_param.timeout_ = timeout_us;
      dml_param.snapshot_.assign(snapshot);
      dml_param.branch_id_ = 0;
      dml_param.store_ctx_guard_ = &store_ctx_guard;
      dml_param.schema_version_ = task_schema.get_schema_version();
      dml_param.dml_allocator_ = &alloc;
      storage::ObValueRowIterator row_iter;
      if (OB_FAIL(row_iter.init())) {
        LOG_WARN("init task delete iter failed", K(ret));
      }
      for (int64_t i = 0; OB_SUCC(ret) && i < rows.count(); ++i) {
        if (OB_FAIL(row_iter.add_row(*rows.at(i)))) {
          LOG_WARN("add task delete row failed", K(ret), K(i));
        }
      }
      if (OB_SUCC(ret)) {
        ret = oas->get_write_store_ctx_guard(timeout_us, *tx_desc, snapshot, 0,
                                             dml_param.write_flag_, store_ctx_guard);
      }
      if (OB_SUCC(ret)) {
        int64_t affected = 0;
        ret = oas->delete_rows(task_schema.get_tablet_id(), *tx_desc, dml_param,
                               column_ids, &row_iter, affected);
      }
    }
  }
  return ret;
}

int delete_ai_column_tasks_for_delete(const uint64_t table_id,
                                      const ObDASDelCtDef &ctdef,
                                      ObDASWriteBuffer &write_buffer,
                                      transaction::ObTxDesc *tx_desc,
                                      const transaction::ObTxReadSnapshot *snapshot,
                                      common::ObIAllocator &alloc)
{
  int ret = common::OB_SUCCESS;
  share::schema::ObSchemaGetterGuard guard;
  const share::schema::ObTableSchema *table_schema = NULL;
  const share::schema::ObTableSchema *task_schema = NULL;
  common::ObSEArray<uint64_t, 4> all_ai_ids;
  common::ObSEArray<common::ObString, 4> all_ai_names;
  common::ObSEArray<uint64_t, 4> task_pk_ids;
  common::ObSEArray<blocksstable::ObDatumRow *, 32> del_rows;
  if (OB_ISNULL(tx_desc) || OB_ISNULL(snapshot)) {
    ret = common::OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(share::schema::ObMultiVersionSchemaService::get_instance().get_runtime_schema_guard(guard))) {
  } else if (OB_FAIL(guard.get_table_schema(table_id, table_schema))) {
  } else if (OB_ISNULL(table_schema)) {
    ret = common::OB_TABLE_NOT_EXIST;
  } else if (OB_FAIL(ai_column_task::collect_ai_columns(*table_schema, all_ai_ids, all_ai_names, alloc))) {
  }
  if (OB_SUCC(ret) && !all_ai_ids.empty()) {
    if (OB_FAIL(guard.get_table_schema(share::OB_ALL_AI_COLUMN_TASK_TID, task_schema))) {
    } else if (OB_ISNULL(task_schema)) {
      ret = common::OB_TABLE_NOT_EXIST;
    } else if (OB_FAIL(task_schema->get_rowkey_column_ids(task_pk_ids))) {
    }
  }
  if (OB_SUCC(ret) && !all_ai_ids.empty()) {
    ObDASWriteBuffer::NewRowIterator dml_iter;
    blocksstable::ObDatumRow *dml_row = NULL;
    if (OB_FAIL(write_buffer.begin(dml_iter, ctdef.column_types_))) {
      LOG_WARN("begin write buffer iterate failed", K(ret));
    }
    while (OB_SUCC(ret) && OB_SUCC(dml_iter.get_next_row(dml_row))) {
      common::ObSqlString key;
      common::ObSqlString pred;
      if (OB_FAIL(ai_column_task::build_rowkey_by_cols(*table_schema, ctdef.column_ids_, *dml_row, key, pred))) {
        if (common::OB_ITEM_NOT_MATCH == ret) {
          LOG_WARN("skip delete row for ai task: rowkey is null");
        }
        ret = common::OB_SUCCESS;
        continue;
      }
      common::ObString key_text;
      if (OB_FAIL(ob_write_string(alloc, key.string(), key_text))) {
      } else {
        for (int64_t c = 0; OB_SUCC(ret) && c < all_ai_ids.count(); ++c) {
          blocksstable::ObDatumRow *del_row = NULL;
          if (OB_FAIL(data_plane::create_datum_row(alloc, 3, del_row))) {
          } else if (OB_ISNULL(del_row)) {
            ret = common::OB_ERR_UNEXPECTED;
          } else {
            del_row->row_flag_.set_flag(blocksstable::ObDmlFlag::DF_DELETE);
            del_row->storage_datums_[0].set_int(static_cast<int64_t>(table_id));
            del_row->storage_datums_[1].set_int(static_cast<int64_t>(all_ai_ids.at(c)));
            del_row->storage_datums_[2].set_string(key_text);
            if (OB_FAIL(del_rows.push_back(del_row))) {
            }
          }
        }
      }
    }
    if (common::OB_ITER_END == ret) {
      ret = common::OB_SUCCESS;
    }
    if (OB_SUCC(ret) && !del_rows.empty()) {
      if (OB_FAIL(delete_task_rows(alloc, tx_desc, *snapshot, *task_schema, task_pk_ids, del_rows))) {
        LOG_WARN("delete ai column task rows failed", K(ret), K(del_rows.count()));
      }
    }
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
