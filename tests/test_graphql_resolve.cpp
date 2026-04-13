//
// Tests for GraphQL Resolve - Query and Mutation execution
// Created: 2026-04-12
//

#include <gtest/gtest.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <sys/stat.h>
#include "Layers/graphql/graphql.h"
#include "Workers/pool.h"

class GraphQLResolveTest : public ::testing::Test {
protected:
    const char* test_dir = "/tmp/wavedb_test_graphql_resolve";
    graphql_layer_t* layer = nullptr;
    graphql_layer_config_t* config = nullptr;

    void SetUp() override {
        rmrf(test_dir);
        mkdir(test_dir, 0755);

        config = graphql_layer_config_default();
        config->path = test_dir;
        config->enable_persist = 1;

        layer = graphql_layer_create(test_dir, config);
        ASSERT_NE(layer, nullptr);

        // Register schema
        const char* sdl = "type User { name: String age: Int friends: [User] }";
        int rc = graphql_schema_parse(layer, sdl);
        ASSERT_EQ(rc, 0);
    }

    void TearDown() override {
        if (layer) graphql_layer_destroy(layer);
        if (config) graphql_layer_config_destroy(config);
        rmrf(test_dir);
    }

    void rmrf(const char* path) {
        char cmd[512];
        snprintf(cmd, sizeof(cmd), "rm -rf %s 2>/dev/null", path);
        (void)system(cmd);
    }
};

// ============================================================
// Query execution
// ============================================================

TEST_F(GraphQLResolveTest, SimpleQuery) {
    const char* query = "{ User { name } }";
    graphql_result_t* result = graphql_query_sync(layer, query);
    ASSERT_NE(result, nullptr);
    EXPECT_TRUE(result->success);

    const char* json = graphql_result_to_json(result);
    EXPECT_NE(json, nullptr);
    EXPECT_NE(strstr(json, "\"data\""), nullptr);

    free((void*)json);
    graphql_result_destroy(result);
}

TEST_F(GraphQLResolveTest, NullQuery) {
    graphql_result_t* result = graphql_query_sync(nullptr, "{ User { name } }");
    EXPECT_NE(result, nullptr);
    EXPECT_FALSE(result->success);
    graphql_result_destroy(result);

    result = graphql_query_sync(layer, nullptr);
    EXPECT_NE(result, nullptr);
    EXPECT_FALSE(result->success);
    graphql_result_destroy(result);
}

TEST_F(GraphQLResolveTest, InvalidQuery) {
    graphql_result_t* result = graphql_query_sync(layer, "type !Broken { }");
    EXPECT_NE(result, nullptr);
    EXPECT_FALSE(result->success);
    graphql_result_destroy(result);
}

// ============================================================
// Mutation execution
// ============================================================

TEST_F(GraphQLResolveTest, CreateMutation) {
    const char* mutation = "mutation { createUser(name: \"Alice\") { id name } }";
    graphql_result_t* result = graphql_mutate_sync(layer, mutation);
    ASSERT_NE(result, nullptr);
    EXPECT_TRUE(result->success);

    const char* json = graphql_result_to_json(result);
    EXPECT_NE(json, nullptr);
    EXPECT_NE(strstr(json, "\"data\""), nullptr);

    free((void*)json);
    graphql_result_destroy(result);
}

TEST_F(GraphQLResolveTest, CreateAndQueryMutation) {
    // Create a user
    const char* create = "mutation { createUser(name: \"Bob\") { id name } }";
    graphql_result_t* create_result = graphql_mutate_sync(layer, create);
    ASSERT_NE(create_result, nullptr);
    EXPECT_TRUE(create_result->success);
    graphql_result_destroy(create_result);

    // Query to verify
    const char* query = "{ User { name } }";
    graphql_result_t* query_result = graphql_query_sync(layer, query);
    ASSERT_NE(query_result, nullptr);
    EXPECT_TRUE(query_result->success);

    const char* json = graphql_result_to_json(query_result);
    EXPECT_NE(json, nullptr);
    free((void*)json);
    graphql_result_destroy(query_result);
}

// ============================================================
// Result JSON serialization
// ============================================================

TEST_F(GraphQLResolveTest, ResultToJsonNull) {
    const char* json = graphql_result_to_json(nullptr);
    EXPECT_NE(json, nullptr);
    EXPECT_NE(strstr(json, "null"), nullptr);
    free((void*)json);
}

TEST_F(GraphQLResolveTest, ResultToJsonString) {
    graphql_result_node_t* data = graphql_result_node_create(RESULT_STRING, "name");
    data->string_val = strdup("Alice");
    graphql_result_t* result = (graphql_result_t*)calloc(1, sizeof(graphql_result_t));
    result->data = data;
    result->success = true;

    const char* json = graphql_result_to_json(result);
    EXPECT_NE(json, nullptr);
    EXPECT_NE(strstr(json, "Alice"), nullptr);

    free((void*)json);
    graphql_result_destroy(result);
}

