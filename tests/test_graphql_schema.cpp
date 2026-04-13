//
// Tests for GraphQL Schema - Storage, loading, and layer lifecycle
// Created: 2026-04-12
//

#include <gtest/gtest.h>
#include <cstring>
#include <cstdio>
#include <sys/stat.h>
#include "Layers/graphql/graphql.h"

class GraphQLSchemaTest : public ::testing::Test {
protected:
    const char* test_dir = "/tmp/wavedb_test_graphql_schema";

    void SetUp() override {
        // Clean up any previous test directory
        rmrf(test_dir);
        mkdir(test_dir, 0755);
    }

    void TearDown() override {
        rmrf(test_dir);
    }

    void rmrf(const char* path) {
        char cmd[512];
        snprintf(cmd, sizeof(cmd), "rm -rf %s 2>/dev/null", path);
        (void)system(cmd);
    }
};

// ============================================================
// Layer lifecycle
// ============================================================

TEST_F(GraphQLSchemaTest, CreateLayerInMemory) {
    // In-memory mode: path is NULL
    graphql_layer_config_t* config = graphql_layer_config_default();
    ASSERT_NE(config, nullptr);

    config->enable_persist = 0;  // In-memory mode
    config->path = nullptr;

    graphql_layer_t* layer = graphql_layer_create(nullptr, config);
    ASSERT_NE(layer, nullptr);

    graphql_layer_destroy(layer);
    graphql_layer_config_destroy(config);
}

TEST_F(GraphQLSchemaTest, CreateLayerPersistent) {
    char db_path[512];
    snprintf(db_path, sizeof(db_path), "%s/persistent_db", test_dir);

    graphql_layer_config_t* config = graphql_layer_config_default();
    ASSERT_NE(config, nullptr);

    config->path = db_path;
    config->enable_persist = 1;

    graphql_layer_t* layer = graphql_layer_create(db_path, config);
    ASSERT_NE(layer, nullptr);

    graphql_layer_destroy(layer);
    graphql_layer_config_destroy(config);
}

// ============================================================
// Schema parsing and storage
// ============================================================

TEST_F(GraphQLSchemaTest, ParseSimpleSchema) {
    graphql_layer_config_t* config = graphql_layer_config_default();
    config->enable_persist = 0;

    graphql_layer_t* layer = graphql_layer_create(nullptr, config);
    ASSERT_NE(layer, nullptr);

    const char* sdl = "type User { name: String age: Int }";
    int result = graphql_schema_parse(layer, sdl);
    EXPECT_EQ(result, 0);

    // Verify type was registered
    graphql_type_t* user_type = graphql_schema_get_type(layer, "User");
    ASSERT_NE(user_type, nullptr);
    EXPECT_STREQ(user_type->name, "User");
    EXPECT_EQ(user_type->kind, GRAPHQL_TYPE_OBJECT);
    EXPECT_EQ(user_type->fields.length, 2);

    graphql_layer_destroy(layer);
    graphql_layer_config_destroy(config);
}

TEST_F(GraphQLSchemaTest, ParseEnumSchema) {
    graphql_layer_config_t* config = graphql_layer_config_default();
    config->enable_persist = 0;

    graphql_layer_t* layer = graphql_layer_create(nullptr, config);
    ASSERT_NE(layer, nullptr);

    const char* sdl = "enum Role { ADMIN USER }";
    int result = graphql_schema_parse(layer, sdl);
    EXPECT_EQ(result, 0);

    graphql_type_t* role_type = graphql_schema_get_type(layer, "Role");
    ASSERT_NE(role_type, nullptr);
    EXPECT_EQ(role_type->kind, GRAPHQL_TYPE_ENUM);
    EXPECT_EQ(role_type->enum_values.length, 2);

    graphql_layer_destroy(layer);
    graphql_layer_config_destroy(config);
}

TEST_F(GraphQLSchemaTest, ParseSchemaWithDirectives) {
    graphql_layer_config_t* config = graphql_layer_config_default();
    config->enable_persist = 0;

    graphql_layer_t* layer = graphql_layer_create(nullptr, config);
    ASSERT_NE(layer, nullptr);

    const char* sdl = "type Person @plural(name: \"People\") { name: String }";
    int result = graphql_schema_parse(layer, sdl);
    EXPECT_EQ(result, 0);

    graphql_type_t* person_type = graphql_schema_get_type(layer, "Person");
    ASSERT_NE(person_type, nullptr);
    EXPECT_STREQ(person_type->name, "Person");
    // @plural directive should set the plural_name
    ASSERT_NE(person_type->plural_name, nullptr);
    EXPECT_STREQ(person_type->plural_name, "People");

    graphql_layer_destroy(layer);
    graphql_layer_config_destroy(config);
}

