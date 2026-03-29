#include "del_worker.h"

DelWorker::DelWorker(Napi::Env env,
                     Napi::Object databaseObj,
                     Napi::Function callback,
                     database_t* db,
                     path_t* path)
  : WaveDBAsyncWorker(env, callback),
    databaseRef_(Napi::Persistent(databaseObj)),
    db_(db),
    path_(path) {
  // Keep database alive while operation is pending
  if (db_) {
    REFERENCE(db_, database_t);
  }
}

DelWorker::~DelWorker() {
  // Release database reference
  if (db_) {
    DEREFERENCE(db_);
  }

  if (path_) {
    path_destroy(path_);
    path_ = nullptr;
  }
}

void DelWorker::Execute() {
  if (!db_) {
    SetError("DATABASE_CLOSED: Database is closed");
    return;
  }

  if (!path_) {
    SetError("INVALID_PATH: Path is null");
    return;
  }

  int result = database_delete_sync(db_, path_);

  // database_delete_sync takes ownership of path
  path_ = nullptr;

  if (result != 0) {
    SetError("IO_ERROR: Delete operation failed");
  }
}

std::vector<napi_value> DelWorker::GetResult(Napi::Env env) {
  return { env.Undefined() };
}
