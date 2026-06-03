{
  "targets": [{
    "target_name": "wavedb_c_lib",
    "type": "static_library",
    "sources": [
      "c_src/deps/hashmap/src/hashmap.c",
      "c_src/deps/libcbor/src/allocators.c",
      "c_src/deps/libcbor/src/cbor.c",
      "c_src/deps/libcbor/src/cbor/arrays.c",
      "c_src/deps/libcbor/src/cbor/bytestrings.c",
      "c_src/deps/libcbor/src/cbor/callbacks.c",
      "c_src/deps/libcbor/src/cbor/common.c",
      "c_src/deps/libcbor/src/cbor/encoding.c",
      "c_src/deps/libcbor/src/cbor/floats_ctrls.c",
      "c_src/deps/libcbor/src/cbor/ints.c",
      "c_src/deps/libcbor/src/cbor/maps.c",
      "c_src/deps/libcbor/src/cbor/serialization.c",
      "c_src/deps/libcbor/src/cbor/streaming.c",
      "c_src/deps/libcbor/src/cbor/strings.c",
      "c_src/deps/libcbor/src/cbor/tags.c",
      "c_src/deps/libcbor/src/cbor/internal/builder_callbacks.c",
      "c_src/deps/libcbor/src/cbor/internal/encoders.c",
      "c_src/deps/libcbor/src/cbor/internal/loaders.c",
      "c_src/deps/libcbor/src/cbor/internal/memory_utils.c",
      "c_src/deps/libcbor/src/cbor/internal/stack.c",
      "c_src/deps/libcbor/src/cbor/internal/unicode.c",
      "c_src/deps/xxhash/xxhash.c",
      "c_src/deps/xxhash/xxh_x86dispatch.c",
      "c_src/src/Buffer/buffer.c",
      "c_src/src/Database/batch.c",
      "c_src/src/Database/database.c",
      "c_src/src/Database/database_config.c",
      "c_src/src/Database/database_iterator.c",
      "c_src/src/Database/database_lru.c",
      "c_src/src/Database/eviction_queue.c",
      "c_src/src/Database/wal.c",
      "c_src/src/Database/wal_compactor.c",
      "c_src/src/Database/wal_manager.c",
      "c_src/src/HBTrie/bnode.c",
      "c_src/src/HBTrie/bs_array.c",
      "c_src/src/HBTrie/chunk.c",
      "c_src/src/HBTrie/hbtrie.c",
      "c_src/src/HBTrie/hbtrie_viz.c",
      "c_src/src/HBTrie/identifier.c",
      "c_src/src/HBTrie/mvcc.c",
      "c_src/src/HBTrie/path.c",
      "c_src/src/Layers/graph/graph.c",
      "c_src/src/Layers/graph/graph_ops.c",
      "c_src/src/Layers/graph/graph_optimizer.c",
      "c_src/src/Layers/graph/graph_parser.c",
      "c_src/src/Layers/graph/graph_schema_parser.c",
      "c_src/src/Layers/graph/graph_set.c",
      "c_src/src/Layers/graph/graph_stats.c",
      "c_src/src/Layers/graphql/graphql_lexer.c",
      "c_src/src/Layers/graphql/graphql_parser.c",
      "c_src/src/Layers/graphql/graphql_plan.c",
      "c_src/src/Layers/graphql/graphql_resolve.c",
      "c_src/src/Layers/graphql/graphql_result.c",
      "c_src/src/Layers/graphql/graphql_schema.c",
      "c_src/src/Layers/graphql/graphql_types.c",
      "c_src/src/RefCounter/refcounter.c",
      "c_src/src/Storage/bnode_cache.c",
      "c_src/src/Storage/encryption.c",
      "c_src/src/Storage/node_serializer.c",
      "c_src/src/Storage/page_file.c",
      "c_src/src/Storage/stale_region.c",
      "c_src/src/Time/debouncer.c",
      "c_src/src/Time/ticker.c",
      "c_src/src/Time/wheel.c",
      "c_src/src/Util/allocator.c",
      "c_src/src/Util/fs_block_size.c",
      "c_src/src/Util/get_dir.c",
      "c_src/src/Util/hash.c",
      "c_src/src/Util/log.c",
      "c_src/src/Util/memory_pool.c",
      "c_src/src/Util/mkdir_p.c",
      "c_src/src/Util/path_join.c",
      "c_src/src/Util/perf_counters.c",
      "c_src/src/Util/rm_rf.c",
      "c_src/src/Util/threadding.c",
      "c_src/src/Util/vec.c",
      "c_src/src/Workers/error.c",
      "c_src/src/Workers/join.c",
      "c_src/src/Workers/pool.c",
      "c_src/src/Workers/promise.c",
      "c_src/src/Workers/queue.c",
      "c_src/src/Workers/transaction_id.c",
      "c_src/src/Workers/work.c"
    ],
    "include_dirs": [
      "c_src/src",
      "c_src/deps/libcbor/src",
      "c_src/deps/hashmap/include",
      "c_src/deps/xxhash"
    ],
    "defines": [
      "CBOR_STATIC_DEFINE"
    ],
    "cflags": ["-O3"],
    "conditions": [
      ["OS=='win'", {
        "defines": [
          "CBOR_STATIC_DEFINE",
          "CBOR_RESTRICT_SPECIFIER=",
          "WIN32_LEAN_AND_MEAN",
          "_WINSOCKAPI_"
        ],
        "msvs_settings": {
          "VCCLCompilerTool": {
            "AdditionalOptions": ["/O2"]
          }
        }
      }]
    ]
  }, {
    "target_name": "wavedb",
    "sources": [
      "src/binding.cpp",
      "src/database.cc",
      "src/path.cc",
      "src/identifier.cc",
      "src/async_bridge.cc",
      "src/iterator.cc",
      "src/graphql_result_js.cc",
      "src/graph_result_js.cc"
    ],
    "include_dirs": [
      "<!@(node -p \"require('node-addon-api').include.replace(/\\\\/g, '/')\")",
      "c_src/src",
      "c_src/deps/libcbor/src",
      "c_src/deps/hashmap/include",
      "c_src/deps/xxhash"
    ],
    "dependencies": [
      "<!@(node -p \"require('node-addon-api').gyp.replace(/\\\\/g, '/')\")",
      "wavedb_c_lib"
    ],
    "defines": [
    ],
    "cflags!": ["-fno-exceptions"],
    "cflags": ["-O3"],
    "cflags_cc!": ["-fno-exceptions"],
    "cflags_cc": ["-O3"],
    "conditions": [
      ["OS=='linux'", {
        "libraries": [
          "-lpthread", "-latomic", "-lcrypto", "-lssl"
        ]
      }],
      ["OS=='mac'", {
        "xcode_settings": {
          "GCC_ENABLE_CPP_EXCEPTIONS": "YES",
          "MACOSX_DEPLOYMENT_TARGET": "10.7"
        },
        "libraries": [
          "-lcrypto", "-lssl"
        ]
      }],
      ["OS=='win'", {
        "defines": [
          "WIN32_LEAN_AND_MEAN",
          "_WINSOCKAPI_"
        ],
        "libraries": [
          "ws2_32.lib",
          "bcrypt.lib"
        ],
        "msvs_settings": {
          "VCCLCompilerTool": {
            "ExceptionHandling": "1",
            "AdditionalOptions": ["/O2"]
          }
        }
      }]
    ]
  }, {
    "target_name": "graphql",
    "sources": [
      "src/graphql_layer.cc",
      "src/graphql_result_js.cc",
      "src/async_bridge.cc",
      "src/identifier.cc",
      "src/graph_result_js.cc"
    ],
    "include_dirs": [
      "<!@(node -p \"require('node-addon-api').include.replace(/\\\\/g, '/')\")",
      "c_src/src",
      "c_src/deps/libcbor/src",
      "c_src/deps/hashmap/include",
      "c_src/deps/xxhash"
    ],
    "dependencies": [
      "<!@(node -p \"require('node-addon-api').gyp.replace(/\\\\/g, '/')\")",
      "wavedb_c_lib"
    ],
    "defines": [
    ],
    "cflags!": ["-fno-exceptions"],
    "cflags": ["-O3"],
    "cflags_cc!": ["-fno-exceptions"],
    "cflags_cc": ["-O3"],
    "conditions": [
      ["OS=='linux'", {
        "libraries": [
          "-lpthread", "-latomic", "-lcrypto", "-lssl"
        ]
      }],
      ["OS=='mac'", {
        "xcode_settings": {
          "GCC_ENABLE_CPP_EXCEPTIONS": "YES",
          "MACOSX_DEPLOYMENT_TARGET": "10.7"
        },
        "libraries": [
          "-lcrypto", "-lssl"
        ]
      }],
      ["OS=='win'", {
        "defines": [
          "WIN32_LEAN_AND_MEAN",
          "_WINSOCKAPI_"
        ],
        "libraries": [
          "ws2_32.lib",
          "bcrypt.lib"
        ],
        "msvs_settings": {
          "VCCLCompilerTool": {
            "ExceptionHandling": "1",
            "AdditionalOptions": ["/O2"]
          }
        }
      }]
    ]
  }, {
    "target_name": "graph",
    "sources": [
      "src/graph_layer.cc",
      "src/graph_result_js.cc",
      "src/async_bridge.cc",
      "src/identifier.cc",
      "src/graphql_result_js.cc"
    ],
    "include_dirs": [
      "<!@(node -p \"require('node-addon-api').include.replace(/\\\\/g, '/')\")",
      "c_src/src",
      "c_src/deps/libcbor/src",
      "c_src/deps/hashmap/include",
      "c_src/deps/xxhash"
    ],
    "dependencies": [
      "<!@(node -p \"require('node-addon-api').gyp.replace(/\\\\/g, '/')\")",
      "wavedb_c_lib"
    ],
    "cflags!": ["-fno-exceptions"],
    "cflags": ["-O3"],
    "cflags_cc!": ["-fno-exceptions"],
    "cflags_cc": ["-O3"],
    "conditions": [
      ["OS=='linux'", {
        "libraries": [
          "-lpthread", "-latomic", "-lcrypto", "-lssl"
        ]
      }],
      ["OS=='mac'", {
        "xcode_settings": {
          "GCC_ENABLE_CPP_EXCEPTIONS": "YES",
          "MACOSX_DEPLOYMENT_TARGET": "10.7"
        },
        "libraries": [
          "-lcrypto", "-lssl"
        ]
      }],
      ["OS=='win'", {
        "defines": [
          "WIN32_LEAN_AND_MEAN",
          "_WINSOCKAPI_"
        ],
        "libraries": [
          "ws2_32.lib",
          "bcrypt.lib"
        ],
        "msvs_settings": {
          "VCCLCompilerTool": {
            "ExceptionHandling": "1",
            "AdditionalOptions": ["/O2"]
          }
        }
      }]
    ]
  }]
}