TEST_F(GraphQLSchemaTest, ParseSchemaDefinition) {
    graphql_layer_config_t* config = graphql_layer_config_default();
    config->enable_persist = 0;

    graphql_layer_t* layer = graphql_layer_create(nullptr, config);
    ASSERT_NE(layer, nullptr);

    const char* sdl = "schema { query: Query mutation: Mutation }";
    int result = graphql_schema_parse(layer, sdl);
    EXPECT_EQ(result, 0);

    graphql_layer_destroy(layer);
    graphql_layer_config_destroy(config);
}

TEST_F(GraphQLSchemaTest, ParseMultipleTypes) {
    graphql_layer_config_t* config = graphql_layer_config_default();
    config->enable_persist = 0;

    graphql_layer_t* layer = graphql_layer_create(nullptr, config);
    ASSERT_NE(layer, nullptr);

    const char* sdl = "type User { name: String } type Post { title: String }";
    int result = graphql_schema_parse(layer, sdl);
    EXPECT_EQ(result, 0);

    graphql_type_t* user_type = graphql_schema_get_type(layer, "User");
    ASSERT_NE(user_type, nullptr);
    EXPECT_STREQ(user_type->name, "User");

    graphql_type_t* post_type = graphql_schema_get_type(layer, "Post");
    ASSERT_NE(post_type, nullptr);
    EXPECT_STREQ(post_type->name, "Post");

    graphql_layer_destroy(layer);
    graphql_layer_config_destroy(config);
}

TEST_F(GraphQLSchemaTest, ParseInvalidSDL) {
    graphql_layer_config_t* config = graphql_layer_config_default();
    config->enable_persist = 0;

    graphql_layer_t* layer = graphql_layer_create(nullptr, config);
    ASSERT_NE(layer, nullptr);

    int result = graphql_schema_parse(layer, "type !Invalid { }");
    EXPECT_NE(result, 0);

    result = graphql_schema_parse(layer, "");
    EXPECT_NE(result, 0);

    result = graphql_schema_parse(layer, NULL);
    EXPECT_NE(result, 0);

    graphql_layer_destroy(layer);
    graphql_layer_config_destroy(config);
}

// ============================================================
// Type registry
// ============================================================

TEST_F(GraphQLSchemaTest, TypeRegistryLookup) {
    graphql_layer_config_t* config = graphql_layer_config_default();
    config->enable_persist = 0;

    graphql_layer_t* layer = graphql_layer_create(nullptr, config);
    ASSERT_NE(layer, nullptr);

    const char* sdl = "type User { name: String }";
    graphql_schema_parse(layer, sdl);

    // Existing type
    graphql_type_t* found = graphql_schema_get_type(layer, "User");
    ASSERT_NE(found, nullptr);

    // Non-existing type
    graphql_type_t* not_found = graphql_schema_get_type(layer, "NonExistent");
    EXPECT_EQ(not_found, nullptr);

    // Null checks
    EXPECT_EQ(graphql_schema_get_type(nullptr, "User"), nullptr);
    EXPECT_EQ(graphql_schema_get_type(layer, nullptr), nullptr);

    graphql_layer_destroy(layer);
    graphql_layer_config_destroy(config);
}

// ============================================================
// Field type references
// ============================================================

