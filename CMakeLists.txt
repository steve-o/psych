# CMake build script for Velocity MarketPsych plugin
# x64 Windows Server-only
# 2012/05/02 -- Steven.McCoy@thomsonreuters.com

cmake_minimum_required (VERSION 2.8.8)

project (psych)

set(PSYCH_VERSION_MAJOR 3)
set(PSYCH_VERSION_MINOR 1)
set(PSYCH_VERSION_BUILD 15)

# libcurl, tested 7.25.0
set(CURL_ROOT C:/curl)
set(CURL_INCLUDE_DIRS ${CURL_ROOT}/include)
set(CURL_LIBRARY_DIRS ${CURL_ROOT}/lib)
set(CURL_LIBRARIES libcurl.lib)

# Net-SNMP SDK installed, tested 5.7.1.
set(NETSNMP_ROOT C:/usr)
set(NETSNMP_INCLUDE_DIRS ${NETSNMP_ROOT}/include)
set(NETSNMP_LIBRARY_DIRS ${NETSNMP_ROOT}/lib)
set(NETSNMP_LIBRARIES
	netsnmp.lib
	netsnmpagent.lib
	netsnmpmibs.lib
)

# TREP-VA 7.0 SDK
set(VHAYU_ROOT C:/Vhayu)
set(VHAYU_INCLUDE_DIRS
	${VHAYU_ROOT}/Engine/TBSDK/Include
	${VHAYU_ROOT}/Engine/ThirdParty/xerces-c-3.1.1-x86_64-windows-vc-10.0/include
)
set(VHAYU_LIBRARY_DIRS
	${VHAYU_ROOT}/Engine/TBSDK/Lib64/Release
	${VHAYU_ROOT}/Engine/ThirdParty/xerces-c-3.1.1-x86_64-windows-vc-10.0/lib
)
set(VHAYU_LIBRARIES
	FlexRecord.lib
	PluginFramework.lib
	Utility.lib
	SEDll.lib
	xerces-c_3.lib
)

# Thomson Reuters Robust Foundation API
set(RFA_ROOT D:/rfa7.2.1.L1.win-shared.rrg)
set(RFA_INCLUDE_DIRS
	${RFA_ROOT}/Include
	${RFA_ROOT}/Include/rwf
)
set(RFA_LIBRARY_DIR ${RFA_ROOT}/Libs/WIN_64_VS100/Release_MD)
set(RFA_LIBRARY_DIRS ${RFA_LIBRARY_DIR})
set(RFA_LIBRARIES
        RFA7_Common100_x64.lib
        RFA7_Config100_x64.lib
        RFA7_Logger100_x64.lib
        RFA7_Data100_x64.lib
        RFA7_Connections100_x64.lib
        RFA7_Connections_OMM100_x64.lib
        RFA7_SessionLayer100_x64.lib
        DACS7_lock100_x64.lib
)
set(RFA_RUNTIME_LIBRARIES
	${RFA_LIBRARY_DIR}/RFA7_Common100_x64.dll
	${RFA_LIBRARY_DIR}/RFA7_Config100_x64.dll
	${RFA_LIBRARY_DIR}/RFA7_Logger100_x64.dll
	${RFA_LIBRARY_DIR}/RFA7_Data100_x64.dll
	${RFA_LIBRARY_DIR}/RFA7_Connections100_x64.dll
	${RFA_LIBRARY_DIR}/RFA7_Connections_OMM100_x64.dll
	${RFA_LIBRARY_DIR}/RFA7_SessionLayer_OMM100_x64.dll
	${RFA_LIBRARY_DIR}/RFA7_SessionLayer100_x64.dll
	${RFA_LIBRARY_DIR}/RFA7_RSSL_Adapter100_x64.dll
	${RFA_LIBRARY_DIR}/RFA7_RSSL_Cons_Adapter100_x64.dll
	${RFA_LIBRARY_DIR}/RFA7_RSSL_Prov_Adapter100_x64.dll
	${RFA_LIBRARY_DIR}/DACS7_lock100_x64.dll
	${RFA_LIBRARY_DIR}/dacs4wn64.dll
)

# Boost headers plus built libraries
set(BOOST_ROOT D:/boost_1_50_0)
set(BOOST_LIBRARYDIR ${BOOST_ROOT}/stage/lib)
set(Boost_USE_STATIC_LIBS ON)
find_package (Boost 1.50 COMPONENTS chrono thread REQUIRED)

