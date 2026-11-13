/*
 * Copyright (c) 2025 OceanBase.
 */
#ifndef OCEANBASE_SQL_RESOLVER_DDL_OB_AI_COLUMN_DEF_H_
#define OCEANBASE_SQL_RESOLVER_DDL_OB_AI_COLUMN_DEF_H_
#include "lib/ob_define.h"
#include "lib/string/ob_string.h"
namespace oceanbase
{
namespace sql
{
struct ObAiColumnDef
{
  common::ObString column_name_;
  common::ObString ai_function_;
  common::ObString model_name_;
  common::ObString normalized_expr_;
  int64_t max_retries_;
  ObAiColumnDef() : max_retries_(0) {}
  void reset()
  {
    column_name_.reset();
    ai_function_.reset();
    model_name_.reset();
    normalized_expr_.reset();
    max_retries_ = 0;
  }
  TO_STRING_KV(K_(column_name), K_(ai_function), K_(model_name), K_(normalized_expr), K_(max_retries));
};
} // namespace sql
} // namespace oceanbase
#endif
