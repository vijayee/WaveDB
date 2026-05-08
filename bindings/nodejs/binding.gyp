{
  "targets": [{
    "target_name": "wavedb",
    "sources": [
      "src/binding.cpp",
      "src/database.cc",
      "src/path.cc",
      "src/identifier.cc",
      "src/async_bridge.cc",
      "src/iterator.cc"
    ],
    "include_dirs": [
      "<!@(node -p \"require('node-addon-api').include\")",
      "../../src",
      "../../deps/libcbor/src",
      "../../build/deps/libcbor/src",
      "../../deps/hashmap/include",
      "../../deps/xxhash"
    ],
    "dependencies": [
      "<!@(node -p \"require('node-addon-api').gyp\")"
    ],
    "defines": [
    ],
    "cflags!": ["-fno-exceptions"],
    "cflags": ["-O3"],
    "cflags_cc!": ["-fno-exceptions"],
    "cflags_cc": ["-O3"],
    "ldflags": [""],
    "libraries": [
      "../../build/libwavedb.a",
      "../../build/libxxhash.a",
      "../../build/libhashmap.a",
      "../../build/deps/libcbor/src/libcbor.a"
    ],
    "conditions": [
      ["OS=='linux'", {
        "libraries": ["-lpthread", "-latomic", "-lcrypto", "-lssl"]
      }],
      ["OS=='mac'", {
        "xcode_settings": {
          "GCC_ENABLE_CPP_EXCEPTIONS": "YES",
          "MACOSX_DEPLOYMENT_TARGET": "10.7"
        },
        "libraries": ["-lcrypto", "-lssl"]
      }],
      ["OS=='win'", {
        "libraries": [
          "../../build/Release/wavedb.lib",
          "../../build/Release/xxhash.lib",
          "../../build/Release/hashmap.lib",
          "../../build/deps/libcbor/src/Release/cbor.lib",
          "ws2_32.lib",
          "bcrypt.lib"
        ],
        "msvs_settings": {
          "VCCLCompilerTool": {
            "AdditionalOptions": ["/std:c11"]
          }
        }
      }]
    ]
  }, {
    "target_name": "graphql",
    "sources": [
      "src/graphql_layer.cc",
      "src/graphql_result_js.cc",
      "src/async_bridge.cc"
    ],
    "include_dirs": [
      "<!@(node -p \"require('node-addon-api').include\")",
      "../../src",
      "../../deps/libcbor/src",
      "../../build/deps/libcbor/src",
      "../../deps/hashmap/include",
      "../../deps/xxhash"
    ],
    "dependencies": [
      "<!@(node -p \"require('node-addon-api').gyp\")"
    ],
    "defines": [
    ],
    "cflags!": ["-fno-exceptions"],
    "cflags": ["-O3"],
    "cflags_cc!": ["-fno-exceptions"],
    "cflags_cc": ["-O3"],
    "ldflags": [""],
    "libraries": [
      "../../build/libwavedb.a",
      "../../build/libxxhash.a",
      "../../build/libhashmap.a",
      "../../build/deps/libcbor/src/libcbor.a"
    ],
    "conditions": [
      ["OS=='linux'", {
        "libraries": ["-lpthread", "-latomic", "-lcrypto", "-lssl"]
      }],
      ["OS=='mac'", {
        "xcode_settings": {
          "GCC_ENABLE_CPP_EXCEPTIONS": "YES",
          "MACOSX_DEPLOYMENT_TARGET": "10.7"
        },
        "libraries": ["-lcrypto", "-lssl"]
      }],
      ["OS=='win'", {
        "libraries": [
          "../../build/Release/wavedb.lib",
          "../../build/Release/xxhash.lib",
          "../../build/Release/hashmap.lib",
          "../../build/deps/libcbor/src/Release/cbor.lib",
          "ws2_32.lib",
          "bcrypt.lib"
        ],
        "msvs_settings": {
          "VCCLCompilerTool": {
            "AdditionalOptions": ["/std:c11"]
          }
        }
      }]
    ]
  }]
}