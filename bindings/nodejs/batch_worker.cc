#include "batch_worker.h"

BatchWorker::BatchWorker(Napi::Env env,
                         Napi::Object databaseObj,
                         database_t* db,
                         std::vector<BatchOp> ops,
                         Napi::Function callback)
  : WaveDBAsyncWorker(env, callback),
    databaseRef_(Napi::Persistent(databaseObj)),
    db_(db),
    ops_(std::move(ops)) {
  // Increment pending operations counter
  if (db_) {
    database_pending_op_start(db_);
  }
}

BatchWorker::~BatchWorker() {
  // Decrement pending operations counter
  if (db_) {
    database_pending_op_finish(db_);
  }

  for (auto& op : ops_) {
    if (op.path) {
      path_destroy(op.path);
      op.path = nullptr;
    }
    if (op.value) {
      identifier_destroy(op.value);
      op.value = nullptr;
    }
  }
}

void BatchWorker::Execute() {
  if (!db_) {
    SetError("DATABASE_CLOSED: Database is closed");
    return;
  }

  for (auto& op : ops_) {
    int rc;
    if (op.type == BatchOpType::PUT) {
      rc = database_put_sync(db_, op.path, op.value);
      // database_put_sync takes ownership of path and value
      op.path = nullptr;
      op.value = nullptr;
    } else {
      rc = database_delete_sync(db_, op.path);
      // database_delete_sync takes ownership of path
      op.path = nullptr;
    }

    if (rc != 0) {
      SetError("IO_ERROR: Batch operation failed");
      return;
    }
  }
}

std::vector<napi_value> BatchWorker::GetResult(Napi::Env env) {
  return { env.Undefined() };
}
