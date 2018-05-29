option(DISABLE_OPENVR_PLUGIN "Disable building of OpenVR plugin" OFF)
if(DISABLE_OPENVR)
	message(STATUS "DISABLE_OPENVR_PLUGIN is set; building of OpenVr plugin is disabled.")
	return()
endif()

if(WIN32)
	project(win-openvr)
	
	if(MSVC)
		include_directories(../../deps/openvr)
		link_directories(../../deps/openvr)
	endif()
	
	set(win-openvr_SOURCES
		win-openvr.cpp)
	
	add_library(win-openvr MODULE
		${win-openvr_SOURCES})
	target_link_libraries(win-openvr
		libobs)
	
	install_obs_plugin_with_data(win-openvr data)
endif()
