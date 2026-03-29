{
  "targets": [{
    "target_name": "wavedb",
    "sources": [
      "src/binding.cpp",
      "src/database.cc",
      "src/path.cc",
      "src/identifier.cc",
      "src/async_worker.cc",
      "src/put_worker.cc",
      "src/get_worker.cc",
      "src/del_worker.cc",
      "src/batch_worker.cc",
      "src/iterator.cc"
    ],
    "include_dirs": [
      "<!@(node -p \"require('node-addon-api').include\")",
      "../../src",
      "../../deps/libcbor/src",
      "../../build/deps/libcbor/src",
      "../../build/deps/libcbor",
      "../../deps/hashmap/include",
      "../../deps/xxhash"
    ],
    "dependencies": [
      "<!@(node -p \"require('node-addon-api').gyp\")"
    ],
    "cflags!": ["-fno-exceptions"],
    "cflags_cc!": ["-fno-exceptions"],
    "libraries": [
      "/home/victor/Workspace/src/github.com/vijayee/WaveDB/build/libwavedb.a",
      "/home/victor/Workspace/src/github.com/vijayee/WaveDB/build/libxxhash.a",
      "/home/victor/Workspace/src/github.com/vijayee/WaveDB/build/libhashmap.a",
      "/home/victor/Workspace/src/github.com/vijayee/WaveDB/build/deps/libcbor/src/libcbor.a",
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