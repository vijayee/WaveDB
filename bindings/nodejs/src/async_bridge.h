#ifndef WAVEDB_BINDINGS_ASYNC_BRIDGE_H
#define WAVEDB_BINDINGS_ASYNC_BRIDGE_H

#include <napi.h>
#include <atomic>
#include "../../../src/Workers/promise.h"
#include "../../../src/Workers/error.h"
#include "../../../src/Database/database.h"
#include "../../../src/Database/batch.h"
#include "../../../src/Layers/graphql/graphql.h"

// Type of async operation
enum class AsyncOpType {
  Put,
  Get,
  Delete,
  Batch,
  Query,
  Mutate
};

// Per-operation context: bridges C promise callbacks to JS Promise
struct AsyncOpContext {
  napi_deferred deferred;                  // JS Promise deferred (from napi_create_promise)
  napi_value promise;                      // JS Promise value (from napi_create_promise)
  napi_env env;                           // Stored for TSFN callback
  napi_threadsafe_function tsfn;          // Raw TSFN handle for C callbacks
  std::atomic<int>* pending_count;        // Pointer to bridge's pending counter
  AsyncOpType type;                       // Operation type (determines result conversion)
  void* result;                           // C result payload (set by resolve callback)
  async_error_t* error;                   // C error (set by reject callback)
  promise_t* promise_c;                   // C promise — must be destroyed after resolution
  batch_t* batch;                         // C batch — must be destroyed after batch completion
  napi_ref callback_ref;                  // Optional error-first callback (may be nullptr)
};

// AsyncBridge bridges C promise_t callbacks to Node.js via Napi::ThreadSafeFunction.
// Each WaveDB/GraphQLLayer instance owns an AsyncBridge for all its async operations.
class AsyncBridge {
public:
  AsyncBridge();
  ~AsyncBridge();

  // Initialize the TSFN. Must be called once per instance, during construction.
  void Init(Napi::Env env);

  // Release the TSFN and wait for pending operations to drain.
  // Must be called before destroying the underlying C object.
  void Shutdown();

  // Create a C promise_t whose resolve/reject callbacks will bridge to the JS Promise.
  // The caller sets ctx->deferred, ctx->env, and ctx->type before calling this.
  // ctx->callback_ref should be set if an error-first callback is provided.
  // Returns the promise_t* (caller passes it to the C async function).
  // Returns nullptr if TSFN is not initialized.
  promise_t* CreatePromise(AsyncOpContext* ctx);

  // Check if the bridge is initialized
  bool IsInitialized() const { return initialized_; }

private:
  Napi::ThreadSafeFunction tsfn_;
  napi_threadsafe_function raw_tsfn_;
  bool initialized_;
  std::atomic<int> pending_count_;

  // C promise resolve callback — called from C worker thread
  static void CResolveCallback(void* ctx, void* payload);

  // C promise reject callback — called from C worker thread
  static void CRejectCallback(void* ctx, async_error_t* error);

  // TSFN call_js callback — called on Node.js main thread
  static void CallJs(napi_env env, napi_value jsCallback, void* context, void* data);

  // Convert C result to JS value based on operation type
  static Napi::Value ConvertResult(Napi::Env env, AsyncOpContext* ctx);
};

#endif // WAVEDB_BINDINGS_ASYNC_BRIDGE_H