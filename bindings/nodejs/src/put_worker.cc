#include "put_worker.h"

PutWorker::PutWorker(Napi::Env env,
                     Napi::Function callback,
                     database_t* db,
                     path_t* path,
                     identifier_t* value)
  : WaveDBAsyncWorker(env, callback),
    db_(db),
    path_(path),
    value_(value) {}

PutWorker::~PutWorker() {
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
  }
}

std::vector<napi_value> PutWorker::GetResult(Napi::Env env) {
  return { env.Undefined() };
}