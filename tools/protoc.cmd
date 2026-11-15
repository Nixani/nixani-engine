@echo off
rem protoc shim: libchiaki only needs protoc to turn takion.proto into a descriptor set.
rem Uses the protoc bundled in grpcio-tools (installed in .venv) instead of building protobuf via vcpkg.
"%~dp0..\.venv\Scripts\python.exe" -m grpc_tools.protoc %*
