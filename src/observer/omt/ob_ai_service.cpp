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

#define USING_LOG_PREFIX SERVER_OMT
#include "ob_ai_service.h"
#include "observer/ai_service/ob_ai_service_executor.h"
#include "sql/privilege_check/ob_ai_model_priv_util.h"
#include "sql/session/ob_sql_session_info.h"
#include "sql/engine/ob_exec_context.h"
#include "common/mysqlclient/ob_mysql_transaction.h"
#include "lib/string/ob_sql_string.h"
#include "lib/allocator/page_arena.h"
#include "lib/time/ob_time_utility.h"
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"
#include "sql/das/ob_das_ai_column_task.h"
#include "lib/profile/ob_trace_id.h"

using namespace oceanbase::share;
using namespace oceanbase::common;
namespace oceanbase
{
namespace omt
{

ObAiService::ObAiService()
  : is_inited_(false)
{
}

ObAiService::~ObAiService()
{
  destroy();
}

int ObAiService::server_module_init(ObAiService* &ai_service)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(ai_service)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(ai_service->init())) {
  }
  return ret;
}

int ObAiService::init()
{
  int ret = OB_SUCCESS;
  if (IS_INIT) {
    ret = OB_INIT_TWICE;
  } else if (OB_FAIL(ai_column_worker_.init())) {
    LOG_WARN("init ai column worker failed", K(ret));
  } else {
    is_inited_ = true;
  }
  return ret;
}

int ObAiService::start()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ai_column_worker_.start())) {
    LOG_WARN("start ai column worker failed", K(ret));
  } else {
    LOG_INFO("ai column worker started");
  }
  return ret;
}

void ObAiService::stop()
{
  ai_column_worker_.stop_worker();
}

void ObAiService::wait()
{
}

void ObAiService::destroy()
{
  ai_column_worker_.stop_worker();
  is_inited_ = false;
}

int ObAiService::get_ai_service_guard(ObAiServiceGuard &ai_service_guard)
{
  // TODO: implement after cache support
  int ret = OB_SUCCESS;
  return ret;
}

ObAiServiceGuard::ObAiServiceGuard()
  : local_allocator_(ObMemAttr("AiServiceGuard", ObCtxIds::DEFAULT_CTX_ID))
{
}

ObAiServiceGuard::~ObAiServiceGuard()
{
}

int ObAiServiceGuard::check_access_privilege()
{
  int ret = OB_SUCCESS;
  bool has_priv = false;

  sql::ObSQLSessionInfo *session = nullptr;
  if (OB_ISNULL(session = THIS_WORKER.get_session())) {
    // system background task thread do not have session, skip check
  } else {
    ObArenaAllocator tmp_allocator;
    share::schema::ObSchemaGetterGuard *schema_guard = session->get_cur_exec_ctx()->get_sql_ctx()->schema_guard_;
    if (OB_ISNULL(schema_guard)) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      sql::ObAIServiceEndpointPrivUtil priv_util(*schema_guard);
      share::schema::ObSessionPrivInfo session_priv;
      uint64_t user_id = session->get_priv_user_id();
      if (user_id == OB_INVALID_ID) {
        user_id = OB_SYS_USER_ID;
      }
      if (OB_FAIL(schema_guard->get_session_priv_info(
                                                    user_id,
                                                    session->get_database_name(),
                                                    session_priv))) {
      } else if (OB_FAIL(priv_util.check_access_ai_model_priv(tmp_allocator, session_priv, has_priv))) {
      } else if (!has_priv) {
        ret = OB_ERR_NO_PRIVILEGE;
        LOG_WARN("no privilege for ai model access", K(ret));
        LOG_USER_ERROR(OB_ERR_NO_PRIVILEGE, "access ai model");
      }
    }
  }

  return ret;
}

int ObAiServiceGuard::get_ai_endpoint(const common::ObString &name, const share::ObAiModelEndpointInfo *&endpoint_info)
{
  int ret = OB_SUCCESS;
  ObAiModelEndpointInfo *tmp_endpoint_info = nullptr;

  if (OB_FAIL(check_access_privilege())) {
  } else if (OB_ISNULL(tmp_endpoint_info = OB_NEWx(ObAiModelEndpointInfo, &local_allocator_))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_FAIL(ObAiServiceExecutor::read_ai_endpoint(local_allocator_, name, *tmp_endpoint_info))) {
  } else {
    endpoint_info = tmp_endpoint_info;
  }
  return ret;
}