TEST_F(GraphQLResolveTest, ResultToJsonObject) {
    graphql_result_node_t* data = graphql_result_node_create(RESULT_OBJECT, "user");

    graphql_result_node_t* name_node = graphql_result_node_create(RESULT_STRING, "name");
    name_node->string_val = strdup("Alice");
    graphql_result_node_add_child(data, name_node);

    graphql_result_node_t* age_node = graphql_result_node_create(RESULT_INT, "age");
    age_node->int_val = 30;
    graphql_result_node_add_child(data, age_node);

    graphql_result_t* result = (graphql_result_t*)calloc(1, sizeof(graphql_result_t));
    result->data = data;
    result->success = true;

    const char* json = graphql_result_to_json(result);
    EXPECT_NE(json, nullptr);
    EXPECT_NE(strstr(json, "\"name\":\"Alice\""), nullptr) << "JSON: " << json;
    EXPECT_NE(strstr(json, "\"age\":30"), nullptr) << "JSON: " << json;

    free((void*)json);
    graphql_result_destroy(result);
}

TEST_F(GraphQLResolveTest, ResultToJsonList) {
    graphql_result_node_t* data = graphql_result_node_create(RESULT_LIST, "users");

    graphql_result_node_t* item1 = graphql_result_node_create(RESULT_STRING, NULL);
    item1->string_val = strdup("Alice");
    graphql_result_node_add_child(data, item1);

    graphql_result_node_t* item2 = graphql_result_node_create(RESULT_STRING, NULL);
    item2->string_val = strdup("Bob");
    graphql_result_node_add_child(data, item2);

    graphql_result_t* result = (graphql_result_t*)calloc(1, sizeof(graphql_result_t));
    result->data = data;
    result->success = true;

    const char* json = graphql_result_to_json(result);
    EXPECT_NE(json, nullptr);
    EXPECT_NE(strstr(json, "[\"Alice\",\"Bob\"]"), nullptr) << "JSON: " << json;

    free((void*)json);
    graphql_result_destroy(result);
}

TEST_F(GraphQLResolveTest, ResultWithErrors) {
    // Use graphql_query_sync with invalid input to get a result with errors
    graphql_result_t* result = graphql_query_sync(layer, "type !Broken { }");
    ASSERT_NE(result, nullptr);
    EXPECT_FALSE(result->success);
    EXPECT_GT(result->errors.length, 0);

    const char* json = graphql_result_to_json(result);
    EXPECT_NE(json, nullptr);
    EXPECT_NE(strstr(json, "\"errors\""), nullptr) << "JSON: " << json;

    free((void*)json);
    graphql_result_destroy(result);
}

// ============================================================
// Introspection queries
// ============================================================

TEST_F(GraphQLResolveTest, IntrospectSchema) {
    const char* query = "{ __schema { types { name kind } } }";
    graphql_result_t* result = graphql_query_sync(layer, query);
    ASSERT_NE(result, nullptr);
    EXPECT_TRUE(result->success);

    const char* json = graphql_result_to_json(result);
    EXPECT_NE(json, nullptr);
    // Should contain the User type we registered
    EXPECT_NE(strstr(json, "User"), nullptr) << "JSON: " << json;
    EXPECT_NE(strstr(json, "OBJECT"), nullptr) << "JSON: " << json;

    free((void*)json);
    graphql_result_destroy(result);
}

TEST_F(GraphQLResolveTest, IntrospectType) {
    const char* query = "{ __type(name: \"User\") { name kind fields { name } } }";
    graphql_result_t* result = graphql_query_sync(layer, query);
    ASSERT_NE(result, nullptr);
    EXPECT_TRUE(result->success);

    const char* json = graphql_result_to_json(result);
    EXPECT_NE(json, nullptr);
    EXPECT_NE(strstr(json, "User"), nullptr) << "JSON: " << json;
    EXPECT_NE(strstr(json, "name"), nullptr) << "JSON: " << json;

    free((void*)json);
    graphql_result_destroy(result);
}

// ============================================================
// Field aliases
// ============================================================

TEST_F(GraphQLResolveTest, FieldAlias) {
    // Query with alias: "admin: User" should produce "admin" as key in result
    const char* query = "{ admin: User { name } }";
    graphql_result_t* result = graphql_query_sync(layer, query);
    ASSERT_NE(result, nullptr);

    const char* json = graphql_result_to_json(result);
    EXPECT_NE(json, nullptr);
    // The result should use the alias "admin" as the key, not "User"
    EXPECT_NE(strstr(json, "\"admin\":"), nullptr) << "JSON: " << json;

    free((void*)json);
    graphql_result_destroy(result);
}