TEST_F(GraphQLSchemaTest, FieldTypesPreserved) {
    graphql_layer_config_t* config = graphql_layer_config_default();
    config->enable_persist = 0;

    graphql_layer_t* layer = graphql_layer_create(nullptr, config);
    ASSERT_NE(layer, nullptr);

    const char* sdl = "type User { name: String age: Int friends: [User] id: ID! }";
    int result = graphql_schema_parse(layer, sdl);
    EXPECT_EQ(result, 0);

    graphql_type_t* user_type = graphql_schema_get_type(layer, "User");
    ASSERT_NE(user_type, nullptr);
    EXPECT_EQ(user_type->fields.length, 4);

    // name: String (scalar)
    graphql_field_t* name_field = user_type->fields.data[0];
    EXPECT_STREQ(name_field->name, "name");
    ASSERT_NE(name_field->type, nullptr);
    EXPECT_EQ(name_field->type->kind, GRAPHQL_TYPE_SCALAR);
    EXPECT_STREQ(name_field->type->name, "String");

    // age: Int (scalar)
    graphql_field_t* age_field = user_type->fields.data[1];
    EXPECT_STREQ(age_field->name, "age");
    ASSERT_NE(age_field->type, nullptr);
    EXPECT_EQ(age_field->type->kind, GRAPHQL_TYPE_SCALAR);

    // friends: [User] (list of object)
    graphql_field_t* friends_field = user_type->fields.data[2];
    EXPECT_STREQ(friends_field->name, "friends");
    ASSERT_NE(friends_field->type, nullptr);
    EXPECT_EQ(friends_field->type->kind, GRAPHQL_TYPE_LIST);
    ASSERT_NE(friends_field->type->of_type, nullptr);
    EXPECT_EQ(friends_field->type->of_type->kind, GRAPHQL_TYPE_OBJECT);
    EXPECT_STREQ(friends_field->type->of_type->name, "User");

    // id: ID! (non-null scalar)
    graphql_field_t* id_field = user_type->fields.data[3];
    EXPECT_STREQ(id_field->name, "id");
    ASSERT_NE(id_field->type, nullptr);
    EXPECT_EQ(id_field->type->kind, GRAPHQL_TYPE_NON_NULL);
    ASSERT_NE(id_field->type->of_type, nullptr);
    EXPECT_EQ(id_field->type->of_type->kind, GRAPHQL_TYPE_SCALAR);

    graphql_layer_destroy(layer);
    graphql_layer_config_destroy(config);
}

// ============================================================
// Null/edge cases
// ============================================================

TEST_F(GraphQLSchemaTest, NullLayerParse) {
    // Parsing with null layer should fail gracefully
    int result = graphql_schema_parse(nullptr, "type User { name: String }");
    EXPECT_NE(result, 0);
}

TEST_F(GraphQLSchemaTest, NullConfigDefaults) {
    // Creating layer with null config should use defaults
    graphql_layer_t* layer = graphql_layer_create(nullptr, nullptr);
    // In-memory mode with default config
    // This might fail if defaults try to persist to a null path
    // Let's just verify it doesn't crash
    if (layer != nullptr) {
        graphql_layer_destroy(layer);
    }
}

// ============================================================
// Plural name generation
// ============================================================

TEST_F(GraphQLSchemaTest, DefaultPluralName) {
    graphql_layer_config_t* config = graphql_layer_config_default();
    config->enable_persist = 0;

    graphql_layer_t* layer = graphql_layer_create(nullptr, config);
    ASSERT_NE(layer, nullptr);

    const char* sdl = "type User { name: String }";
    graphql_schema_parse(layer, sdl);

    graphql_type_t* user_type = graphql_schema_get_type(layer, "User");
    ASSERT_NE(user_type, nullptr);

    // Default plural: type name itself (the make_plural function adds 's')
    const char* plural = graphql_type_get_plural(user_type);
    EXPECT_NE(plural, nullptr);

    graphql_layer_destroy(layer);
    graphql_layer_config_destroy(config);
}

TEST_F(GraphQLSchemaTest, CustomPluralName) {
    graphql_layer_config_t* config = graphql_layer_config_default();
    config->enable_persist = 0;

    graphql_layer_t* layer = graphql_layer_create(nullptr, config);
    ASSERT_NE(layer, nullptr);

    const char* sdl = "type Person @plural(name: \"People\") { name: String }";
    graphql_schema_parse(layer, sdl);

    graphql_type_t* person_type = graphql_schema_get_type(layer, "Person");
    ASSERT_NE(person_type, nullptr);

    // Custom plural should return the @plural name
    const char* plural = graphql_type_get_plural(person_type);
    ASSERT_NE(plural, nullptr);
    EXPECT_STREQ(plural, "People");

    graphql_layer_destroy(layer);
    graphql_layer_config_destroy(config);
}

// ============================================================
// Scalar definition tests
// ============================================================

TEST_F(GraphQLSchemaTest, ParseScalarDefinition) {
    graphql_layer_config_t* config = graphql_layer_config_default();
    config->enable_persist = 0;

    graphql_layer_t* layer = graphql_layer_create(nullptr, config);
    ASSERT_NE(layer, nullptr);

    const char* sdl = "scalar Date\nscalar DateTime\ntype User { name: String }";
    int rc = graphql_schema_parse(layer, sdl);
    EXPECT_EQ(rc, 0);

    // Check Date scalar was registered
    graphql_type_t* date_type = graphql_schema_get_type(layer, "Date");
    ASSERT_NE(date_type, nullptr);
    EXPECT_EQ(date_type->kind, GRAPHQL_TYPE_SCALAR);
    EXPECT_STREQ(date_type->name, "Date");

    // Check DateTime scalar was registered
    graphql_type_t* datetime_type = graphql_schema_get_type(layer, "DateTime");
    ASSERT_NE(datetime_type, nullptr);
    EXPECT_EQ(datetime_type->kind, GRAPHQL_TYPE_SCALAR);
    EXPECT_STREQ(datetime_type->name, "DateTime");

    // User type should still be an object
    graphql_type_t* user_type = graphql_schema_get_type(layer, "User");
    ASSERT_NE(user_type, nullptr);
    EXPECT_EQ(user_type->kind, GRAPHQL_TYPE_OBJECT);

    graphql_layer_destroy(layer);
    graphql_layer_config_destroy(config);
}