int ObAiServiceGuard::get_ai_endpoint_by_ai_model_name(const common::ObString &ai_model_name, const share::ObAiModelEndpointInfo *&endpoint_info, bool need_check)
{
  int ret = OB_SUCCESS;
  ObAiModelEndpointInfo *tmp_endpoint_info = nullptr;

  if (need_check && OB_FAIL(check_access_privilege())) {
    LOG_WARN("failed to check access privilege", K(ret));
  } else if (OB_ISNULL(tmp_endpoint_info = OB_NEWx(ObAiModelEndpointInfo, &local_allocator_))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_FAIL(ObAiServiceExecutor::read_ai_endpoint_by_ai_model_name(local_allocator_, ai_model_name, *tmp_endpoint_info))) {
  } else {
    endpoint_info = tmp_endpoint_info;
  }
  return ret;
}


// ---------------------------------------------------------------------------
// AI column async task worker: polls __all_ai_column_task, calls the model and
// writes results back in one inner transaction.
// ---------------------------------------------------------------------------
namespace ai_column_worker
{
static const int64_t TASK_BATCH = 32;
static const int64_t IDLE_US = 100 * 1000;

struct TaskRow
{
  uint64_t table_id_;
  uint64_t column_id_;
  int64_t retry_count_;
  int64_t max_retries_;
  common::ObString request_id_;
  common::ObString rowkey_text_;
  common::ObString pred_;
  common::ObString ai_function_;
  common::ObString model_name_;
  common::ObString norm_expr_;
  common::ObString db_name_;
  common::ObString table_name_;
  common::ObString col_name_;
  TaskRow() : table_id_(0), column_id_(0), retry_count_(0), max_retries_(0) {}
};

static int escape_literal(common::ObSqlString &out, const common::ObString &in)
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

static bool contains_str(const common::ObString &hay, const char *needle)
{
  bool found = false;
  const int64_t nlen = static_cast<int64_t>(STRLEN(needle));
  if (nlen <= hay.length()) {
    for (int64_t i = 0; !found && i + nlen <= hay.length(); ++i) {
      found = (0 == MEMCMP(hay.ptr() + i, needle, nlen));
    }
  }
  return found;
}

// normalized_expr looks like: AI_COMPLETE('model', concat('Classify: ', content)) <hint>
static int extract_prompt_expr(const common::ObString &norm, common::ObString &expr)
{
  int ret = common::OB_SUCCESS;
  const char *p = norm.ptr();
  int64_t len = norm.length();
  const char *hint = " Return exactly one valid JSON value";
  const int64_t hint_len = static_cast<int64_t>(STRLEN(hint));
  if (len > hint_len && 0 == MEMCMP(p + len - hint_len, hint, hint_len)) {
    len -= hint_len;
  }
  int64_t l = -1;
  int64_t r = -1;
  for (int64_t i = 0; -1 == l && i < len; ++i) {
    if ('(' == p[i]) {
      l = i;
    }
  }
  for (int64_t i = len - 1; -1 == r && i >= 0; --i) {
    if (')' == p[i]) {
      r = i;
    }
  }
  if (l < 0 || r <= l) {
    ret = common::OB_INVALID_ARGUMENT;
  } else {
    int64_t depth = 0;
    bool in_quote = false;
    int64_t start = l + 1;
    int64_t argIdx = 0;
    while (start < r && ' ' == p[start]) {
      ++start;
    }
    for (int64_t i = start; i <= r; ++i) {
      const char c = (i == r) ? ',' : p[i];
      if ('\'' == c) {
        in_quote = !in_quote;
      } else if (!in_quote && '(' == c) {
        ++depth;
      } else if (!in_quote && ')' == c) {
        --depth;
      } else if (!in_quote && 0 == depth && ',' == c) {
        if (1 == argIdx) {
          expr.assign_ptr(p + start, static_cast<int32_t>(i - start));
        }
        ++argIdx;
        start = i + 1;
        while (start < r && ' ' == p[start]) {
          ++start;
        }
      }
    }
    if (expr.empty()) {
      ret = common::OB_INVALID_ARGUMENT;
    }
  }
  return ret;
}

static int read_one_string(common::ObArenaAllocator &alloc, const common::ObSqlString &sql,
                           common::ObString &out, bool &found)
{
  int ret = common::OB_SUCCESS;
  found = false;
  SMART_VAR(ObMySQLProxy::MySQLResult, res) {
    common::sqlclient::ObMySQLResult *result = NULL;
    if (OB_ISNULL(GCTX.sql_proxy_)) {
      ret = common::OB_NOT_INIT;
    } else if (OB_FAIL(GCTX.sql_proxy_->read(res, sql.ptr()))) {
    } else if (OB_ISNULL(result = res.get_result())) {
      ret = common::OB_ERR_UNEXPECTED;
    } else {
      int tmp = result->next();
      common::ObString val;
      if (common::OB_ITER_END == tmp) {
      } else if (OB_FAIL(tmp)) {
      } else if (OB_FAIL(result->get_varchar(static_cast<int64_t>(0), val))) {
      } else if (OB_FAIL(common::ob_write_string(alloc, val, out))) {
      } else {
        found = true;
      }
    }
  }
  return ret;
}

static int exec_write(const common::ObString &sql, int64_t &affected_rows)
{
  int ret = common::OB_SUCCESS;
  if (OB_ISNULL(GCTX.sql_proxy_)) {
    ret = common::OB_NOT_INIT;
  } else if (OB_FAIL(GCTX.sql_proxy_->write(sql.ptr(), affected_rows))) {
  }
  return ret;
}

static int call_model(common::ObIAllocator &alloc,
                      const common::ObString &model_name,
                      const common::ObString &system_text,
                      const common::ObString &user_text,
                      const bool use_embedding,
                      common::ObString &out)
{
  int ret = common::OB_SUCCESS;
  omt::ObAiServiceGuard guard;
  const share::ObAiModelEndpointInfo *endpoint = NULL;
  ObAIFuncExprInfo *info = NULL;
  common::ObString model_name_mut = model_name;
  if (OB_FAIL(guard.get_ai_endpoint_by_ai_model_name(model_name_mut, endpoint, false))) {
  } else if (OB_ISNULL(endpoint)) {
    ret = common::OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(common::ObAIFuncUtils::get_ai_func_info(alloc, model_name_mut, info))) {
  } else if (OB_ISNULL(info)) {
    ret = common::OB_ERR_UNEXPECTED;
  } else {
    common::ObAIFuncModel model(alloc, *info, *endpoint);
    common::ObString sys = system_text;
    common::ObString user = user_text;
    if (use_embedding) {
      ret = model.call_dense_embedding(user, NULL, out);
    } else {
      ret = model.call_completion_with_system(sys, user, NULL, out);
    }
  }
  return ret;
}

static bool value_is_integer(const common::ObString &v)
{
  bool ok = !v.empty();
  for (int64_t i = 0; ok && i < v.length(); ++i) {
    const char c = v.ptr()[i];
    ok = ('0' <= c && c <= '9') || ('-' == c && 0 == i);
  }
  return ok;
}

static int build_task_key_cond(common::ObSqlString &sql, const TaskRow &t)
{
  int ret = common::OB_SUCCESS;
  common::ObSqlString esc;
  if (OB_FAIL(escape_literal(esc, t.rowkey_text_))) {
  } else if (OB_FAIL(sql.append_fmt("table_id = %lu and column_id = %lu and rowkey_text = '%.*s'",
                                    t.table_id_, t.column_id_,
                                    static_cast<int>(esc.length()), esc.ptr()))) {
  }
  return ret;
}


static int get_ai_column_obj_type(const uint64_t table_id,
                                 const uint64_t column_id,
                                 common::ObObjType &type)
{
  int ret = common::OB_SUCCESS;
  share::schema::ObSchemaGetterGuard guard;
  const share::schema::ObTableSchema *table_schema = NULL;
  const share::schema::ObColumnSchemaV2 *col = NULL;
  if (OB_FAIL(share::schema::ObMultiVersionSchemaService::get_instance().get_runtime_schema_guard(guard))) {
  } else if (OB_FAIL(guard.get_table_schema(table_id, table_schema))) {
  } else if (OB_ISNULL(table_schema)) {
    ret = common::OB_TABLE_NOT_EXIST;
  } else if (OB_ISNULL(col = table_schema->get_column_schema(column_id))) {
    ret = common::OB_ERR_UNEXPECTED;
  } else {
    type = col->get_data_type();
  }
  return ret;
}

static int handle_group(common::ObArenaAllocator &alloc,
                        const common::ObSEArray<TaskRow, 32> &tasks,
                        const int64_t begin,
                        const int64_t end)
{
  int ret = common::OB_SUCCESS;
  const int64_t n = end - begin;
  common::ObSEArray<common::ObString, 8> results;
  common::ObSEArray<int8_t, 8> ok_flags;
  common::ObSEArray<int8_t, 8> embed_flags;
  for (int64_t i = begin; OB_SUCC(ret) && i < end; ++i) {
    const TaskRow &t = tasks.at(i);
    common::ObString expr;
    common::ObString prompt;
    common::ObString out;
    bool found = false;
    bool known = false;
    const bool is_embed = contains_str(t.ai_function_, "AI_EMBED");
    const bool is_json = contains_str(t.norm_expr_, "valid JSON value");
    common::ObSqlString psql;
    int tmp_ret = common::OB_SUCCESS;
    if (OB_FAIL(extract_prompt_expr(t.norm_expr_, expr))) {
      LOG_WARN("extract prompt expr failed", K(t.norm_expr_));
    } else if (OB_FAIL(psql.append_fmt("select %.*s from `%.*s`.`%.*s` where %.*s limit 1",
                                       static_cast<int>(expr.length()), expr.ptr(),
                                       static_cast<int>(t.db_name_.length()), t.db_name_.ptr(),
                                       static_cast<int>(t.table_name_.length()), t.table_name_.ptr(),
                                       static_cast<int>(t.pred_.length()), t.pred_.ptr()))) {
    } else if (OB_FAIL(read_one_string(alloc, psql, prompt, found))) {
    } else if (!found) {
      LOG_WARN("ai column row gone, drop task", K(t.table_id_), K(t.column_id_));
      known = true;
    } else {
      common::ObObjType col_type = common::ObMaxType;
      int type_ret = get_ai_column_obj_type(t.table_id_, t.column_id_, col_type);
      if (common::OB_SUCCESS != type_ret) {
        col_type = common::ObMaxType;
      }
      const bool is_int_col = (common::ObIntType == col_type || common::ObUInt64Type == col_type
                               || common::ObNumberType == col_type || common::ObDecimalIntType == col_type);
      const char *sys_text = is_embed ? "" : (is_json ? "Return exactly one valid JSON value"
                                                      : (is_int_col ? "Return the answer as a base-10 integer only"
                                                                    : "Answer the request directly"));
      tmp_ret = call_model(alloc, t.model_name_, common::ObString(sys_text), prompt, is_embed, out);
      if (OB_SUCCESS == tmp_ret) {
        known = true;
      } else {
        LOG_WARN("ai column model call failed", K(tmp_ret), K(t.model_name_));
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(results.push_back(out))) {
    } else if (OB_FAIL(ok_flags.push_back(known ? 1 : 0))) {
    } else if (OB_FAIL(embed_flags.push_back(is_embed ? 1 : 0))) {
    }
  }
  if (OB_FAIL(ret)) {
    LOG_WARN("handle ai column group failed", K(ret), K(begin), K(end));
    ret = common::OB_SUCCESS;
  } else {
    int64_t affected = 0;
    common::ObMySQLTransaction trans;
    if (OB_FAIL(trans.start(GCTX.sql_proxy_))) {
      LOG_WARN("start inner transaction failed", K(ret));
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < n; ++i) {
        const TaskRow &t = tasks.at(begin + i);
        if (0 == ok_flags.at(i)) {
          continue;
        }
        common::ObSqlString qsql;
        common::ObString cur_req;
        bool found = false;
        if (OB_FAIL(qsql.append("select request_id from oceanbase.__all_ai_column_task where ")) ||
            OB_FAIL(build_task_key_cond(qsql, t)) ||
            OB_FAIL(qsql.append(" for update"))) {
        } else if (OB_FAIL(read_one_string(alloc, qsql, cur_req, found))) {
        } else if (!found || 0 != cur_req.compare(t.request_id_)) {
          LOG_INFO("skip stale ai column result", K(cur_req), K(t.request_id_));
          continue;
        } else {
          common::ObSqlString usql;
          common::ObSqlString lit;
          const common::ObString &val = results.at(i);
          if (embed_flags.at(i) || !value_is_integer(val)) {
            if (OB_FAIL(lit.append("'"))) {
            } else if (OB_FAIL(escape_literal(lit, val))) {
            } else if (OB_FAIL(lit.append("'"))) {
            }
          } else {
            if (OB_FAIL(lit.append(val))) {
            }
          }
          if (OB_FAIL(ret)) {
          } else if (OB_FALSE_IT(sql::g_ai_worker_write = true)) {
          } else if (OB_FAIL(usql.append_fmt("update `%.*s`.`%.*s` set `%.*s` = %.*s where %.*s",
                                             static_cast<int>(t.db_name_.length()), t.db_name_.ptr(),
                                             static_cast<int>(t.table_name_.length()), t.table_name_.ptr(),
                                             static_cast<int>(t.col_name_.length()), t.col_name_.ptr(),
                                             static_cast<int>(lit.length()), lit.ptr(),
                                             static_cast<int>(t.pred_.length()), t.pred_.ptr()))) {
          } else if (OB_FAIL(trans.write(usql.ptr(), affected))) {
            LOG_WARN("write ai column result failed", K(ret), K(usql));
          } else {
            common::ObSqlString dsql;
            if (OB_FAIL(dsql.append("delete from oceanbase.__all_ai_column_task where ")) ||
                OB_FAIL(build_task_key_cond(dsql, t)) ||
                OB_FAIL(dsql.append(" and request_id = '")) ||
                OB_FAIL(escape_literal(dsql, t.request_id_)) ||
                OB_FAIL(dsql.append("'"))) {
            } else if (OB_FAIL(trans.write(dsql.ptr(), affected))) {
              LOG_WARN("delete ai column task failed", K(ret));
            }
          }
        }
      }
      const int end_ret = trans.end(OB_SUCC(ret));
      if (OB_SUCCESS != end_ret) {
        LOG_WARN("end inner transaction failed", K(end_ret));
        if (OB_SUCC(ret)) {
          ret = end_ret;
        }
      }
    }
    for (int64_t i = 0; i < n; ++i) {
      const TaskRow &t = tasks.at(begin + i);
      int tmp_ret = common::OB_SUCCESS;
      common::ObSqlString fsql;
      if (ok_flags.at(i)) {
        continue;
      }
      if (t.retry_count_ < t.max_retries_) {
        if (OB_SUCCESS != (tmp_ret = fsql.append("update oceanbase.__all_ai_column_task set state = 3, "
                                                 "retry_count = retry_count + 1, gmt_modified = now(6), "
                                                 "eligible_ts = date_add(now(6), interval 1 second) where "))) {
        }
      } else {
        if (OB_SUCCESS != (tmp_ret = fsql.append("delete from oceanbase.__all_ai_column_task where "))) {
        }
      }
      if (OB_SUCCESS == tmp_ret) {
        tmp_ret = build_task_key_cond(fsql, t);
      }
      if (OB_SUCCESS == tmp_ret && t.retry_count_ < t.max_retries_) {
        tmp_ret = fsql.append(" and request_id = '");
        if (OB_SUCCESS == tmp_ret) {
          tmp_ret = escape_literal(fsql, t.request_id_);
        }
        if (OB_SUCCESS == tmp_ret) {
          tmp_ret = fsql.append("'");
        }
      }
      if (OB_SUCCESS == tmp_ret) {
        int64_t affected = 0;
        if (OB_SUCCESS != (tmp_ret = exec_write(fsql.string(), affected))) {
          LOG_WARN("update ai column task state failed", K(tmp_ret));
        }
      }
    }
  }
  return ret;
}
} // namespace ai_column_worker