find_package(PythonInterp REQUIRED)

option(CONFIG_PSYCH_AS_APPLICATION
	"Build an application instead of a library." ON)

#-----------------------------------------------------------------------------
# force off-tree build

if(${CMAKE_SOURCE_DIR} STREQUAL ${CMAKE_BINARY_DIR})
message(FATAL_ERROR "CMake generation is not allowed within the source directory!
Remove the CMakeCache.txt file and try again from another folder, e.g.:

   del CMakeCache.txt
   mkdir build
   cd build
   cmake ..
")
endif(${CMAKE_SOURCE_DIR} STREQUAL ${CMAKE_BINARY_DIR})

#-----------------------------------------------------------------------------
# default to Release build, frequently broken: manually run cmake-gui to fix

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING
      "Choose the type of build, options are: None Debug Release RelWithDebInfo MinSizeRel."
      FORCE)
endif(NOT CMAKE_BUILD_TYPE)

set(EXECUTABLE_OUTPUT_PATH ${CMAKE_BINARY_DIR}/bin)
set(LIBRARY_OUTPUT_PATH  ${CMAKE_BINARY_DIR}/lib)

#-----------------------------------------------------------------------------
# platform specifics

add_definitions(
	-DWIN32
	-DWIN32_LEAN_AND_MEAN
# Windows Server 2008 (not-R2)
	-D_WIN32_WINNT=0x0600
# libcurl static imports
	-DCURL_STATICLIB
# Net-SNMP Autoconf overrides
	-DHAVE_STDINT_H
# RFA version
        -DRFA_LIBRARY_VERSION="7.2.1."
# production release
	-DOFFICIAL_BUILD
)

if (CONFIG_PSYCH_AS_APPLICATION)
	add_definitions(
		-DCONFIG_PSYCH_AS_APPLICATION
	)
endif (CONFIG_PSYCH_AS_APPLICATION)

# SEH Exceptions
set(CMAKE_CXX_FLAGS "/EHa")

#-----------------------------------------------------------------------------
# generators

configure_file(
	${CMAKE_SOURCE_DIR}/version_generator.py.in
	${CMAKE_BINARY_DIR}/version_generator.py
	@ONLY
)
add_custom_command(
	OUTPUT ${CMAKE_BINARY_DIR}/version.cc
	COMMAND ${PYTHON_EXECUTABLE} ${CMAKE_BINARY_DIR}/version_generator.py > ${CMAKE_BINARY_DIR}/version.cc
	WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
	DEPENDS ${CMAKE_BINARY_DIR}/version_generator.py
)
configure_file(
	${CMAKE_SOURCE_DIR}/src/version.hh
	${CMAKE_BINARY_DIR}/version.hh
	COPYONLY
)
configure_file(
	${CMAKE_SOURCE_DIR}/version.rc.in
	${CMAKE_BINARY_DIR}/version.rc
	@ONLY
)
#-----------------------------------------------------------------------------
# source files

set(cxx-sources
	src/config.cc
	src/error.cc
	src/main.cc
	src/plugin.cc
	src/provider.cc
	src/rfa.cc
	src/rfa_logging.cc
	src/session.cc
	src/snmp_agent.cc
	src/psych.cc
	src/psychMIB.cc
	src/tcl.cc
	src/chromium/chromium_switches.cc
	src/chromium/command_line.cc
	src/chromium/debug/stack_trace.cc
	src/chromium/debug/stack_trace_win.cc
	src/chromium/file_util.cc
	src/chromium/file_util_win.cc
	src/chromium/json/json_reader.cc
	src/chromium/memory/singleton.cc
	src/chromium/logging.cc
	src/chromium/logging_win.cc
	src/chromium/string_piece.cc
	src/chromium/string_split.cc
	src/chromium/string_util.cc
	src/chromium/string_number_conversions.cc
	src/chromium/stringprintf.cc
	src/chromium/synchronization/lock.cc
	src/chromium/synchronization/lock_impl_win.cc
	src/chromium/values.cc
	src/chromium/vlog.cc
	src/chromium/win/event_trace_provider.cc
	${CMAKE_BINARY_DIR}/version.cc
)
set(rc-sources
	${CMAKE_BINARY_DIR}/version.rc
)