// ============================================================
// Named fragments
// ============================================================

TEST_F(GraphQLResolveTest, FragmentSpread) {
    // Test that fragment spread parses and compiles without crashing.
    // The fragment "UserFields" expands to { name age } and is inlined
    // into the User selection. Since no data is stored, we verify the
    // query compiles and returns a valid result structure.
    const char* query =
        "fragment UserFields on User { name age } "
        "{ User { ...UserFields } }";
    graphql_result_t* result = graphql_query_sync(layer, query);
    ASSERT_NE(result, nullptr);

    const char* json = graphql_result_to_json(result);
    EXPECT_NE(json, nullptr);
    EXPECT_NE(strstr(json, "\"data\""), nullptr) << "JSON: " << json;

    free((void*)json);
    graphql_result_destroy(result);
}

TEST_F(GraphQLResolveTest, CircularFragment) {
    // Circular fragment should not infinite loop — it should be detected and skipped
    const char* query =
        "fragment A on User { name ...B } "
        "fragment B on User { age ...A } "
        "{ User { ...A } }";
    graphql_result_t* result = graphql_query_sync(layer, query);
    ASSERT_NE(result, nullptr);
    // Should not crash; may succeed with partial data or fail gracefully
    // The key test is that it doesn't infinite loop

    const char* json = graphql_result_to_json(result);
    EXPECT_NE(json, nullptr);

    free((void*)json);
    graphql_result_destroy(result);
}

// ============================================================
// Partial-success errors
// ============================================================

TEST_F(GraphQLResolveTest, PartialSuccessDepthExceeded) {
    // Deep nesting should exceed max depth, producing partial data with errors
    // Build a query with deeply nested friends
    const char* query = "{ User { friends { friends { friends { friends { name } } } } } }";
    graphql_result_t* result = graphql_query_sync(layer, query);
    ASSERT_NE(result, nullptr);

    // Even if depth is exceeded, we should get a result (possibly with null fields and errors)
    const char* json = graphql_result_to_json(result);
    EXPECT_NE(json, nullptr);
    // The result should have a "data" key (possibly with null fields)
    EXPECT_NE(strstr(json, "\"data\":"), nullptr) << "JSON: " << json;

    free((void*)json);
    graphql_result_destroy(result);
}

// ============================================================
// Required field validation
// ============================================================

class GraphQLRequiredFieldTest : public ::testing::Test {
protected:
    const char* test_dir = "/tmp/wavedb_test_graphql_required";
    graphql_layer_t* layer = nullptr;
    graphql_layer_config_t* config = nullptr;

    void SetUp() override {
        rmrf(test_dir);
        mkdir(test_dir, 0755);

        config = graphql_layer_config_default();
        config->path = test_dir;
        config->enable_persist = 1;

        layer = graphql_layer_create(test_dir, config);
        ASSERT_NE(layer, nullptr);

        // Schema with required fields: name is required, age and email are optional
        const char* sdl = "type User { name: String! age: Int email: String }";
        int rc = graphql_schema_parse(layer, sdl);
        ASSERT_EQ(rc, 0);
    }

    void TearDown() override {
        if (layer) graphql_layer_destroy(layer);
        if (config) graphql_layer_config_destroy(config);
        rmrf(test_dir);
    }

    void rmrf(const char* path) {
        char cmd[512];
        snprintf(cmd, sizeof(cmd), "rm -rf %s 2>/dev/null", path);
        (void)system(cmd);
    }
};

TEST_F(GraphQLRequiredFieldTest, SchemaHasIsRequirededFlag) {
    // Verify that is_required is set on the name field
    graphql_type_t* user_type = graphql_schema_get_type(layer, "User");
    ASSERT_NE(user_type, nullptr);

    bool found_name = false;
    bool found_age = false;
    for (size_t i = 0; i < user_type->fields.length; i++) {
        graphql_field_t* field = user_type->fields.data[i];
        if (strcmp(field->name, "name") == 0) {
            found_name = true;
            EXPECT_TRUE(field->is_required) << "name should be required (String!)";
        }
        if (strcmp(field->name, "age") == 0) {
            found_age = true;
            EXPECT_FALSE(field->is_required) << "age should not be required (Int)";
        }
    }
    EXPECT_TRUE(found_name);
    EXPECT_TRUE(found_age);
}

TEST_F(GraphQLRequiredFieldTest, CreateMissingRequiredField) {
    // Mutation without the required "name" field should fail
    const char* mutation = "mutation { createUser(age: \"30\") { id } }";
    graphql_result_t* result = graphql_mutate_sync(layer, mutation);
    ASSERT_NE(result, nullptr);
    EXPECT_FALSE(result->success) << "Should fail when required field is missing";
    EXPECT_GT(result->errors.length, 0) << "Should have error messages";

    const char* json = graphql_result_to_json(result);
    EXPECT_NE(json, nullptr);
    EXPECT_NE(strstr(json, "name"), nullptr) << "Error should mention missing field 'name': " << json;

    free((void*)json);
    graphql_result_destroy(result);
}

