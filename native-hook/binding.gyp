{
  "targets": [
    {
      "target_name": "equihdr_hook",
      "sources": [
        "hook.cpp",
        "minhook/src/buffer.c",
        "minhook/src/hook.c",
        "minhook/src/trampoline.c",
        "minhook/src/hde/hde64.c"
      ],
      "include_dirs": [
        "<!@(node -p \"require('node-addon-api').include\")",
        "minhook/include",
        "minhook/src",
        "minhook/src/hde"
      ],
      "defines": [
        "NAPI_CPP_EXCEPTIONS",
        "WIN32_LEAN_AND_MEAN",
        "NOMINMAX"
      ],
      "cflags_cc!": ["-fno-exceptions"],
      "msvs_settings": {
        "VCCLCompilerTool": {
          "ExceptionHandling": 1,
          "AdditionalOptions": ["/std:c++20"]
        }
      },
      "libraries": [
        "d3d11.lib",
        "dxgi.lib",
        "d3dcompiler.lib",
        "uuid.lib"
      ]
    }
  ]
}