include_directories(
	include
	${CURL_INCLUDE_DIRS}
	${NETSNMP_INCLUDE_DIRS}
	${VHAYU_INCLUDE_DIRS}
	${RFA_INCLUDE_DIRS}
	${Boost_INCLUDE_DIRS}
)

link_directories(
	${CURL_LIBRARY_DIRS}
	${NETSNMP_LIBRARY_DIRS}
	${VHAYU_LIBRARY_DIRS}
	${RFA_LIBRARY_DIRS}
	${Boost_LIBRARY_DIRS}
)

#-----------------------------------------------------------------------------
# output

if (CONFIG_PSYCH_AS_APPLICATION)
	add_executable(psych ${cxx-sources} ${rc-sources})
	target_link_libraries(psych
		${CURL_LIBRARIES}
		${NETSNMP_LIBRARIES}
		${RFA_LIBRARIES}
		${Boost_LIBRARIES}
		ws2_32.lib
		dbghelp.lib
	)
else (CONFIG_PSYCH_AS_APPLICATION)
	add_library(psych SHARED ${cxx-sources} ${rc-sources})
	target_link_libraries(psych
		${CURL_LIBRARIES}
		${NETSNMP_LIBRARIES}
		${VHAYU_LIBRARIES}
		${RFA_LIBRARIES}
		${Boost_LIBRARIES}
		ws2_32.lib
		dbghelp.lib
	)
endif (CONFIG_PSYCH_AS_APPLICATION)

add_executable(rfa_version src/rfa_version.cc)
target_link_libraries(rfa_version ${RFA_LIBRARIES})

if (CONFIG_PSYCH_AS_APPLICATION)
	set(config
		${CMAKE_CURRENT_SOURCE_DIR}/config/config.json
		${CMAKE_CURRENT_SOURCE_DIR}/config/example.reg
		"${CMAKE_CURRENT_SOURCE_DIR}/config/Sawbuck ETW provider.reg"
	)
else (CONFIG_PSYCH_AS_APPLICATION)
	set(config
		${CMAKE_CURRENT_SOURCE_DIR}/config/psych.xml
		${CMAKE_CURRENT_SOURCE_DIR}/config/example.reg
		"${CMAKE_CURRENT_SOURCE_DIR}/config/Sawbuck ETW provider.reg"
	)
endif (CONFIG_PSYCH_AS_APPLICATION)

file(GLOB mibs "${CMAKE_CURRENT_SOURCE_DIR}/mibs/*.txt")

install (TARGETS psych rfa_version DESTINATION bin)
install (FILES ${RFA_RUNTIME_LIBRARIES} DESTINATION bin)
install (FILES ${config} DESTINATION config)
install (FILES ${mibs} DESTINATION mibs)

set (CPACK_SOURCE_IGNORE_FILES "*.lib")

include (InstallRequiredSystemLibraries)
set (CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}")
if (CONFIG_PSYCH_AS_APPLICATION)
	set (CPACK_PACKAGE_DESCRIPTION_SUMMARY "Velocity Analytics MarketPsych application")
else (CONFIG_PSYCH_AS_APPLICATION)
	set (CPACK_PACKAGE_DESCRIPTION_SUMMARY "Velocity Analytics MarketPsych plugin")
endif (CONFIG_PSYCH_AS_APPLICATION)
set (CPACK_PACKAGE_VENDOR "Thomson Reuters")
set (CPACK_NSIS_CONTACT "Steven McCoy <Steven.McCoy@thomsonreuters.com>")
# There is a bug in NSI that does not handle full unix paths properly. Make
# sure there is at least one set of four (4) backlasshes.
set (CPACK_NSIS_MUI_ICON "${CMAKE_CURRENT_SOURCE_DIR}\\\\installer.ico")
set (CPACK_NSIS_MUI_UNIICON "${CMAKE_CURRENT_SOURCE_DIR}\\\\installer.ico")
set (CPACK_PACKAGE_ICON "${CMAKE_CURRENT_SOURCE_DIR}\\\\branding.bmp")
set (CPACK_NSIS_COMPRESSOR "/SOLID lzma")
set (CPACK_PACKAGE_VERSION_MAJOR "${PSYCH_VERSION_MAJOR}")
set (CPACK_PACKAGE_VERSION_MINOR "${PSYCH_VERSION_MINOR}")
set (CPACK_PACKAGE_VERSION_PATCH "${PSYCH_VERSION_BUILD}")
include (CPack)

# end of file