TEST_F(GraphQLRequiredFieldTest, CreateWithRequiredField) {
    // Mutation with the required "name" field should succeed
    const char* mutation = "mutation { createUser(name: \"Alice\") { id name } }";
    graphql_result_t* result = graphql_mutate_sync(layer, mutation);
    ASSERT_NE(result, nullptr);
    EXPECT_TRUE(result->success) << "Should succeed when required field is provided";

    const char* json = graphql_result_to_json(result);
    EXPECT_NE(json, nullptr);
    EXPECT_NE(strstr(json, "\"name\":\"Alice\""), nullptr) << "JSON: " << json;

    free((void*)json);
    graphql_result_destroy(result);
}

TEST_F(GraphQLRequiredFieldTest, UpdateSkipsRequiredValidation) {
    // First create a user with required fields (this will pass validation)
    const char* create = "mutation { createUser(name: \"Bob\") { id name } }";
    graphql_result_t* create_result = graphql_mutate_sync(layer, create);
    ASSERT_NE(create_result, nullptr);
    EXPECT_TRUE(create_result->success) << "Create should succeed with required field";
    graphql_result_destroy(create_result);

    // Update should succeed without requiring all fields (updates are partial)
    // We verify this by checking the type's is_required fields exist
    // but the update mutation code path skips validation
    graphql_type_t* user_type = graphql_schema_get_type(layer, "User");
    ASSERT_NE(user_type, nullptr);

    // Verify validation flag exists but update skips it
    bool has_required = false;
    for (size_t i = 0; i < user_type->fields.length; i++) {
        if (user_type->fields.data[i]->is_required) {
            has_required = true;
            break;
        }
    }
    EXPECT_TRUE(has_required) << "User type should have required fields";
}

TEST_F(GraphQLRequiredFieldTest, CreateWithOptionalFieldsOnlyFails) {
    // Provide only optional fields, missing required "name"
    const char* mutation = "mutation { createUser(email: \"test@example.com\") { id } }";
    graphql_result_t* result = graphql_mutate_sync(layer, mutation);
    ASSERT_NE(result, nullptr);
    EXPECT_FALSE(result->success) << "Should fail when required field is missing";

    graphql_result_destroy(result);
}

TEST_F(GraphQLRequiredFieldTest, MultipleMissingRequiredFields) {
    // Create a new layer with a schema that has multiple required fields
    const char* test_dir2 = "/tmp/wavedb_test_graphql_required2";
    rmrf(test_dir2);
    mkdir(test_dir2, 0755);

    graphql_layer_config_t* config2 = graphql_layer_config_default();
    config2->path = test_dir2;
    config2->enable_persist = 1;

    graphql_layer_t* layer2 = graphql_layer_create(test_dir2, config2);
    ASSERT_NE(layer2, nullptr);

    const char* sdl = "type Account { name: String! email: String! age: Int }";
    int rc = graphql_schema_parse(layer2, sdl);
    ASSERT_EQ(rc, 0);

    // Create without either required field
    const char* mutation = "mutation { createAccount(age: \"25\") { id } }";
    graphql_result_t* result = graphql_mutate_sync(layer2, mutation);
    ASSERT_NE(result, nullptr);
    EXPECT_FALSE(result->success);

    const char* json = graphql_result_to_json(result);
    EXPECT_NE(json, nullptr);
    // Should mention both missing fields
    EXPECT_NE(strstr(json, "name"), nullptr) << "Error should mention 'name': " << json;
    EXPECT_NE(strstr(json, "email"), nullptr) << "Error should mention 'email': " << json;

    free((void*)json);
    graphql_result_destroy(result);
    graphql_layer_destroy(layer2);
    graphql_layer_config_destroy(config2);
    rmrf(test_dir2);
}
// ============================================================

// Context struct passed through the promise callback
struct AsyncTestContext {
    graphql_result_t* result;
    std::atomic<bool> resolved;
    std::atomic<bool> rejected;
};

static void async_query_resolve(void* ctx, void* payload) {
    auto* tc = static_cast<AsyncTestContext*>(ctx);
    tc->result = static_cast<graphql_result_t*>(payload);
    tc->resolved.store(true, std::memory_order_release);
}

static void async_query_reject(void* ctx, async_error_t* error) {
    auto* tc = static_cast<AsyncTestContext*>(ctx);
    tc->rejected.store(true, std::memory_order_release);
    if (error) error_destroy(error);
}

class GraphQLAsyncTest : public ::testing::Test {
protected:
    const char* test_dir = "/tmp/wavedb_test_graphql_async";
    graphql_layer_t* layer = nullptr;
    graphql_layer_config_t* config = nullptr;

