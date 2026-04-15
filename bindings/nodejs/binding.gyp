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
      "../../build-release/deps/libcbor/src",
      "../../build-release/deps/libcbor",
      "../../deps/hashmap/include",
      "../../deps/xxhash"
    ],
    "dependencies": [
      "<!@(node -p \"require('node-addon-api').gyp\")"
    ],
    "defines": [
      "REFCOUNTER_ATOMIC"
    ],
    "cflags!": ["-fno-exceptions"],
    "cflags": ["-O3"],
    "cflags_cc!": ["-fno-exceptions"],
    "cflags_cc": ["-O3"],
    "ldflags": [""],
    "libraries": [
      "/home/victor/Workspace/src/github.com/vijayee/WaveDB/build-release/libwavedb.a",
      "/home/victor/Workspace/src/github.com/vijayee/WaveDB/build-release/libxxhash.a",
      "/home/victor/Workspace/src/github.com/vijayee/WaveDB/build-release/libhashmap.a",
      "/home/victor/Workspace/src/github.com/vijayee/WaveDB/build-release/deps/libcbor/src/libcbor.a",
      "-lpthread",
      "-latomic"
    ],
    "conditions": [
      ["OS=='linux'", {
        "libraries": ["-lpthread"]
      }],
      ["OS=='mac'", {
        "xcode_settings": {
          "GCC_ENABLE_CPP_EXCEPTIONS": "YES",
          "MACOSX_DEPLOYMENT_TARGET": "10.7"
        }
      }],
      ["OS=='win'", {
        "libraries": ["ws2_32.lib"]
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
      "../../build-release/deps/libcbor/src",
      "../../build-release/deps/libcbor",
      "../../deps/hashmap/include",
      "../../deps/xxhash"
    ],
    "dependencies": [
      "<!@(node -p \"require('node-addon-api').gyp\")"
    ],
    "defines": [
      "REFCOUNTER_ATOMIC"
    ],
    "cflags!": ["-fno-exceptions"],
    "cflags": ["-O3"],
    "cflags_cc!": ["-fno-exceptions"],
    "cflags_cc": ["-O3"],
    "ldflags": [""],
    "libraries": [
      "/home/victor/Workspace/src/github.com/vijayee/WaveDB/build-release/libwavedb.a",
      "/home/victor/Workspace/src/github.com/vijayee/WaveDB/build-release/libxxhash.a",
      "/home/victor/Workspace/src/github.com/vijayee/WaveDB/build-release/libhashmap.a",
      "/home/victor/Workspace/src/github.com/vijayee/WaveDB/build-release/deps/libcbor/src/libcbor.a",
      "-lpthread",
      "-latomic"
    ],
    "conditions": [
      ["OS=='linux'", {
        "libraries": ["-lpthread"]
      }],
      ["OS=='mac'", {
        "xcode_settings": {
          "GCC_ENABLE_CPP_EXCEPTIONS": "YES",
          "MACOSX_DEPLOYMENT_TARGET": "10.7"
        }
      }],
      ["OS=='win'", {
        "libraries": ["ws2_32.lib"]
      }]
    ]
  }]
}