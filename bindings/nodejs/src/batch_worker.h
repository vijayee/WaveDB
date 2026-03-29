#ifndef WAVEDB_BINDINGS_BATCH_WORKER_H
#define WAVEDB_BINDINGS_BATCH_WORKER_H

#include <napi.h>
#include <vector>
#include "../../../src/Database/database.h"
#include "async_worker.h"
#include "path.h"
#include "identifier.h"

enum class BatchOpType { PUT, DEL };

struct BatchOp {
  BatchOpType type;
  path_t* path;
  identifier_t* value;  // nullptr for DEL
};

class BatchWorker : public WaveDBAsyncWorker {
public:
  BatchWorker(Napi::Env env,
              Napi::Object databaseObj,
              database_t* db,
              std::vector<BatchOp> ops,
              Napi::Function callback);

  virtual ~BatchWorker();

  void Execute() override;

protected:
  std::vector<napi_value> GetResult(Napi::Env env) override;

private:
  Napi::ObjectReference databaseRef_;
  database_t* db_;
  std::vector<BatchOp> ops_;
};

#endif // WAVEDB_BINDINGS_BATCH_WORKER_H