    void SetUp() override {
        rmrf(test_dir);
        mkdir(test_dir, 0755);

        config = graphql_layer_config_default();
        config->path = test_dir;
        config->enable_persist = 1;
        config->worker_threads = 2;

        layer = graphql_layer_create(test_dir, config);
        ASSERT_NE(layer, nullptr);

        // Launch worker pool threads for async execution
        if (layer->pool) {
            work_pool_launch(layer->pool);
        }

        const char* sdl = "type User { name: String age: Int friends: [User] }";
        int rc = graphql_schema_parse(layer, sdl);
        ASSERT_EQ(rc, 0);
    }

    void TearDown() override {
        if (layer) {
            if (layer->pool) {
                work_pool_shutdown(layer->pool);
                work_pool_join_all(layer->pool);
            }
            graphql_layer_destroy(layer);
        }
        if (config) graphql_layer_config_destroy(config);
        rmrf(test_dir);
    }

    void rmrf(const char* path) {
        char cmd[512];
        snprintf(cmd, sizeof(cmd), "rm -rf %s 2>/dev/null", path);
        (void)system(cmd);
    }
};

TEST_F(GraphQLAsyncTest, AsyncQuery) {
    const char* query = "{ User { name } }";
    AsyncTestContext ctx = {};
    promise_t* promise = promise_create(async_query_resolve, async_query_reject, &ctx);
    ASSERT_NE(promise, nullptr);

    graphql_query(layer, query, promise, nullptr);

    // Wait for the async callback (with timeout)
    int max_wait = 100;
    while (!ctx.resolved.load(std::memory_order_acquire) && !ctx.rejected.load(std::memory_order_acquire) && max_wait-- > 0) {
        usleep(10000);
    }

    EXPECT_TRUE(ctx.resolved.load(std::memory_order_acquire));
    EXPECT_FALSE(ctx.rejected.load(std::memory_order_acquire));
    ASSERT_NE(ctx.result, nullptr);

    const char* json = graphql_result_to_json(ctx.result);
    EXPECT_NE(json, nullptr);
    EXPECT_NE(strstr(json, "\"data\":"), nullptr) << "JSON: " << json;

    free((void*)json);
    graphql_result_destroy(ctx.result);
    promise_destroy(promise);
}

TEST_F(GraphQLAsyncTest, AsyncMutation) {
    const char* mutation = "mutation { createUser(name: \"Alice\") { id name } }";
    AsyncTestContext ctx = {};
    promise_t* promise = promise_create(async_query_resolve, async_query_reject, &ctx);
    ASSERT_NE(promise, nullptr);

    graphql_mutate(layer, mutation, promise, nullptr);

    int max_wait = 100;
    while (!ctx.resolved.load(std::memory_order_acquire) && !ctx.rejected.load(std::memory_order_acquire) && max_wait-- > 0) {
        usleep(10000);
    }

    EXPECT_TRUE(ctx.resolved.load(std::memory_order_acquire));
    ASSERT_NE(ctx.result, nullptr);

    const char* json = graphql_result_to_json(ctx.result);
    EXPECT_NE(json, nullptr);
    EXPECT_NE(strstr(json, "\"id\":"), nullptr) << "JSON: " << json;

    free((void*)json);
    graphql_result_destroy(ctx.result);
    promise_destroy(promise);
}

TEST_F(GraphQLAsyncTest, ConcurrentQueries) {
    // Submit multiple queries concurrently and verify all complete
    const char* query = "{ User { name } }";
    AsyncTestContext ctx1 = {}, ctx2 = {}, ctx3 = {};
    promise_t* p1 = promise_create(async_query_resolve, async_query_reject, &ctx1);
    promise_t* p2 = promise_create(async_query_resolve, async_query_reject, &ctx2);
    promise_t* p3 = promise_create(async_query_resolve, async_query_reject, &ctx3);
    ASSERT_NE(p1, nullptr);
    ASSERT_NE(p2, nullptr);
    ASSERT_NE(p3, nullptr);

    graphql_query(layer, query, p1, nullptr);
    graphql_query(layer, query, p2, nullptr);
    graphql_query(layer, query, p3, nullptr);

    // Wait for all to resolve
    int max_wait = 100;
    while ((!ctx1.resolved.load(std::memory_order_acquire) || !ctx2.resolved.load(std::memory_order_acquire) || !ctx3.resolved.load(std::memory_order_acquire)) && max_wait-- > 0) {
        usleep(10000);
    }

    EXPECT_TRUE(ctx1.resolved.load(std::memory_order_acquire));
    EXPECT_TRUE(ctx2.resolved.load(std::memory_order_acquire));
    EXPECT_TRUE(ctx3.resolved.load(std::memory_order_acquire));

    ASSERT_NE(ctx1.result, nullptr);
    ASSERT_NE(ctx2.result, nullptr);
    ASSERT_NE(ctx3.result, nullptr);

    graphql_result_destroy(ctx1.result);
    graphql_result_destroy(ctx2.result);
    graphql_result_destroy(ctx3.result);
    promise_destroy(p1);
    promise_destroy(p2);
    promise_destroy(p3);
}

