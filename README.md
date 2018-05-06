# OpenVR Capture plugin for OBS source code

by Keijo "Kegetys" Ruotsalainen, http://www.kegetys.fi

Compiling:
- Extract over OBS source
- Put OpenVR SDK (openvr-master) under deps
- Add add_subdirectory(win-openvr) to plugins/CMakeLists.txt after win-capture
- Compile with visual studio. Only 64bit version has been tested.
