project(win-openvr)

if(MSVC)
    include_directories(../../deps/openvr-master)
	link_directories(../../deps/openvr-master)
endif()

set(win-openvr_SOURCES
	win-openvr.cpp)

add_library(win-openvr MODULE
	${win-openvr_SOURCES})
target_link_libraries(win-openvr
	libobs)

install_obs_plugin_with_data(win-openvr data)