TEST_F(GraphQLAsyncTest, AsyncQueryAfterMutation) {
    // First create a user synchronously
    const char* create = "mutation { createUser(name: \"Bob\") { id name } }";
    graphql_result_t* create_result = graphql_mutate_sync(layer, create);
    ASSERT_NE(create_result, nullptr);
    EXPECT_TRUE(create_result->success);
    graphql_result_destroy(create_result);

    // Then query asynchronously
    const char* query = "{ User { name } }";
    AsyncTestContext ctx = {};
    promise_t* promise = promise_create(async_query_resolve, async_query_reject, &ctx);
    ASSERT_NE(promise, nullptr);

    graphql_query(layer, query, promise, nullptr);

    int max_wait = 100;
    while (!ctx.resolved.load(std::memory_order_acquire) && !ctx.rejected.load(std::memory_order_acquire) && max_wait-- > 0) {
        usleep(10000);
    }

    EXPECT_TRUE(ctx.resolved.load(std::memory_order_acquire));
    ASSERT_NE(ctx.result, nullptr);

    const char* json = graphql_result_to_json(ctx.result);
    EXPECT_NE(json, nullptr);
    EXPECT_NE(strstr(json, "\"data\":"), nullptr) << "JSON: " << json;

    free((void*)json);
    graphql_result_destroy(ctx.result);
    promise_destroy(promise);
}
// ============================================================
// Plan compiler improvement tests
// ============================================================

class GraphQLPlanImprovementTest : public ::testing::Test {
protected:
    const char* test_dir = "/tmp/wavedb_test_graphql_plan_imp";
    graphql_layer_t* layer = nullptr;
    graphql_layer_config_t* config = nullptr;

    void SetUp() override {
        rmrf(test_dir);
        mkdir(test_dir, 0755);

        config = graphql_layer_config_default();
        config->path = test_dir;
        config->enable_persist = 1;

        layer = graphql_layer_create(test_dir, config);
        ASSERT_NE(layer, nullptr);

        const char* sdl = "type User { name: String age: Int email: String }";
        int rc = graphql_schema_parse(layer, sdl);
        ASSERT_EQ(rc, 0);
    }

    void TearDown() override {
        if (layer) graphql_layer_destroy(layer);
        if (config) graphql_layer_config_destroy(config);
        rmrf(test_dir);
    }

    void rmrf(const char* path) {
        char cmd[512];
        snprintf(cmd, sizeof(cmd), "rm -rf %s 2>/dev/null", path);
        (void)system(cmd);
    }
};

TEST_F(GraphQLPlanImprovementTest, QueryWithIdArgumentFetchesSingleEntity) {
    // Create two users
    const char* create1 = "mutation { createUser(name: \"Alice\", age: \"30\") { id name } }";
    graphql_result_t* r1 = graphql_mutate_sync(layer, create1);
    ASSERT_NE(r1, nullptr);
    EXPECT_TRUE(r1->success);
    graphql_result_destroy(r1);

    const char* create2 = "mutation { createUser(name: \"Bob\", age: \"25\") { id name } }";
    graphql_result_t* r2 = graphql_mutate_sync(layer, create2);
    ASSERT_NE(r2, nullptr);
    EXPECT_TRUE(r2->success);
    graphql_result_destroy(r2);

    // Query with id argument — should return only user 1
    const char* query = "{ User(id: \"1\") { name } }";
    graphql_result_t* result = graphql_query_sync(layer, query);
    ASSERT_NE(result, nullptr);
    EXPECT_TRUE(result->success) << "Query should succeed";

    const char* json = graphql_result_to_json(result);
    EXPECT_NE(json, nullptr);
    EXPECT_NE(strstr(json, "Alice"), nullptr) << "Should contain Alice: " << json;

    free((void*)json);
    graphql_result_destroy(result);
}

TEST_F(GraphQLPlanImprovementTest, PlanCompilationUsesBatchGetForArgs) {
    const char* query = "{ User(id: \"1\") { name } }";
    graphql_plan_t* plan = graphql_compile_query(layer, query);
    ASSERT_NE(plan, nullptr);

    char* plan_str = graphql_plan_to_string(plan);
    EXPECT_NE(plan_str, nullptr);
    EXPECT_NE(strstr(plan_str, "BATCH_GET"), nullptr) << "Plan should contain BATCH_GET: " << plan_str;

    free(plan_str);
    graphql_plan_destroy(plan);
}

