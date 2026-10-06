cmake_minimum_required(VERSION 3.25)

project(
	SPS
	VERSION 1.0.4
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
	NAME "SPS"
	AUTHOR "dnai92"
	USE_ADDRESS_LIBRARY
	SOURCES
		src/main.cpp
		src/Features.cpp
		src/Hooks.cpp
		src/Hotkey.cpp
		src/InstancingAnalysis.cpp
		src/LightGather.cpp
		src/EngineTimers.cpp
		src/TextureStream.cpp
		src/Menu.cpp
		src/Config.cpp
		src/DetourHelper.cpp
		src/GpuMemory.cpp
		src/WaitProbe.cpp
		src/InstancedDraw.cpp
		src/ShadowCulling.cpp
		src/Stats.cpp
		extern/tracy/public/TracyClient.cpp
)

# Tracy direkt aus dem Submodul (v0.14.1 - Profiler-GUI muss dieselbe Version haben).
# ON_DEMAND: kein Overhead/Speicher, solange kein Profiler verbunden ist.
# NO_CRASH_HANDLER: Crash Logger nicht in die Quere kommen.
target_include_directories(${PROJECT_NAME} PRIVATE src extern/tracy/public)
# Nur fuer Entwickler-Builds (-DSPS_TRACY=ON): Tracy oeffnet einen Netzwerk-Port und sendet Broadcasts ins lokale Netz
# (Firewall-Abfrage bei Nutzern, 1.0.3). Ohne TRACY_ENABLE sind alle Tracy-Makros leer.
option(SPS_TRACY "Tracy-Profiler einbauen (Entwicklung)" OFF)
target_compile_definitions(${PROJECT_NAME} PRIVATE TRACY_ON_DEMAND TRACY_NO_CRASH_HANDLER NOMINMAX)
if(SPS_TRACY)
	target_compile_definitions(${PROJECT_NAME} PRIVATE TRACY_ENABLE)
endif()
target_link_libraries(${PROJECT_NAME} PRIVATE ws2_32 dbghelp)
target_precompile_headers(${PROJECT_NAME} PRIVATE src/PCH.h)
# Microsoft Detours (wie Community Shaders) - fuer Funktionen, die CS bereits per Detours umleitet
find_path(DETOURS_INCLUDE_DIRS "detours/detours.h" REQUIRED)
find_library(DETOURS_LIBRARY detours REQUIRED)
target_include_directories(${PROJECT_NAME} PRIVATE ${DETOURS_INCLUDE_DIRS})
target_link_libraries(${PROJECT_NAME} PRIVATE ${DETOURS_LIBRARY})
set_source_files_properties(src/DetourHelper.cpp src/GpuMemory.cpp src/WaitProbe.cpp src/InstancedDraw.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

set_source_files_properties(extern/tracy/public/TracyClient.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON COMPILE_OPTIONS "/W0")
target_compile_options(${PROJECT_NAME} PRIVATE /W4 /permissive- /utf-8)

# DLL verweist nur auf den PDB-Dateinamen (liegt neben der DLL) -> laufendes Skyrim sperrt nicht die Build-PDB
target_link_options(${PROJECT_NAME} PRIVATE "/PDBALTPATH:%_PDB%")