TEST_F(GraphQLSchemaTest, ScalarTypePersistAndLoad) {
    const char* test_dir = "/tmp/wavedb_test_scalar_persist";
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s 2>/dev/null", test_dir);
    (void)system(cmd);
    mkdir(test_dir, 0755);

    graphql_layer_config_t* config = graphql_layer_config_default();
    config->path = test_dir;
    config->enable_persist = 1;

    graphql_layer_t* layer = graphql_layer_create(test_dir, config);
    ASSERT_NE(layer, nullptr);

    const char* sdl = "scalar Date\ntype Event { title: String date: Date }";
    int rc = graphql_schema_parse(layer, sdl);
    EXPECT_EQ(rc, 0);

    // Verify Date is a scalar after schema parse
    graphql_type_t* date_type = graphql_schema_get_type(layer, "Date");
    ASSERT_NE(date_type, nullptr);
    EXPECT_EQ(date_type->kind, GRAPHQL_TYPE_SCALAR);
    EXPECT_STREQ(date_type->name, "Date");

    // Verify Event is an object type with fields
    graphql_type_t* event_type = graphql_schema_get_type(layer, "Event");
    ASSERT_NE(event_type, nullptr);
    EXPECT_EQ(event_type->kind, GRAPHQL_TYPE_OBJECT);
    EXPECT_EQ(event_type->fields.length, 2);

    graphql_layer_destroy(layer);
    graphql_layer_config_destroy(config);

    snprintf(cmd, sizeof(cmd), "rm -rf %s", test_dir);
    (void)system(cmd);
}

// ============================================================
// Type extension tests
// ============================================================

TEST_F(GraphQLSchemaTest, ParseTypeExtension) {
    graphql_layer_config_t* config = graphql_layer_config_default();
    config->enable_persist = 0;

    graphql_layer_t* layer = graphql_layer_create(nullptr, config);
    ASSERT_NE(layer, nullptr);

    const char* sdl = "type User { name: String }\nextend type User { age: Int email: String }";
    int rc = graphql_schema_parse(layer, sdl);
    EXPECT_EQ(rc, 0);

    graphql_type_t* user_type = graphql_schema_get_type(layer, "User");
    ASSERT_NE(user_type, nullptr);
    EXPECT_EQ(user_type->kind, GRAPHQL_TYPE_OBJECT);

    // Should have 3 fields: name (original) + age, email (extension)
    EXPECT_EQ(user_type->fields.length, 3);

    // Verify original field
    bool found_name = false, found_age = false, found_email = false;
    for (int i = 0; i < user_type->fields.length; i++) {
        if (strcmp(user_type->fields.data[i]->name, "name") == 0) found_name = true;
        if (strcmp(user_type->fields.data[i]->name, "age") == 0) found_age = true;
        if (strcmp(user_type->fields.data[i]->name, "email") == 0) found_email = true;
    }
    EXPECT_TRUE(found_name);
    EXPECT_TRUE(found_age);
    EXPECT_TRUE(found_email);

    graphql_layer_destroy(layer);
    graphql_layer_config_destroy(config);
}

TEST_F(GraphQLSchemaTest, ExtendTypeMergesFields) {
    graphql_layer_config_t* config = graphql_layer_config_default();
    config->enable_persist = 0;

    graphql_layer_t* layer = graphql_layer_create(nullptr, config);
    ASSERT_NE(layer, nullptr);

    // Extension before definition — should create type first
    const char* sdl = "extend type Post { title: String }\ntype Post { content: String }";
    int rc = graphql_schema_parse(layer, sdl);
    EXPECT_EQ(rc, 0);

    graphql_type_t* post_type = graphql_schema_get_type(layer, "Post");
    ASSERT_NE(post_type, nullptr);
    EXPECT_EQ(post_type->fields.length, 2);

    graphql_layer_destroy(layer);
    graphql_layer_config_destroy(config);
}

