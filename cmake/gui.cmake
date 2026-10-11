# hdmon-gui: wxWidgets desktop app (tray icon + drive list), built on hdmon_core.
#   -DHDMON_BUILD_GUI=ON                    use an installed wxWidgets 3.2+ (Linux: libwxgtk3.2-dev)
#   -DHDMON_BUILD_GUI=ON -DHDMON_WX_FETCH=ON  download wxWidgets 3.2.8 and link it statically (CI, Windows, macOS)
set(HDMON_WX_VERSION 3.2.8)

if(HDMON_WX_FETCH)
  include(FetchContent)
  set(wxBUILD_SHARED OFF CACHE BOOL "" FORCE)
  set(wxBUILD_INSTALL OFF CACHE BOOL "" FORCE)
  set(wxBUILD_SAMPLES OFF CACHE STRING "" FORCE)
  set(wxBUILD_TESTS OFF CACHE STRING "" FORCE)
  set(wxBUILD_DEMOS OFF CACHE BOOL "" FORCE)
  set(wxBUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
  set(wxBUILD_PRECOMP OFF CACHE STRING "" FORCE)
  if(MSVC)
    set(wxBUILD_USE_STATIC_RUNTIME ON CACHE BOOL "" FORCE)   # matches hdmon_core's /MT
  endif()
  foreach(opt wxUSE_WEBVIEW wxUSE_MEDIACTRL wxUSE_STC wxUSE_PROPGRID wxUSE_RIBBON wxUSE_AUI wxUSE_OPENGL
              wxUSE_GLCANVAS wxUSE_RICHTEXT wxUSE_XRC wxUSE_XML wxUSE_LIBTIFF wxUSE_WEBREQUEST
              wxUSE_SOCKETS wxUSE_URL wxUSE_PROTOCOL wxUSE_FS_INET wxUSE_SECRETSTORE wxUSE_LIBSDL
              wxUSE_GSTREAMER wxUSE_LIBNOTIFY wxUSE_SPELLCHECK wxUSE_PRIVATE_FONTS)
    set(${opt} OFF CACHE STRING "" FORCE)
  endforeach()
  # Linux: GTK already pulls in zlib/libpng/libjpeg, so use the system ones (wx's bundled zlib also
  # fails with GCC 14's implicit-declaration errors). Windows/macOS: bundled copies, no extra DLLs.
  if(UNIX AND NOT APPLE)
    set(HDMON_WX_IMG sys)
  else()
    set(HDMON_WX_IMG builtin)
  endif()
  foreach(opt wxUSE_ZLIB wxUSE_LIBPNG wxUSE_LIBJPEG)
    set(${opt} ${HDMON_WX_IMG} CACHE STRING "" FORCE)
  endforeach()
  foreach(opt wxUSE_REGEX wxUSE_EXPAT)
    set(${opt} builtin CACHE STRING "" FORCE)
  endforeach()
  FetchContent_Declare(wxWidgets
    URL https://github.com/wxWidgets/wxWidgets/releases/download/v${HDMON_WX_VERSION}/wxWidgets-${HDMON_WX_VERSION}.tar.bz2
    URL_HASH SHA256=c74784904109d7229e6894c85cfa068f1106a4a07c144afd78af41f373ee0fe6
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(wxWidgets)
  set(HDMON_WX_LIBS wx::core wx::base)
else()
  find_package(wxWidgets 3.2 REQUIRED COMPONENTS core base)
  include(${wxWidgets_USE_FILE})
  set(HDMON_WX_LIBS ${wxWidgets_LIBRARIES})
endif()

set(HDMON_GUI_SOURCES gui/hdmon_gui.cpp)
if(WIN32)
  configure_file(gui/res/hdmon-gui.manifest.in ${CMAKE_CURRENT_BINARY_DIR}/hdmon-gui.manifest @ONLY)
  configure_file(gui/res/hdmon-gui.rc.in ${CMAKE_CURRENT_BINARY_DIR}/hdmon-gui.rc @ONLY)
  list(APPEND HDMON_GUI_SOURCES ${CMAKE_CURRENT_BINARY_DIR}/hdmon-gui.rc)
  add_executable(hdmon-gui WIN32 ${HDMON_GUI_SOURCES})
  if(MSVC)
    target_link_options(hdmon-gui PRIVATE /MANIFEST:NO)   # ours (requireAdministrator) is in the .rc
  endif()
elseif(APPLE)
  set(HDMON_ICNS ${CMAKE_CURRENT_SOURCE_DIR}/gui/res/hdmon.icns)
  set_source_files_properties(${HDMON_ICNS} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
  add_executable(hdmon-gui MACOSX_BUNDLE ${HDMON_GUI_SOURCES} ${HDMON_ICNS})
  set_target_properties(hdmon-gui PROPERTIES
    OUTPUT_NAME "Hard Drive Monitor"
    MACOSX_BUNDLE_BUNDLE_NAME "Hard Drive Monitor"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.rreeves5.hard-drive-monitor"
    MACOSX_BUNDLE_ICON_FILE hdmon.icns
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    MACOSX_BUNDLE_COPYRIGHT "(c) 2026 Richard Reeves"
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/gui/res/Info.plist.in)
else()
  add_executable(hdmon-gui ${HDMON_GUI_SOURCES})
endif()
target_include_directories(hdmon-gui PRIVATE gui)
target_link_libraries(hdmon-gui PRIVATE hdmon_core ${HDMON_WX_LIBS})
target_compile_definitions(hdmon-gui PRIVATE HDMON_GUI_VERSION="Beta V ${PROJECT_VERSION}")
if(MSVC)
  target_compile_options(hdmon-gui PRIVATE /utf-8 /EHsc)
endif()
if(MINGW AND HDMON_STATIC)
  target_link_options(hdmon-gui PRIVATE -static -static-libgcc -static-libstdc++)
endif()
if(NOT MSVC AND NOT APPLE)
  target_link_options(hdmon-gui PRIVATE $<$<CONFIG:Release,MinSizeRel>:-s>)
endif()

if(APPLE)
  install(TARGETS hdmon-gui BUNDLE DESTINATION .)
else()
  install(TARGETS hdmon-gui RUNTIME DESTINATION bin)
endif()
