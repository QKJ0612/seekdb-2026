#ifndef OCEANBASE_SQL_DAS_OB_DAS_AI_COLUMN_TASK_H_
#define OCEANBASE_SQL_DAS_OB_DAS_AI_COLUMN_TASK_H_
#include "lib/ob_define.h"
#include "sql/das/ob_das_dml_ctx_define.h"
namespace oceanbase
{
namespace transaction
{
class ObTxDesc;
class ObTxReadSnapshot;
}
namespace sql
{

// set while the ai column worker writes a result back, so the update hook
// does not enqueue a fresh task for its own write.
extern thread_local bool g_ai_worker_write;
// create pending ai column tasks for rows written by an insert statement,
// inside the same transaction as the user statement.
int create_ai_column_tasks_for_insert(const uint64_t table_id,
                                      const ObDASInsCtDef &ctdef,
                                      ObDASWriteBuffer &write_buffer,
                                      transaction::ObTxDesc *tx_desc,
                                      const transaction::ObTxReadSnapshot *snapshot,
                                      common::ObIAllocator &alloc);
int delete_ai_column_tasks_for_delete(const uint64_t table_id,
                                      const ObDASDelCtDef &ctdef,
                                      ObDASWriteBuffer &write_buffer,
                                      transaction::ObTxDesc *tx_desc,
                                      const transaction::ObTxReadSnapshot *snapshot,
                                      common::ObIAllocator &alloc);

int create_ai_column_tasks_for_update(const uint64_t table_id,
                                      const ObDASUpdCtDef &ctdef,
                                      ObDASWriteBuffer &write_buffer,
                                      transaction::ObTxDesc *tx_desc,
                                      const transaction::ObTxReadSnapshot *snapshot,
                                      common::ObIAllocator &alloc);
} // namespace sql
} // namespace oceanbase
#endif
