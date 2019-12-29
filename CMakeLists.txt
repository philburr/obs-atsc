cmake_minimum_required(VERSION 3.12)

if (NOT IS_DIRECTORY ${OBS_SOURCE_DIR})
	set(OBS_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../obs-studio)
endif()
if (NOT IS_DIRECTORY ${OBS_INSTALL_DIR})
	set(OBS_INSTALL_DIR ${CMAKE_INSTALL_PREFIX})
endif()

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
list(APPEND CMAKE_MODULE_PATH ${OBS_SOURCE_DIR}/cmake)

include(Modules/ObsHelpers)

set(_LIBOBS_LIBRARY_DIRS ${OBS_INSTALL_DIR}/lib)
set(_LIBOBS_INCLUDE_DIRS ${OBS_SOURCE_DIR})
include(FindLibObs)
find_package(LibObs REQUIRED)
find_package(Threads)	
find_package(Qt5Core REQUIRED)
find_package(Qt5Widgets REQUIRED)

find_file(LIBOBS_FRONTEND_API_LIB NAMES libobs-frontend-api.so HINTS ${_LIBOBS_LIBRARY_DIRS} )
find_file(LIBOBS_FRONTEND_TOOLS_LIB NAMES frontend-tools.so HINTS ${_LIBOBS_LIBRARY_DIRS}/obs-plugins )

add_library( frontend-tools SHARED IMPORTED )
set_target_properties( frontend-tools PROPERTIES IMPORTED_LOCATION ${LIBOBS_FRONTEND_TOOLS_LIB} )

add_subdirectory(external/atsc EXCLUDE_FROM_ALL)
add_subdirectory(src)

