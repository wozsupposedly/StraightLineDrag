{
  "targets": [
    {
      "target_name": "straight_drag",
      "sources": [
        "native/straight_drag.cc"
      ],
      "include_dirs": [
        "<!@(node -p \"require('node-addon-api').include\")"
      ],
      "dependencies": [
        "<!(node -p \"require('node-addon-api').gyp\")"
      ],
      "defines": [
        "NAPI_CPP_EXCEPTIONS"
      ],
      "cflags_cc": [
        "-std=c++17"
      ],
      "conditions": [
        [
          "OS==\"mac\"",
          {
            "sources!": [
              "native/straight_drag.cc"
            ],
            "sources": [
              "native/straight_drag.mm"
            ],
            "xcode_settings": {
              "CLANG_CXX_LANGUAGE_STANDARD": "c++17",
              "GCC_ENABLE_CPP_EXCEPTIONS": "YES"
            },
            "libraries": [
              "-framework ApplicationServices",
              "-framework CoreFoundation",
              "-framework AppKit"
            ]
          }
        ],
        [
          "OS==\"win\"",
          {
            "msvs_settings": {
              "VCCLCompilerTool": {
                "ExceptionHandling": 1,
                "AdditionalOptions": [
                  "/std:c++17"
                ]
              }
            },
            "libraries": [
              "user32.lib"
            ]
          }
        ]
      ]
    }
  ]
}
