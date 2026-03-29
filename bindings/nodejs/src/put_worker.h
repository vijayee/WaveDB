#ifndef WAVEDB_BINDINGS_PUT_WORKER_H
#define WAVEDB_BINDINGS_PUT_WORKER_H

#include <napi.h>
#include "../../../src/Database/database.h"
#include "async_worker.h"
#include "path.h"
#include "identifier.h"

class PutWorker : public WaveDBAsyncWorker {
public:
  PutWorker(Napi::Env env,
            Napi::Object databaseObj,
            Napi::Function callback,
            database_t* db,
            path_t* path,
            identifier_t* value);

  virtual ~PutWorker();

  void Execute() override;

protected:
  std::vector<napi_value> GetResult(Napi::Env env) override;

private:
  Napi::ObjectReference databaseRef_;
  database_t* db_;
  path_t* path_;
  identifier_t* value_;
};

#endif // WAVEDB_BINDINGS_PUT_WORKER_H