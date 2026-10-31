cmake_minimum_required(VERSION 3.24)

project(KillCamDirector VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# CommonLibSSE-NG (alandtse fork, SE + AE + VR runtimes so the published prebuilt can be used)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_SE ON CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_AE ON CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_VR ON CACHE BOOL "" FORCE)

include(FetchContent)
FetchContent_Declare(
	CommonLibSSE
	GIT_REPOSITORY https://github.com/alandtse/CommonLibVR.git
	GIT_TAG v14.0.1
	GIT_SHALLOW TRUE
	GIT_SUBMODULES "extern/openvr"
)
FetchContent_MakeAvailable(CommonLibSSE)
get_target_property(COMMONLIB_SRC_DIR CommonLibSSE SOURCE_DIR)
if(NOT COMMONLIB_SRC_DIR OR NOT EXISTS "${COMMONLIB_SRC_DIR}/cmake/CommonLibSSE.cmake")
	set(COMMONLIB_SRC_DIR "${commonlibsse_SOURCE_DIR}")
endif()
include("${COMMONLIB_SRC_DIR}/cmake/CommonLibSSE.cmake")

find_package(minhook CONFIG REQUIRED)
find_path(SIMPLEINI_INCLUDE_DIRS "SimpleIni.h")

set(SOURCES
	src/main.cpp
	src/Settings.cpp
	src/Director.cpp
	src/Input.cpp
	src/UI.cpp
)

add_commonlibsse_plugin(
	${PROJECT_NAME}
	NAME "KillCamDirector"
	AUTHOR "KillCamDirector"
	SOURCES ${SOURCES}
)

target_include_directories(${PROJECT_NAME} PRIVATE src include ${SIMPLEINI_INCLUDE_DIRS})
target_compile_definitions(${PROJECT_NAME} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_link_libraries(${PROJECT_NAME} PRIVATE minhook::minhook)
target_precompile_headers(${PROJECT_NAME} PRIVATE src/PCH.h)

if(MSVC)
	target_compile_options(${PROJECT_NAME} PRIVATE /utf-8 /Zc:preprocessor /EHsc /permissive-)
endif()


# ---------------------------------------------------------------- Execution Director
find_package(nlohmann_json CONFIG REQUIRED)

add_commonlibsse_plugin(
	ExecutionDirector
	NAME "ExecutionDirector"
	AUTHOR "KillCamDirector"
	SOURCES
		execution/src/main.cpp
		execution/src/Settings.cpp
		execution/src/Finishers.cpp
		execution/src/Executor.cpp
		execution/src/Input.cpp
		execution/src/UI.cpp
)
target_include_directories(ExecutionDirector PRIVATE execution/src include ${SIMPLEINI_INCLUDE_DIRS})
target_compile_definitions(ExecutionDirector PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_link_libraries(ExecutionDirector PRIVATE minhook::minhook nlohmann_json::nlohmann_json)
target_precompile_headers(ExecutionDirector PRIVATE execution/src/PCH.h)
if(MSVC)
	target_compile_options(ExecutionDirector PRIVATE /utf-8 /Zc:preprocessor /EHsc /permissive-)
endif()
