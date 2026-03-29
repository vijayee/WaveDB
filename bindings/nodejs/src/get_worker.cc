#include "get_worker.h"

GetWorker::GetWorker(Napi::Env env,
                     Napi::Function callback,
                     database_t* db,
                     path_t* path)
  : WaveDBAsyncWorker(env, callback),
    db_(db),
    path_(path),
    result_(nullptr) {}

GetWorker::~GetWorker() {
  if (path_) {
    path_destroy(path_);
    path_ = nullptr;
  }
  if (result_) {
    identifier_destroy(result_);
    result_ = nullptr;
  }
}

void GetWorker::Execute() {
  if (!db_) {
    SetError("DATABASE_CLOSED: Database is closed");
    return;
  }

  if (!path_) {
    SetError("INVALID_PATH: Path is null");
    return;
  }

  int ret = database_get_sync(db_, path_, &result_);

  // database_get_sync takes ownership of path
  path_ = nullptr;

  if (ret == -1) {
    // Error occurred
    SetError("IO_ERROR: Get operation failed");
    return;
  }

  // ret == 0: found (result_ is set)
  // ret == -2: not found (result_ is null, which is valid)
  // Either case is success - caller gets result or null
}

std::vector<napi_value> GetWorker::GetResult(Napi::Env env) {
  return { ValueToJS(env, result_) };
}