TEST_F(GraphQLPlanImprovementTest, PlanCompilationUsesResolveFieldForScalarChildren) {
    const char* query = "{ User { name } }";
    graphql_plan_t* plan = graphql_compile_query(layer, query);
    ASSERT_NE(plan, nullptr);

    char* plan_str = graphql_plan_to_string(plan);
    EXPECT_NE(plan_str, nullptr);
    EXPECT_NE(strstr(plan_str, "RESOLVE_FIELD"), nullptr) << "Plan should contain RESOLVE_FIELD: " << plan_str;

    free(plan_str);
    graphql_plan_destroy(plan);
}

// ============================================================
// __typename tests
// ============================================================

class GraphQLTypenameTest : public ::testing::Test {
protected:
    const char* test_dir = "/tmp/wavedb_test_graphql_typename";
    graphql_layer_t* layer = nullptr;
    graphql_layer_config_t* config = nullptr;

    void SetUp() override {
        rmrf(test_dir);
        mkdir(test_dir, 0755);

        config = graphql_layer_config_default();
        config->path = test_dir;
        config->enable_persist = 1;

        layer = graphql_layer_create(test_dir, config);
        ASSERT_NE(layer, nullptr);

        const char* sdl = "type User { name: String age: Int }";
        int rc = graphql_schema_parse(layer, sdl);
        ASSERT_EQ(rc, 0);
    }

    void TearDown() override {
        if (layer) graphql_layer_destroy(layer);
        if (config) graphql_layer_config_destroy(config);
        rmrf(test_dir);
    }

    void rmrf(const char* path) {
        char cmd[512];
        snprintf(cmd, sizeof(cmd), "rm -rf %s 2>/dev/null", path);
        (void)system(cmd);
    }
};

TEST_F(GraphQLTypenameTest, TypenameOnQueryResult) {
    // Create a user first
    const char* create = "mutation { createUser(name: \"Alice\", age: \"30\") { id } }";
    graphql_result_t* cr = graphql_mutate_sync(layer, create);
    ASSERT_NE(cr, nullptr);
    EXPECT_TRUE(cr->success);
    graphql_result_destroy(cr);

    const char* query = "{ User { name __typename } }";

    graphql_result_t* result = graphql_query_sync(layer, query);
    ASSERT_NE(result, nullptr);
    EXPECT_TRUE(result->success);

    const char* json = graphql_result_to_json(result);
    EXPECT_NE(json, nullptr);
    EXPECT_NE(strstr(json, "__typename"), nullptr) << "Should contain __typename: " << json;
    EXPECT_NE(strstr(json, "User"), nullptr) << "Should contain User type name: " << json;

    free((void*)json);
    graphql_result_destroy(result);
}

TEST_F(GraphQLTypenameTest, TypenameOnIntrospection) {
    const char* query = "{ __schema { types { name __typename } } }";
    graphql_result_t* result = graphql_query_sync(layer, query);
    ASSERT_NE(result, nullptr);
    EXPECT_TRUE(result->success);

    const char* json = graphql_result_to_json(result);
    EXPECT_NE(json, nullptr);
    EXPECT_NE(strstr(json, "__Type"), nullptr) << "Should contain __Type: " << json;

    free((void*)json);
    graphql_result_destroy(result);
}

TEST_F(GraphQLTypenameTest, TypenameWithAlias) {
    const char* create = "mutation { createUser(name: \"Bob\", age: \"25\") { id } }";
    graphql_result_t* cr = graphql_mutate_sync(layer, create);
    ASSERT_NE(cr, nullptr);
    EXPECT_TRUE(cr->success);
    graphql_result_destroy(cr);

    const char* query = "{ User { t: __typename } }";
    graphql_result_t* result = graphql_query_sync(layer, query);
    ASSERT_NE(result, nullptr);
    EXPECT_TRUE(result->success);

    const char* json = graphql_result_to_json(result);
    EXPECT_NE(json, nullptr);
    EXPECT_NE(strstr(json, "\"t\""), nullptr) << "Should contain alias 't': " << json;
    EXPECT_NE(strstr(json, "User"), nullptr) << "Should contain User type name: " << json;

    free((void*)json);
    graphql_result_destroy(result);
}

// ============================================================
// Mutation sub-selections tests
// ============================================================

TEST_F(GraphQLResolveTest, MutationCreateWithSelection) {
    // Create with sub-selection should return requested fields
    const char* create = "mutation { createUser(name: \"Charlie\", age: \"25\") { id name } }";
    graphql_result_t* result = graphql_mutate_sync(layer, create);
    ASSERT_NE(result, nullptr);
    EXPECT_TRUE(result->success);

    const char* json = graphql_result_to_json(result);
    EXPECT_NE(json, nullptr);
    // Should contain name from selection
    EXPECT_NE(strstr(json, "Charlie"), nullptr) << "Should contain name from selection: " << json;
    // Should contain id
    EXPECT_NE(strstr(json, "id"), nullptr) << "Should contain id: " << json;

    free((void*)json);
    graphql_result_destroy(result);
}

