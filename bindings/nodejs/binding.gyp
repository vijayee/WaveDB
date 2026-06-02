{
  "variables": {
    "enable_lto": "false",
    "enable_thin_lto": "false",
    "lto_jobs": ""
  },
  "targets": [{
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
      "../../src",
      "../../deps/libcbor/src",
      "../../build/deps/libcbor/src",
      "../../build/deps/libcbor",
      "../../deps/hashmap/include",
      "../../deps/xxhash"
    ],
    "dependencies": [
      "<!@(node -p \"require('node-addon-api').gyp.replace(/\\\\/g, '/')\")"
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
          "../../../build/libwavedb.a",
          "../../../build/libxxhash.a",
          "../../../build/libhashmap.a",
          "../../../build/deps/libcbor/src/libcbor.a",
          "-lpthread", "-latomic", "-lcrypto", "-lssl"
        ]
      }],
      ["OS=='mac'", {
        "xcode_settings": {
          "GCC_ENABLE_CPP_EXCEPTIONS": "YES",
          "MACOSX_DEPLOYMENT_TARGET": "10.7"
        },
        "libraries": [
          "../../../build/libwavedb.a",
          "../../../build/libxxhash.a",
          "../../../build/libhashmap.a",
          "../../../build/deps/libcbor/src/libcbor.a",
          "-lcrypto", "-lssl"
        ]
      }],
      ["OS=='win'", {
        "libraries": [
          "wavedb_core.lib",
          "wavedb_cbor.lib",
          "wavedb_xxhash.lib",
          "wavedb_hashmap.lib",
          "ws2_32.lib",
          "bcrypt.lib"
        ],
        "msvs_settings": {
          "VCCLCompilerTool": {
            "ExceptionHandling": "1",
            "AdditionalOptions": ["/O2"]
          },
          "VCLinkerTool": {
            "AdditionalLibraryDirectories": ["Release"]
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
      "../../src",
      "../../deps/libcbor/src",
      "../../build/deps/libcbor/src",
      "../../build/deps/libcbor",
      "../../deps/hashmap/include",
      "../../deps/xxhash"
    ],
    "dependencies": [
      "<!@(node -p \"require('node-addon-api').gyp.replace(/\\\\/g, '/')\")"
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
          "../../../build/libwavedb.a",
          "../../../build/libxxhash.a",
          "../../../build/libhashmap.a",
          "../../../build/deps/libcbor/src/libcbor.a",
          "-lpthread", "-latomic", "-lcrypto", "-lssl"
        ]
      }],
      ["OS=='mac'", {
        "xcode_settings": {
          "GCC_ENABLE_CPP_EXCEPTIONS": "YES",
          "MACOSX_DEPLOYMENT_TARGET": "10.7"
        },
        "libraries": [
          "../../../build/libwavedb.a",
          "../../../build/libxxhash.a",
          "../../../build/libhashmap.a",
          "../../../build/deps/libcbor/src/libcbor.a",
          "-lcrypto", "-lssl"
        ]
      }],
      ["OS=='win'", {
        "libraries": [
          "wavedb_core.lib",
          "wavedb_cbor.lib",
          "wavedb_xxhash.lib",
          "wavedb_hashmap.lib",
          "ws2_32.lib",
          "bcrypt.lib"
        ],
        "msvs_settings": {
          "VCCLCompilerTool": {
            "ExceptionHandling": "1",
            "AdditionalOptions": ["/O2"]
          },
          "VCLinkerTool": {
            "AdditionalLibraryDirectories": ["Release"]
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
      "../../src",
      "../../deps/libcbor/src",
      "../../build/deps/libcbor/src",
      "../../build/deps/libcbor",
      "../../deps/hashmap/include",
      "../../deps/xxhash"
    ],
    "dependencies": [
      "<!@(node -p \"require('node-addon-api').gyp.replace(/\\\\/g, '/')\")"
    ],
    "cflags!": ["-fno-exceptions"],
    "cflags": ["-O3"],
    "cflags_cc!": ["-fno-exceptions"],
    "cflags_cc": ["-O3"],
    "conditions": [
      ["OS=='linux'", {
        "libraries": [
          "../../../build/libwavedb.a",
          "../../../build/libxxhash.a",
          "../../../build/libhashmap.a",
          "../../../build/deps/libcbor/src/libcbor.a",
          "-lpthread", "-latomic", "-lcrypto", "-lssl"
        ]
      }],
      ["OS=='mac'", {
        "xcode_settings": {
          "GCC_ENABLE_CPP_EXCEPTIONS": "YES",
          "MACOSX_DEPLOYMENT_TARGET": "10.7"
        },
        "libraries": [
          "../../../build/libwavedb.a",
          "../../../build/libxxhash.a",
          "../../../build/libhashmap.a",
          "../../../build/deps/libcbor/src/libcbor.a",
          "-lcrypto", "-lssl"
        ]
      }],
      ["OS=='win'", {
        "libraries": [
          "wavedb_core.lib",
          "wavedb_cbor.lib",
          "wavedb_xxhash.lib",
          "wavedb_hashmap.lib",
          "ws2_32.lib",
          "bcrypt.lib"
        ],
        "msvs_settings": {
          "VCCLCompilerTool": {
            "ExceptionHandling": "1",
            "AdditionalOptions": ["/O2"]
          },
          "VCLinkerTool": {
            "AdditionalLibraryDirectories": ["Release"]
          }
        }
      }]
    ]
  }]
}