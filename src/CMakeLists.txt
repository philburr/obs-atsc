cmake_minimum_required(VERSION 3.12)

find_package(SoapySDR)

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${LIBOBS_MODULE_PATH})

find_package(FFmpeg REQUIRED
	COMPONENTS avcodec avfilter avdevice avutil swscale avformat swresample)

include_directories(${FFMPEG_INCLUDE_DIRS})

qt5_wrap_ui(obs-atsc-ouput-ui_UI_HEADERS
	output.ui)
qt5_wrap_cpp(HEADERS obs-atsc-module-ui.h)

add_library(obs-atsc MODULE
	obs-atsc-module.cpp
	obs-atsc-module-ui.cpp
	${HEADERS}
	ffmpeg/obs-ffmpeg-vaapi.c
	ffmpeg/obs-ffmpeg-ac3.c
	${obs-atsc-ouput-ui_UI_HEADERS}
	)

message("FE: ${LIBOBS_FRONTEND_TOOLS_LIB}")

target_include_directories(obs-atsc PRIVATE ${LIBOBS_INCLUDE_DIRS} ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(obs-atsc
	atsc
	SoapySDR
	Qt5::Core
	Qt5::Widgets
	${FFMPEG_LIBRARIES}
	${LIBOBS_LIB}
	${LIBOBS_FRONTEND_API_LIB}
	frontend-tools
	)
target_compile_options(obs-atsc PRIVATE -Wno-unused-parameter -Wno-missing-field-initializers)

#target_compile_features(obs-atsc PRIVATE cxx_std_17)