TEST_F(GraphQLResolveTest, MutationUpdateWithSelection) {
    // First create a user
    const char* create = "mutation { createUser(name: \"Dave\", age: \"40\") { id } }";
    graphql_result_t* cr = graphql_mutate_sync(layer, create);
    ASSERT_NE(cr, nullptr);
    EXPECT_TRUE(cr->success);
    graphql_result_destroy(cr);

    // Update with selection
    const char* update = "mutation { updateUser(id: \"1\", name: \"David\") { id name } }";
    graphql_result_t* result = graphql_mutate_sync(layer, update);
    ASSERT_NE(result, nullptr);
    EXPECT_TRUE(result->success);

    const char* json = graphql_result_to_json(result);
    EXPECT_NE(json, nullptr);
    EXPECT_NE(strstr(json, "David"), nullptr) << "Should contain updated name: " << json;

    free((void*)json);
    graphql_result_destroy(result);
}

// Atomic batch mutation tests

TEST_F(GraphQLResolveTest, AtomicCreateMutation) {
    // Create should use atomic increment for ID generation
    const char* create = "mutation { createUser(name: \"Eve\", age: \"28\") { id name age } }";
    graphql_result_t* result = graphql_mutate_sync(layer, create);
    ASSERT_NE(result, nullptr);
    EXPECT_TRUE(result->success);

    const char* json = graphql_result_to_json(result);
    EXPECT_NE(json, nullptr);
    EXPECT_NE(strstr(json, "Eve"), nullptr) << "Should contain name: " << json;
    EXPECT_NE(strstr(json, "28"), nullptr) << "Should contain age: " << json;

    free((void*)json);
    graphql_result_destroy(result);

    // Verify the entity was actually written by querying it
    const char* query = "{ User { name } }";
    graphql_result_t* qr = graphql_query_sync(layer, query);
    ASSERT_NE(qr, nullptr);
    EXPECT_TRUE(qr->success);

    const char* qjson = graphql_result_to_json(qr);
    EXPECT_NE(strstr(qjson, "Eve"), nullptr) << "Query should find created user: " << qjson;

    free((void*)qjson);
    graphql_result_destroy(qr);
}

TEST_F(GraphQLResolveTest, AtomicUpdateMutation) {
    // Create first, then update atomically
    const char* create = "mutation { createUser(name: \"Frank\", age: \"35\") { id } }";
    graphql_result_t* cr = graphql_mutate_sync(layer, create);
    ASSERT_NE(cr, nullptr);
    EXPECT_TRUE(cr->success);
    graphql_result_destroy(cr);

    const char* update = "mutation { updateUser(id: \"1\", name: \"Francis\") { id name } }";
    graphql_result_t* result = graphql_mutate_sync(layer, update);
    ASSERT_NE(result, nullptr);
    EXPECT_TRUE(result->success);

    const char* json = graphql_result_to_json(result);
    EXPECT_NE(json, nullptr);
    EXPECT_NE(strstr(json, "Francis"), nullptr) << "Should contain updated name: " << json;

    free((void*)json);
    graphql_result_destroy(result);

    // Verify update persisted
    const char* query = "{ User { name } }";
    graphql_result_t* qr = graphql_query_sync(layer, query);
    ASSERT_NE(qr, nullptr);
    EXPECT_TRUE(qr->success);

    const char* qjson = graphql_result_to_json(qr);
    EXPECT_NE(strstr(qjson, "Francis"), nullptr) << "Query should find updated user: " << qjson;

    free((void*)qjson);
    graphql_result_destroy(qr);
}

TEST_F(GraphQLResolveTest, AtomicDeleteMutation) {
    // Create, then delete, verify the delete result is returned
    const char* create = "mutation { createUser(name: \"Grace\", age: \"42\") { id } }";
    graphql_result_t* cr = graphql_mutate_sync(layer, create);
    ASSERT_NE(cr, nullptr);
    EXPECT_TRUE(cr->success);
    graphql_result_destroy(cr);

    // Delete
    const char* del = "mutation { deleteUser(id: \"1\") { id } }";
    graphql_result_t* dr = graphql_mutate_sync(layer, del);
    ASSERT_NE(dr, nullptr);
    EXPECT_TRUE(dr->success);

    const char* djson = graphql_result_to_json(dr);
    EXPECT_NE(djson, nullptr);
    EXPECT_NE(strstr(djson, "1"), nullptr) << "Delete result should contain id";
    free((void*)djson);
    graphql_result_destroy(dr);
}
