#include "async_bridge.h"
#include "identifier.h"
#include "graphql_result_js.h"
#include <unistd.h>

AsyncBridge::AsyncBridge()
  : raw_tsfn_(nullptr),
    initialized_(false),
    pending_count_(0) {}

AsyncBridge::~AsyncBridge() {
  if (initialized_) {
    Shutdown();
  }
}

void AsyncBridge::Init(Napi::Env env) {
  if (initialized_) return;

  // Create a ThreadSafeFunction. We don't provide a JS function —
  // the CallJs callback handles all result conversion on the main thread.
  napi_value work_name;
  napi_create_string_utf8(env, "WaveDBAsyncBridge", NAPI_AUTO_LENGTH, &work_name);

  napi_status status = napi_create_threadsafe_function(
    env,
    nullptr,        // No JS function
    nullptr,        // No async resource
    work_name,      // Name for debugging
    0,              // Max queue size (0 = unlimited)
    1,              // Initial thread count
    nullptr,        // No thread finalize data
    nullptr,        // No thread finalize callback
    nullptr,        // Context (not needed — we pass context via the call)
    CallJs,         // call_js callback
    &raw_tsfn_
  );

  if (status != napi_ok) {
    return;
  }

  // Also create the Napi::ThreadSafeFunction wrapper for Release() in Shutdown
  tsfn_ = Napi::ThreadSafeFunction(raw_tsfn_);

  initialized_ = true;
}

void AsyncBridge::Shutdown() {
  if (!initialized_) return;

  // Release the TSFN — prevents new calls from C threads
  tsfn_.Release();

  // Wait for pending operations to drain
  uint32_t max_wait_ms = 5000;
  uint32_t waited_ms = 0;
  while (pending_count_.load(std::memory_order_acquire) > 0 && waited_ms < max_wait_ms) {
    usleep(1000);  // 1ms
    waited_ms += 1;
  }

  raw_tsfn_ = nullptr;
  initialized_ = false;
}

promise_t* AsyncBridge::CreatePromise(AsyncOpContext* ctx) {
  if (!initialized_) return nullptr;

  pending_count_.fetch_add(1, std::memory_order_relaxed);

  // Store the raw TSFN handle and pending counter in the context
  ctx->tsfn = raw_tsfn_;
  ctx->pending_count = &pending_count_;

  promise_t* promise = promise_create(CResolveCallback, CRejectCallback, ctx);
  if (!promise) {
    pending_count_.fetch_sub(1, std::memory_order_relaxed);
    return nullptr;
  }

  return promise;
}

// --- C promise callbacks (called from C worker threads) ---

void AsyncBridge::CResolveCallback(void* ctx, void* payload) {
  AsyncOpContext* opCtx = static_cast<AsyncOpContext*>(ctx);
  opCtx->result = payload;
  opCtx->error = nullptr;

  // Signal the Node.js main thread via TSFN
  napi_call_threadsafe_function(opCtx->tsfn, opCtx, napi_tsfn_nonblocking);
}

void AsyncBridge::CRejectCallback(void* ctx, async_error_t* error) {
  AsyncOpContext* opCtx = static_cast<AsyncOpContext*>(ctx);
  opCtx->result = nullptr;
  opCtx->error = error;

  // Signal the Node.js main thread via TSFN
  napi_call_threadsafe_function(opCtx->tsfn, opCtx, napi_tsfn_nonblocking);
}

// --- TSFN call_js callback (runs on Node.js main thread) ---

void AsyncBridge::CallJs(napi_env env, napi_value jsCallback, void* context, void* data) {
  AsyncOpContext* opCtx = static_cast<AsyncOpContext*>(data);
  if (!opCtx) return;

  // Decrement pending count — this operation is now complete
  if (opCtx->pending_count) {
    opCtx->pending_count->fetch_sub(1, std::memory_order_relaxed);
  }

  Napi::Env napiEnv(env);

  // If there was an error, reject the promise
  if (opCtx->error) {
    std::string errorMsg = "Operation failed";
    const char* msg = error_get_message(opCtx->error);
    if (msg != nullptr) {
      errorMsg = msg;
    }
    error_destroy(opCtx->error);

    Napi::Error jsError = Napi::Error::New(napiEnv, errorMsg);
    napi_reject_deferred(env, opCtx->deferred, jsError.Value());

    // If there's a callback, call it with the error
    if (opCtx->callback_ref) {
      napi_value callback;
      napi_get_reference_value(env, opCtx->callback_ref, &callback);
      if (callback) {
        napi_value global;
        napi_get_global(env, &global);
        napi_value argv[2];
        napi_create_string_utf8(env, errorMsg.c_str(), errorMsg.length(), &argv[0]);
        napi_get_null(env, &argv[1]);
        napi_call_function(env, global, callback, 2, argv, nullptr);
      }
      napi_delete_reference(env, opCtx->callback_ref);
    }

    // Clean up C-side allocations on error
    if (opCtx->promise_c) {
      promise_destroy(opCtx->promise_c);
    }
    if (opCtx->batch) {
      batch_destroy(opCtx->batch);
    }

    delete opCtx;
    return;
  }

  // Success — convert result and resolve
  Napi::Value jsResult = ConvertResult(napiEnv, opCtx);
  napi_resolve_deferred(env, opCtx->deferred, jsResult);

  // If there's a callback, call it with (null, result)
  if (opCtx->callback_ref) {
    napi_value callback;
    napi_get_reference_value(env, opCtx->callback_ref, &callback);
    if (callback) {
      napi_value global;
      napi_get_global(env, &global);
      napi_value argv[2];
      napi_get_null(env, &argv[0]);
      argv[1] = jsResult;
      napi_call_function(env, global, callback, 2, argv, nullptr);
    }
    napi_delete_reference(env, opCtx->callback_ref);
  }

  // Clean up C-side allocations
  // Free int* result from batch/put/delete resolve callbacks
  if (opCtx->result && opCtx->type != AsyncOpType::Get &&
      opCtx->type != AsyncOpType::Query && opCtx->type != AsyncOpType::Mutate) {
    free(opCtx->result);
  }

  // Destroy the C promise
  if (opCtx->promise_c) {
    promise_destroy(opCtx->promise_c);
  }

  // Destroy the C batch (if this was a batch operation)
  if (opCtx->batch) {
    batch_destroy(opCtx->batch);
  }

  delete opCtx;
}

// --- Result conversion ---

Napi::Value AsyncBridge::ConvertResult(Napi::Env env, AsyncOpContext* ctx) {
  switch (ctx->type) {
    case AsyncOpType::Get: {
      identifier_t* id = static_cast<identifier_t*>(ctx->result);
      if (!id) {
        return env.Null();
      }
      Napi::Value value = ValueToJS(env, id);
      // The value was CONSUME'd before being passed to the callback
      // (yield=1). REFERENCE consumes the yield (protecting the value
      // from other threads), then identifier_destroy decrements the count
      // to release this reference.
      REFERENCE(id, identifier_t);
      identifier_destroy(id);
      return value;
    }

    case AsyncOpType::Query:
    case AsyncOpType::Mutate: {
      graphql_result_t* result = static_cast<graphql_result_t*>(ctx->result);
      if (!result) {
        return Napi::Object::New(env);
      }
      Napi::Value jsResult = GraphQLResultToJS(env, result);
      graphql_result_destroy(result);
      return jsResult;
    }

    case AsyncOpType::Put:
    case AsyncOpType::Delete:
    case AsyncOpType::Batch:
    default:
      return env.Undefined();
  }
}