int ObAiColumnTaskWorker::init()
{
  int ret = common::OB_SUCCESS;
  if (is_inited_) {
    ret = common::OB_INIT_TWICE;
  } else if (OB_FAIL(rootserver::ObServerThreadHelper::create("AiColWkr", 1))) {
    LOG_WARN("create ai column worker thread failed", K(ret));
  } else {
    is_inited_ = true;
  }
  return ret;
}

int ObAiColumnTaskWorker::start()
{
  return rootserver::ObServerThreadHelper::start();
}

void ObAiColumnTaskWorker::stop_worker()
{
  stop_flag_ = true;
  wakeup();
  stop();
  wait();
}

void ObAiColumnTaskWorker::do_work()
{
  int ret = common::OB_SUCCESS;
  ObCurTraceId::init(GCTX.self_addr());
  while (!stop_flag_) {
    ret = do_one_round();
    if (common::OB_SUCCESS != ret && common::OB_EAGAIN != ret) {
      LOG_WARN("ai column worker round failed", K(ret));
    }
    rootserver::ObServerThreadHelper::idle(ai_column_worker::IDLE_US);
  }
}

int ObAiColumnTaskWorker::do_one_round()
{
  int ret = common::OB_SUCCESS;
  common::ObArenaAllocator alloc;
  common::ObSEArray<ai_column_worker::TaskRow, 32> tasks;
  common::ObSqlString sql;
  if (OB_ISNULL(GCTX.sql_proxy_)) {
    ret = common::OB_NOT_INIT;
  } else if (OB_FAIL(sql.append("select t.table_id, t.column_id, t.request_id, t.rowkey_text, "
                                "t.rowkey_predicate, t.retry_count, a.ai_function, a.ai_model_name, "
                                "a.max_retries, a.normalized_expr, d.database_name, tb.table_name, "
                                "c.column_name from oceanbase.__all_ai_column_task t "
                                "join oceanbase.__all_ai_column a on a.table_id = t.table_id "
                                "and a.column_id = t.column_id "
                                "join oceanbase.__all_table tb on tb.table_id = t.table_id "
                                "join oceanbase.__all_database d on d.database_id = tb.database_id "
                                "join oceanbase.__all_column c on c.table_id = a.table_id "
                                "and c.column_id = a.column_id "
                                "where t.eligible_ts <= now(6) order by t.request_id limit 32"))) {
  } else {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      common::sqlclient::ObMySQLResult *result = NULL;
      if (OB_FAIL(GCTX.sql_proxy_->read(res, sql.ptr()))) {
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = common::OB_ERR_UNEXPECTED;
      } else {
        common::ObString tmp;
        while (OB_SUCC(result->next())) {
          ai_column_worker::TaskRow row;
          int64_t tmp_int = 0;
          if (OB_FAIL(result->get_int("table_id", tmp_int))) {
          } else if (OB_FALSE_IT(row.table_id_ = static_cast<uint64_t>(tmp_int))) {
          } else if (OB_FAIL(result->get_int("column_id", tmp_int))) {
          } else if (OB_FALSE_IT(row.column_id_ = static_cast<uint64_t>(tmp_int))) {
          } else if (OB_FAIL(result->get_int("retry_count", row.retry_count_))) {
          } else if (OB_FAIL(result->get_int("max_retries", row.max_retries_))) {
          } else {
            const char *cols[] = {"request_id", "rowkey_text", "rowkey_predicate", "ai_function",
                                  "ai_model_name", "normalized_expr", "database_name", "table_name",
                                  "column_name"};
            common::ObString *dst[] = {&row.request_id_, &row.rowkey_text_, &row.pred_,
                                       &row.ai_function_, &row.model_name_, &row.norm_expr_,
                                       &row.db_name_, &row.table_name_, &row.col_name_};
            for (int64_t ci = 0; OB_SUCC(ret) && ci < 9; ++ci) {
              if (OB_FAIL(result->get_varchar(cols[ci], tmp))) {
              } else if (OB_FAIL(common::ob_write_string(alloc, tmp, *dst[ci]))) {
              }
            }
          }
          if (OB_FAIL(ret)) {
            break;
          } else if (OB_FAIL(tasks.push_back(row))) {
          }
        }
        if (common::OB_ITER_END == ret) {
          ret = common::OB_SUCCESS;
        }
      }
    }
  }
  int64_t i = 0;
  while (OB_SUCC(ret) && i < tasks.count()) {
    int64_t j = i;
    while (j < tasks.count() && 0 == tasks.at(j).request_id_.compare(tasks.at(i).request_id_)) {
      ++j;
    }
    if (OB_FAIL(ai_column_worker::handle_group(alloc, tasks, i, j))) {
      LOG_WARN("handle ai column task group failed", K(ret));
      ret = common::OB_SUCCESS;
    }
    i = j;
  }
  return ret;
}

} // namespace omt
} // namespace oceanbase
