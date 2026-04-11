Napi::Value WaveDB::Close(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (db_) {
    database_t* db = db_;
    db_ = nullptr;  // Clear pointer first to prevent double-destroy

    // Dereference the database. If async workers or iterators still hold
    // references, the actual destruction is deferred until they release them.
    // This is safe because those holders also check for null/closed state.
    database_destroy(db);

    // Release the JS object reference so the databaseRef_ in async workers
    // and iterators is no longer needed to keep the JS wrapper alive
  }

  return env.Undefined();
}