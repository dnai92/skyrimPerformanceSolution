cmake_minimum_required(VERSION 3.25)

project(
	SkyrimPerf
	VERSION 0.9.6
	LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# CommonLibSSE-NG als Submodul; nur SE/AE, keine Tests
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_VR OFF CACHE BOOL "" FORCE)
add_subdirectory(extern/CommonLibSSE-NG EXCLUDE_FROM_ALL)
include(extern/CommonLibSSE-NG/cmake/CommonLibSSE.cmake)  # add_commonlibsse_plugin() - beim Source-Build nicht automatisch geladen

add_commonlibsse_plugin(
	${PROJECT_NAME}
	NAME "SkyrimPerf"
	AUTHOR "dnai92"
	USE_ADDRESS_LIBRARY
	SOURCES
		src/main.cpp
		src/Hooks.cpp
		src/Hotkey.cpp
		src/InstancingAnalysis.cpp
		src/Config.cpp
		src/DetourHelper.cpp
		src/WaitProbe.cpp
		src/ShadowCulling.cpp
		src/Stats.cpp
		extern/tracy/public/TracyClient.cpp
)

# Tracy direkt aus dem Submodul (v0.14.1 - Profiler-GUI muss dieselbe Version haben).
# ON_DEMAND: kein Overhead/Speicher, solange kein Profiler verbunden ist.
# NO_CRASH_HANDLER: Crash Logger nicht in die Quere kommen.
target_include_directories(${PROJECT_NAME} PRIVATE src extern/tracy/public)
target_compile_definitions(${PROJECT_NAME} PRIVATE TRACY_ENABLE TRACY_ON_DEMAND TRACY_NO_CRASH_HANDLER NOMINMAX)
target_link_libraries(${PROJECT_NAME} PRIVATE ws2_32 dbghelp)
target_precompile_headers(${PROJECT_NAME} PRIVATE src/PCH.h)
# Microsoft Detours (wie Community Shaders) - fuer Funktionen, die CS bereits per Detours umleitet
find_path(DETOURS_INCLUDE_DIRS "detours/detours.h" REQUIRED)
find_library(DETOURS_LIBRARY detours REQUIRED)
target_include_directories(${PROJECT_NAME} PRIVATE ${DETOURS_INCLUDE_DIRS})
target_link_libraries(${PROJECT_NAME} PRIVATE ${DETOURS_LIBRARY})
set_source_files_properties(src/DetourHelper.cpp src/WaitProbe.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

set_source_files_properties(extern/tracy/public/TracyClient.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON COMPILE_OPTIONS "/W0")
target_compile_options(${PROJECT_NAME} PRIVATE /W4 /permissive- /utf-8)

# DLL verweist nur auf den PDB-Dateinamen (liegt neben der DLL) -> laufendes Skyrim sperrt nicht die Build-PDB
target_link_options(${PROJECT_NAME} PRIVATE "/PDBALTPATH:%_PDB%")
