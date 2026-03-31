#include "put_worker.h"

PutWorker::PutWorker(Napi::Env env,
                     Napi::Object databaseObj,
                     Napi::Function callback,
                     database_t* db,
                     path_t* path,
                     identifier_t* value)
  : WaveDBAsyncWorker(env, callback),
    databaseRef_(Napi::Persistent(databaseObj)),
    db_(db),
    path_(path),
    value_(value) {
  // Keep database alive while operation is pending
  if (db_) {
    REFERENCE(db_, database_t);
  }
}

PutWorker::~PutWorker() {
  // Release database reference
  if (db_) {
    DEREFERENCE(db_);
  }

  if (path_) {
    path_destroy(path_);
    path_ = nullptr;
  }
  if (value_) {
    identifier_destroy(value_);
    value_ = nullptr;
  }
}

void PutWorker::Execute() {
  if (!db_) {
    SetError("DATABASE_CLOSED: Database is closed");
    return;
  }

  if (!path_) {
    SetError("INVALID_PATH: Path is null");
    return;
  }

  if (!value_) {
    SetError("INVALID_VALUE: Value is null");
    return;
  }

  int result = database_put_sync(db_, path_, value_);

  // database_put_sync takes ownership of path and value
  path_ = nullptr;
  value_ = nullptr;

  if (result != 0) {
    SetError("IO_ERROR: Put operation failed");
    return;
  }

  // NOTE: Do NOT flush WAL here - flushing happens during database close
  // after all workers have completed
}

std::vector<napi_value> PutWorker::GetResult(Napi::Env env) {
  return { env.Undefined() };
}
