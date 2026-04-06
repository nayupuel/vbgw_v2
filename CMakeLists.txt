cmake_minimum_required(VERSION 3.15)
project(VoicebotGateway VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 1. 의존성 패키지 찾기
find_package(PkgConfig REQUIRED)

# PJSIP 찾기
pkg_check_modules(PJPROJECT REQUIRED libpjproject)

# Boost, gRPC, Protobuf, OpenSSL 찾기
find_package(Boost REQUIRED)
find_package(gRPC CONFIG REQUIRED)
find_package(Protobuf CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_library(ONNXRUNTIME_LIB 
    NAMES onnxruntime 
    PATHS /opt/homebrew/lib /usr/local/lib /usr/lib /opt/onnxruntime/lib
    REQUIRED)

# ONNX Runtime 헤더 탐색 추가
find_path(ONNXRUNTIME_INCLUDE_DIR 
    NAMES onnxruntime_cxx_api.h 
    PATHS /opt/homebrew/include/onnxruntime /usr/local/include/onnxruntime /usr/include/onnxruntime
    REQUIRED)

find_package(spdlog REQUIRED)
find_library(SPEEXDSP_LIB
    NAMES speexdsp
    PATHS /opt/homebrew/lib /usr/local/lib /usr/lib
    REQUIRED)

find_program(PROTOC_EXECUTABLE protoc)
find_program(GRPC_CPP_PLUGIN_EXECUTABLE grpc_cpp_plugin)

set(PROTO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/protos)
set(PROTO_SRC ${PROTO_DIR}/voicebot.proto)

set(PROTO_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${PROTO_GEN_DIR})

set(PROTO_HDRS ${PROTO_GEN_DIR}/voicebot.pb.h ${PROTO_GEN_DIR}/voicebot.grpc.pb.h)
set(PROTO_SRCS ${PROTO_GEN_DIR}/voicebot.pb.cc ${PROTO_GEN_DIR}/voicebot.grpc.pb.cc)

add_custom_command(
    OUTPUT ${PROTO_SRCS} ${PROTO_HDRS}
    COMMAND ${PROTOC_EXECUTABLE}
    ARGS --grpc_out=${PROTO_GEN_DIR}
         --cpp_out=${PROTO_GEN_DIR}
         -I ${PROTO_DIR}
         --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN_EXECUTABLE}
         ${PROTO_SRC}
    DEPENDS ${PROTO_SRC}
)

# 2. 정적 라이브러리 (테스트 및 메인 실행파일 공유)
add_library(vbgw_lib STATIC
    src/engine/VoicebotEndpoint.cpp
    src/engine/VoicebotAccount.cpp
    src/engine/VoicebotCall.cpp
    src/engine/VoicebotMediaPort.cpp
    src/ai/VoicebotAiClient.cpp
    src/ai/SileroVad.cpp
    src/ai/SpeexDsp.cpp
    src/ivr/IvrManager.cpp
    src/api/HttpServer.cpp
    ${PROTO_SRCS}
)

add_executable(vbgw src/main.cpp)
target_link_libraries(vbgw PRIVATE vbgw_lib)

# 3. 헤더 디렉토리 포함
target_include_directories(vbgw_lib PUBLIC 
    ${PJPROJECT_INCLUDE_DIRS}
    src
    ${PROTO_GEN_DIR}
    ${ONNXRUNTIME_INCLUDE_DIR}
)

# OpenSSL 라이브러리 경로 추출 (macOS 등 비표준 경로 대비)
get_filename_component(OPENSSL_LIB_DIR ${OPENSSL_SSL_LIBRARY} DIRECTORY)

# 4. 컴파일 옵션 및 라이브러리 링크
target_compile_options(vbgw_lib PUBLIC ${PJPROJECT_CFLAGS_OTHER})
target_link_directories(vbgw_lib PUBLIC ${PJPROJECT_LIBRARY_DIRS} ${OPENSSL_LIB_DIR})

execute_process(COMMAND pkg-config --static --libs libpjproject
                OUTPUT_VARIABLE PJ_RAW_LDFLAGS
                OUTPUT_STRIP_TRAILING_WHITESPACE)
set_target_properties(vbgw_lib PROPERTIES LINK_FLAGS "${PJ_RAW_LDFLAGS}")
set_target_properties(vbgw PROPERTIES LINK_FLAGS "${PJ_RAW_LDFLAGS}")

target_link_libraries(vbgw_lib PUBLIC 
    ${PJPROJECT_LIBRARIES}
    Boost::headers
    gRPC::grpc++
    protobuf::libprotobuf
    OpenSSL::SSL
    OpenSSL::Crypto
    ${ONNXRUNTIME_LIB}
    ${SPEEXDSP_LIB}
    spdlog::spdlog
)

# ------------------------------------------------------------------------------
# 5. Testing Framework (GoogleTest) [M-4]
# ------------------------------------------------------------------------------
include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/03597a01ee50ed33e9dfd640b249b4be3799d395.zip
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

enable_testing()
add_executable(test_vbgw
    tests/test_ringbuffer.cpp
    tests/test_runtime_metrics.cpp
)
target_link_libraries(test_vbgw PRIVATE vbgw_lib GTest::gtest_main)
include(GoogleTest)
gtest_discover_tests(test_vbgw)