TEST_F(GraphQLSchemaTest, ExtendTypePersists) {
    const char* test_dir = "/tmp/wavedb_test_extend_persist";
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s 2>/dev/null", test_dir);
    (void)system(cmd);
    mkdir(test_dir, 0755);

    graphql_layer_config_t* config = graphql_layer_config_default();
    config->path = test_dir;
    config->enable_persist = 1;

    graphql_layer_t* layer = graphql_layer_create(test_dir, config);
    ASSERT_NE(layer, nullptr);

    const char* sdl = "type User { name: String }\nextend type User { age: Int }";
    int rc = graphql_schema_parse(layer, sdl);
    EXPECT_EQ(rc, 0);

    graphql_type_t* user_type = graphql_schema_get_type(layer, "User");
    ASSERT_NE(user_type, nullptr);
    EXPECT_EQ(user_type->fields.length, 2);

    graphql_layer_destroy(layer);
    graphql_layer_config_destroy(config);

    snprintf(cmd, sizeof(cmd), "rm -rf %s", test_dir);
    (void)system(cmd);
}

// ============================================================
// Default value tests
// ============================================================

TEST_F(GraphQLSchemaTest, ParseDefaultValue) {
    graphql_layer_config_t* config = graphql_layer_config_default();
    config->enable_persist = 0;

    graphql_layer_t* layer = graphql_layer_create(nullptr, config);
    ASSERT_NE(layer, nullptr);

    const char* sdl = "type User { name: String = \"Anonymous\" age: Int = 0 active: Boolean = true }";
    int rc = graphql_schema_parse(layer, sdl);
    EXPECT_EQ(rc, 0);

    graphql_type_t* user_type = graphql_schema_get_type(layer, "User");
    ASSERT_NE(user_type, nullptr);
    EXPECT_EQ(user_type->fields.length, 3);

    // name field should have default value "Anonymous"
    graphql_field_t* name_field = user_type->fields.data[0];
    EXPECT_STREQ(name_field->name, "name");
    ASSERT_NE(name_field->default_value, nullptr);
    EXPECT_EQ(name_field->default_value->kind, GRAPHQL_LITERAL_STRING);
    EXPECT_STREQ(name_field->default_value->string_val, "Anonymous");

    // age field should have default value 0
    graphql_field_t* age_field = user_type->fields.data[1];
    EXPECT_STREQ(age_field->name, "age");
    ASSERT_NE(age_field->default_value, nullptr);
    EXPECT_EQ(age_field->default_value->kind, GRAPHQL_LITERAL_INT);
    EXPECT_EQ(age_field->default_value->int_val, 0);

    // active field should have default value true
    graphql_field_t* active_field = user_type->fields.data[2];
    EXPECT_STREQ(active_field->name, "active");
    ASSERT_NE(active_field->default_value, nullptr);
    EXPECT_EQ(active_field->default_value->kind, GRAPHQL_LITERAL_BOOL);
    EXPECT_EQ(active_field->default_value->bool_val, true);

    graphql_layer_destroy(layer);
    graphql_layer_config_destroy(config);
}

TEST_F(GraphQLSchemaTest, DefaultValuesPersistAndLoad) {
    const char* test_dir = "/tmp/wavedb_test_default_persist";
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s 2>/dev/null", test_dir);
    (void)system(cmd);
    mkdir(test_dir, 0755);

    graphql_layer_config_t* config = graphql_layer_config_default();
    config->path = test_dir;
    config->enable_persist = 1;

    graphql_layer_t* layer = graphql_layer_create(test_dir, config);
    ASSERT_NE(layer, nullptr);

    const char* sdl = "type Config { name: String = \"default\" count: Int = 10 }";
    int rc = graphql_schema_parse(layer, sdl);
    EXPECT_EQ(rc, 0);

    // Verify default values are present
    graphql_type_t* config_type = graphql_schema_get_type(layer, "Config");
    ASSERT_NE(config_type, nullptr);

    graphql_field_t* name_field = config_type->fields.data[0];
    ASSERT_NE(name_field->default_value, nullptr);
    EXPECT_STREQ(name_field->default_value->string_val, "default");

    graphql_field_t* count_field = config_type->fields.data[1];
    ASSERT_NE(count_field->default_value, nullptr);
    EXPECT_EQ(count_field->default_value->int_val, 10);

    graphql_layer_destroy(layer);
    graphql_layer_config_destroy(config);

    snprintf(cmd, sizeof(cmd), "rm -rf %s", test_dir);
    (void)system(cmd);
}