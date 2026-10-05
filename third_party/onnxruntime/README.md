ONNX Runtime 1.24.4 C API headers (MIT License, Copyright (c) Microsoft Corporation), from
https://github.com/microsoft/onnxruntime/tree/v1.24.4/include/onnxruntime/core/session.
NodeLab loads onnxruntime.dll at run time (src/ml/Onnx.cpp); the DLL is downloaded with the AI
models (src/ml/Models.cpp) and must match these headers' ORT_API_VERSION or be newer.
