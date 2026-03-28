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
      "../../src"
    ],
    "dependencies": [
      "<!@(node -p \"require('node-addon-api').gyp\")"
    ],
    "cflags!": ["-fno-exceptions"],
    "cflags_cc!": ["-fno-exceptions"],
    "libraries": [
      "-L../../build",
      "-lwavedb"
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