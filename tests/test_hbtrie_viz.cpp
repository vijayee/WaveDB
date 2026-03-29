//
// Test for HBTrie visualization
//

#include <gtest/gtest.h>
#include "HBTrie/hbtrie.h"
#include "HBTrie/hbtrie_viz.h"
#include "Buffer/buffer.h"
#include <fstream>
#include <sstream>
#include <string>
#include <cstring>

class HbtrieVizTest : public ::testing::Test {
protected:
    void SetUp() override {
        trie = hbtrie_create(4, 4096);
        ASSERT_NE(trie, nullptr);
    }

    void TearDown() override {
        if (trie) {
            hbtrie_destroy(trie);
        }
    }

    hbtrie_t* trie;

    // Helper to create a path from string subscripts
    path_t* make_path(std::initializer_list<const char*> subscripts) {
        path_t* path = path_create();
        for (const char* sub : subscripts) {
            buffer_t* buf = buffer_create_from_pointer_copy((uint8_t*)sub, strlen(sub));
            identifier_t* id = identifier_create(buf, 0);
            buffer_destroy(buf);
            path_append(path, id);
            identifier_destroy(id);
        }
        return path;
    }

    // Helper to create an identifier value
    identifier_t* make_value(const char* data) {
        buffer_t* buf = buffer_create_from_pointer_copy((uint8_t*)data, strlen(data));
        identifier_t* id = identifier_create(buf, 0);
        buffer_destroy(buf);
        return id;
    }

    // Helper to read file contents
    std::string read_file(const char* path) {
        std::ifstream ifs(path);
        std::stringstream buffer;
        buffer << ifs.rdbuf();
        return buffer.str();
    }
};

TEST_F(HbtrieVizTest, EmptyTrie) {
    const char* output_path = "/tmp/test_empty.html";

    int result = hbtrie_visualize(trie, output_path);
    EXPECT_EQ(result, 0);

    // Verify file exists
    std::ifstream ifs(output_path);
    EXPECT_TRUE(ifs.good());

    // Verify contains HTML structure
    std::string content = read_file(output_path);
    EXPECT_NE(content.find("<!DOCTYPE html>"), std::string::npos);
    EXPECT_NE(content.find("<title>HBTrie Visualizer</title>"), std::string::npos);

    // Clean up (DISABLED for testing)
    // unlink(output_path);
}

TEST_F(HbtrieVizTest, SingleNode) {
    // Create simple trie with one entry
    path_t* path = make_path({"test"});
    identifier_t* value = make_value("value");

    hbtrie_insert(trie, path, value);

    const char* output_path = "/tmp/test_single.html";

    int result = hbtrie_visualize(trie, output_path);
    EXPECT_EQ(result, 0);

    // Verify file exists
    std::ifstream ifs(output_path);
    EXPECT_TRUE(ifs.good());

    // Verify contains JSON data
    std::string content = read_file(output_path);
    EXPECT_NE(content.find("\"chunk_size\":"), std::string::npos);
    EXPECT_NE(content.find("\"root\":"), std::string::npos);

    // Clean up
    path_destroy(path);
    identifier_destroy(value);
    // unlink(output_path);  // DISABLED for testing
}

TEST_F(HbtrieVizTest, DeepTree) {
    // Create deep trie with multiple levels
    for (int i = 0; i < 5; i++) {
        char path_str[32];
        sprintf(path_str, "level%d", i);
        char value_str[32];
        sprintf(value_str, "value%d", i);

        path_t* path = make_path({path_str});
        identifier_t* value = make_value(value_str);

        hbtrie_insert(trie, path, value);

        path_destroy(path);
        identifier_destroy(value);
    }

    const char* output_path = "/tmp/test_deep.html";

    int result = hbtrie_visualize(trie, output_path);
    EXPECT_EQ(result, 0);

    // Verify file exists and has reasonable size
    std::ifstream ifs(output_path, std::ios::binary | std::ios::ate);
    EXPECT_TRUE(ifs.good());
    std::streamsize size = ifs.tellg();
    EXPECT_GT(size, 10000);  // Should be at least 10KB (D3 + HTML + JSON)

    // Clean up (DISABLED for testing)
    // unlink(output_path);
}