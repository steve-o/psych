# - Try to find RFA
# Once done this will define
#  RFA_FOUND - System has RFA
#  RFA_INCLUDE_DIRS - The RFA include directories
#  RFA_LIBRARIES - The libraries needed to use RFA
#  RFA_DEFINITIONS - Compiler switches required for using RFA

#-------------------------------------------------------------------------------
#  FindRFA functions & macros
#

macro(_RFA_ADJUST_LIB_VARS basename)
  if(RFA_INCLUDE_DIR )
    if(RFA_${basename}_LIBRARY_DEBUG AND RFA_${basename}_LIBRARY_RELEASE)
      # if the generator supports configuration types then set
      # optimized and debug libraries, or if the CMAKE_BUILD_TYPE has a value
      if(CMAKE_CONFIGURATION_TYPES OR CMAKE_BUILD_TYPE)
        set(RFA_${basename}_LIBRARY optimized ${RFA_${basename}_LIBRARY_RELEASE} debug ${RFA_${basename}_LIBRARY_DEBUG})
        set(RFA_${basename}_SHARED_LIBRARY ${RFA_${basename}_SHARED_LIBRARY_RELEASE})
      else()
        # if there are no configuration types and CMAKE_BUILD_TYPE has no value
        # then just use the release libraries
        set(RFA_${basename}_LIBRARY ${RFA_${basename}_LIBRARY_RELEASE} )
        set(RFA_${basename}_SHARED_LIBRARY ${RFA_${basename}_SHARED_LIBRARY_RELEASE})
      endif()
      # FIXME: This probably should be set for both cases
      set(RFA_${basename}_LIBRARIES optimized ${RFA_${basename}_LIBRARY_RELEASE} debug ${RFA_${basename}_LIBRARY_DEBUG})
      set(RFA_${basename}_SHARED_LIBRARIES ${RFA_${basename}_SHARED_LIBRARY_RELEASE})
    endif()

    if(RFA_${basename}_LIBRARY)
      set(RFA_${basename}_LIBRARY ${RFA_${basename}_LIBRARY} CACHE FILEPATH "The RFA ${basename} library")
      set(RFA_${basename}_SHARED_LIBRARY ${RFA_${basename}_SHARED_LIBRARY} CACHE FILEPATH "The RFA ${basename} shared library")

      # Remove superfluous "debug" / "optimized" keywords from
      # RFA_LIBRARY_DIRS
      foreach(_rfa_my_lib ${RFA_${basename}_LIBRARY})
        get_filename_component(_rfa_my_lib_path "${_rfa_my_lib}" PATH)
        list(APPEND RFA_LIBRARY_DIRS ${_rfa_my_lib_path})
      endforeach()
      list(REMOVE_DUPLICATES RFA_LIBRARY_DIRS)

      set(RFA_LIBRARY_DIRS ${RFA_LIBRARY_DIRS} CACHE FILEPATH "RFA library directory")
      set(RFA_${basename}_FOUND ON CACHE INTERNAL "Whether the RFA ${basename} library found")
    endif(RFA_${basename}_LIBRARY)

  endif(RFA_INCLUDE_DIR )
  # Make variables changeble to the advanced user
  mark_as_advanced(
      RFA_${basename}_LIBRARY
      RFA_${basename}_LIBRARY_RELEASE
      RFA_${basename}_LIBRARY_DEBUG
      RFA_${basename}_SHARED_LIBRARY
      RFA_${basename}_SHARED_LIBRARY_RELEASE
      RFA_${basename}_SHARED_LIBRARY_DEBUG
  )
endmacro(_RFA_ADJUST_LIB_VARS)

# Guesses RFA's compiler prefix used in built library names
# Returns the guess by setting the variable pointed to by _ret
function(_RFA_GUESS_COMPILER_PREFIX _ret)
  if (MSVC11)
    set(_rfa_COMPILER "110")
  elseif (MSVC10)
    set(_rfa_COMPILER "100")
  elseif (MSVC90)
    set(_rfa_COMPILER "90")
  else()
    # TODO at least RFA_DEBUG here?
    set(_rfa_COMPILER "")
  endif()
  set(${_ret} ${_rfa_COMPILER} PARENT_SCOPE)
endfunction()

# Guesses target architecture
function(_RFA_GUESS_ARCH_PREFIX _ret)
  if (CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(_rfa_ARCH "64")
  else()
    set(_rfa_ARCH "32")
  endif()
  set(${_ret} ${_rfa_ARCH} PARENT_SCOPE)
endfunction()

#
# End functions/macros
#
#-------------------------------------------------------------------------------

#-------------------------------------------------------------------------------
# main.
#-------------------------------------------------------------------------------

# Check the version of RFA against the requested version.
if(RFA_FIND_VERSION AND NOT RFA_FIND_VERSION_MINOR)
  message(SEND_ERROR "When requesting a specific version of RFA, you must provide at least the major and minor version numbers, e.g., 7.2")
endif()

# The reason that we failed to find RFA This will be set to a
# user-friendly message when we fail to find some necessary piece of
# RFA.
set(RFA_ERROR_REASON)

  if(RFA_DEBUG)
    # Output some of their choices
    message(STATUS "[ ${CMAKE_CURRENT_LIST_FILE}:${CMAKE_CURRENT_LIST_LINE} ] "
                   "RFA_USE_STATIC_LIBS = ${RFA_USE_STATIC_LIBS}")
  endif()

  if( RFA_ROOT )
    file(TO_CMAKE_PATH ${RFA_ROOT} RFA_ROOT)
  endif()
 
  if(RFA_DEBUG)
    message(STATUS "[ ${CMAKE_CURRENT_LIST_FILE}:${CMAKE_CURRENT_LIST_LINE} ] "
                   "Declared as CMake or Environmental Variables:")
    message(STATUS "[ ${CMAKE_CURRENT_LIST_FILE}:${CMAKE_CURRENT_LIST_LINE} ] "
                   "  RFA_ROOT = ${RFA_ROOT}")
  endif()

  if( RFA_ROOT )
    set(_rfa_INCLUDE_SEARCH_DIRS
      ${RFA_ROOT}/Include
      ${RFA_ROOT}/Include/rwf)
  endif()

  # ------------------------------------------------------------------------
  #  Search for RFA include DIR
  # ------------------------------------------------------------------------
  if( NOT RFA_INCLUDE_DIR )
    if(RFA_DEBUG)
      message(STATUS "[ ${CMAKE_CURRENT_LIST_FILE}:${CMAKE_CURRENT_LIST_LINE} ] "
                     "Include debugging info:")
      message(STATUS "[ ${CMAKE_CURRENT_LIST_FILE}:${CMAKE_CURRENT_LIST_LINE} ] "
                     "  _rfa_INCLUDE_SEARCH_DIRS = ${_rfa_INCLUDE_SEARCH_DIRS}")
    endif()

    # Look for a standard rfa header file.
    find_path(RFA_INCLUDE_DIR
      NAMES         AnsiPage/Page.h
      HINTS         ${_rfa_INCLUDE_SEARCH_DIRS}
      ${_rfa_FIND_OPTIONS}
      )
    find_path(RWF_INCLUDE_DIR
      NAMES         rtr/os.h
      HINTS         ${_rfa_INCLUDE_SEARCH_DIRS}
      ${_rfa_FIND_OPTIONS}
      )
  endif()

  # ------------------------------------------------------------------------
  #  Suffix initialization and compiler suffix detection.
  # ------------------------------------------------------------------------
  if (RFA_COMPILER)
    set(_rfa_COMPILER ${RFA_COMPILER})
    if(RFA_DEBUG)
      message(STATUS "[ ${CMAKE_CURRENT_LIST_FILE}:${CMAKE_CURRENT_LIST_LINE} ] "
                     "using user-specified RFA_COMPILER = ${_rfa_COMPILER}")
    endif()
  else()
    # Attempt to guess the compiler suffix
    # NOTE: this is not perfect yet, if you experience any issues
    # please report them and use the RFA_COMPILER variable
    # to work around the problems.
    _RFA_GUESS_COMPILER_PREFIX(_rfa_COMPILER)
    if(RFA_DEBUG)
      message(STATUS "[ ${CMAKE_CURRENT_LIST_FILE}:${CMAKE_CURRENT_LIST_LINE} ] "
        "guessed _rfa_COMPILER = ${_rfa_COMPILER}")
    endif()
  endif()

  if (RFA_ARCH)
    set(_rfa_ARCH ${RFA_ARCH})
    if(RFA_DEBUG)
      message(STATUS "[ ${CMAKE_CURRENT_LIST_FILE}:${CMAKE_CURRENT_LIST_LINE} ] "
                     "using user-specified RFA_ARCH = ${_rfa_ARCH}")
    endif()
  else()
    _RFA_GUESS_ARCH_PREFIX(_rfa_ARCH)
    if(RFA_DEBUG)
      message(STATUS "[ ${CMAKE_CURRENT_LIST_FILE}:${CMAKE_CURRENT_LIST_LINE} ] "
        "guessed _rfa_ARCH = ${_rfa_ARCH}")
    endif()
  endif()

  if("${_rfa_ARCH}" STREQUAL "64")
    set( _rfa_ARCH_LIBRARY "_x${_rfa_ARCH}")
  endif()

  # ------------------------------------------------------------------------
  #  Begin finding rfa libraries
  # ------------------------------------------------------------------------

  if(RFA_ROOT)
    set(_rfa_LIBRARY_SEARCH_DIRS_ALWAYS
      ${RFA_ROOT}/Libs/WIN_${_rfa_ARCH}_VS${_rfa_COMPILER})
  endif()
  set(_rfa_LIBRARY_SEARCH_DIRS ${_rfa_LIBRARY_SEARCH_DIRS_ALWAYS})

  # prepend RFA_LIBRARYDIR to search path if specified
  if( RFA_LIBRARYDIR )
    file(TO_CMAKE_PATH ${RFA_LIBRARYDIR} RFA_LIBRARYDIR)
    set(_rfa_LIBRARY_SEARCH_DIRS
      ${RFA_LIBRARYDIR} ${_rfa_LIBRARY_SEARCH_DIRS})
  endif()

  if(RFA_DEBUG)
    message(STATUS "[ ${CMAKE_CURRENT_LIST_FILE}:${CMAKE_CURRENT_LIST_LINE} ] "
      "_rfa_LIBRARY_SEARCH_DIRS = ${_rfa_LIBRARY_SEARCH_DIRS}")
  endif()

  foreach(COMPONENT ${RFA_FIND_COMPONENTS})
    string(TOUPPER ${COMPONENT} UPPERCOMPONENT)
    set( RFA_${UPPERCOMPONENT}_LIBRARY "RFA_${UPPERCOMPONENT}_LIBRARY-NOTFOUND" )
    set( RFA_${UPPERCOMPONENT}_LIBRARY_RELEASE "RFA_${UPPERCOMPONENT}_LIBRARY_RELEASE-NOTFOUND" )
    set( RFA_${UPPERCOMPONENT}_LIBRARY_DEBUG "RFA_${UPPERCOMPONENT}_LIBRARY_DEBUG-NOTFOUND")

    set( _rfa_docstring_release "RFA ${COMPONENT} library (release)")
    set( _rfa_docstring_debug   "RFA ${COMPONENT} library (debug)")

    #
    # Find RELEASE libraries
    #
    set(_rfa_RELEASE_NAMES
      Release_MD/RFA${RFA_FIND_VERSION_MAJOR}_${COMPONENT}${_rfa_COMPILER}${_rfa_ARCH_LIBRARY}${CMAKE_LINK_LIBRARY_SUFFIX}
      Release_MD/DACS${RFA_FIND_VERSION_MAJOR}_${COMPONENT}${_rfa_COMPILER}${_rfa_ARCH_LIBRARY}${CMAKE_LINK_LIBRARY_SUFFIX} )
    if(RFA_DEBUG)
      message(STATUS "[ ${CMAKE_CURRENT_LIST_FILE}:${CMAKE_CURRENT_LIST_LINE} ] "
                     "Searching for ${UPPERCOMPONENT}_LIBRARY_RELEASE: ${_rfa_RELEASE_NAMES}")
    endif()
    find_library(RFA_${UPPERCOMPONENT}_LIBRARY_RELEASE
        NAMES ${_rfa_RELEASE_NAMES}
        HINTS ${_rfa_LIBRARY_SEARCH_DIRS}
        ${_rfa_FIND_OPTIONS}
        DOC "${_rfa_docstring_release}"
    )

    find_file(RFA_${UPPERCOMPONENT}_SHARED_LIBRARY_RELEASE
        NAMES
            RFA${RFA_FIND_VERSION_MAJOR}_${COMPONENT}${_rfa_COMPILER}${_rfa_ARCH_LIBRARY}${CMAKE_SHARED_LIBRARY_SUFFIX}
            DACS${RFA_FIND_VERSION_MAJOR}_${COMPONENT}${_rfa_COMPILER}${_rfa_ARCH_LIBRARY}${CMAKE_SHARED_LIBRARY_SUFFIX}
        PATHS ${_rfa_LIBRARY_SEARCH_DIRS}
	PATH_SUFFIXES Release_MD
    )

    #
    # Find DEBUG libraries
    #
    set(_rfa_DEBUG_NAMES
      Debug_MDd/RFA${RFA_FIND_VERSION_MAJOR}_${COMPONENT}${_rfa_COMPILER}${_rfa_ARCH_LIBRARY}${CMAKE_LINK_LIBRARY_SUFFIX}
      Debug_MDd/DACS${RFA_FIND_VERSION_MAJOR}_${COMPONENT}${_rfa_COMPILER}${_rfa_ARCH_LIBRARY}${CMAKE_LINK_LIBRARY_SUFFIX})
    if(RFA_DEBUG)
      message(STATUS "[ ${CMAKE_CURRENT_LIST_FILE}:${CMAKE_CURRENT_LIST_LINE} ] "
                     "Searching for ${UPPERCOMPONENT}_LIBRARY_DEBUG: ${_rfa_DEBUG_NAMES}")
    endif()
    find_library(RFA_${UPPERCOMPONENT}_LIBRARY_DEBUG
        NAMES ${_rfa_DEBUG_NAMES}
        HINTS ${_rfa_LIBRARY_SEARCH_DIRS}
        ${_rfa_FIND_OPTIONS}
        DOC "${_rfa_docstring_debug}"
    )

    find_file(RFA_${UPPERCOMPONENT}_SHARED_LIBRARY_DEBUG
        NAMES
            RFA${RFA_FIND_VERSION_MAJOR}_${COMPONENT}${_rfa_COMPILER}${_rfa_ARCH_LIBRARY}${CMAKE_SHARED_LIBRARY_SUFFIX}
            DACS${RFA_FIND_VERSION_MAJOR}_${COMPONENT}${_rfa_COMPILER}${_rfa_ARCH_LIBRARY}${CMAKE_SHARED_LIBRARY_SUFFIX}
        PATHS ${_rfa_LIBRARY_SEARCH_DIRS}
	PATH_SUFFIXES Debug_MDd
    )

    _RFA_ADJUST_LIB_VARS(${UPPERCOMPONENT})

  endforeach(COMPONENT)

  # ------------------------------------------------------------------------
  #  Extract version information from runtime
  # ------------------------------------------------------------------------

  MESSAGE(STATUS "Detecting RFA")
        SET(TRY_RUN_DIR ${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_FILES_DIRECTORY}/rfa_run.dir)

# prepare dependent Dlls
        EXECUTE_PROCESS(COMMAND ${CMAKE_COMMAND} -E copy ${RFA_COMMON_SHARED_LIBRARY_RELEASE} ${TRY_RUN_DIR}/CMakeFiles/CMakeTmp/RFA${RFA_FIND_VERSION_MAJOR}_Common${_rfa_COMPILER}${_rfa_ARCH_LIBRARY}${CMAKE_SHARED_LIBRARY_SUFFIX})
        EXECUTE_PROCESS(COMMAND ${CMAKE_COMMAND} -E copy ${RFA_CONFIG_SHARED_LIBRARY_RELEASE} ${TRY_RUN_DIR}/CMakeFiles/CMakeTmp/RFA${RFA_FIND_VERSION_MAJOR}_Config${_rfa_COMPILER}${_rfa_ARCH_LIBRARY}${CMAKE_SHARED_LIBRARY_SUFFIX})
        EXECUTE_PROCESS(COMMAND ${CMAKE_COMMAND} -E copy ${RFA_CONNECTIONS_SHARED_LIBRARY_RELEASE} ${TRY_RUN_DIR}/CMakeFiles/CMakeTmp/RFA${RFA_FIND_VERSION_MAJOR}_Connections${_rfa_COMPILER}${_rfa_ARCH_LIBRARY}${CMAKE_SHARED_LIBRARY_SUFFIX})
        EXECUTE_PROCESS(COMMAND ${CMAKE_COMMAND} -E copy ${RFA_SESSIONLAYER_SHARED_LIBRARY_RELEASE} ${TRY_RUN_DIR}/CMakeFiles/CMakeTmp/RFA${RFA_FIND_VERSION_MAJOR}_SessionLayer${_rfa_COMPILER}${_rfa_ARCH_LIBRARY}${CMAKE_SHARED_LIBRARY_SUFFIX})
        EXECUTE_PROCESS(COMMAND ${CMAKE_COMMAND} -E copy ${RFA_LOGGER_SHARED_LIBRARY_RELEASE} ${TRY_RUN_DIR}/CMakeFiles/CMakeTmp/RFA${RFA_FIND_VERSION_MAJOR}_Logger${_rfa_COMPILER}${_rfa_ARCH_LIBRARY}${CMAKE_SHARED_LIBRARY_SUFFIX})

        TRY_RUN(RUN_RESULT COMPILE_RESULT
          ${TRY_RUN_DIR}
          ${CMAKE_SOURCE_DIR}/src/rfa_version.cc
          CMAKE_FLAGS 
            "-DINCLUDE_DIRECTORIES:STRING=${CMAKE_SOURCE_DIR}/include;${RFA_INCLUDE_DIR};${RWF_INCLUDE_DIR}"
            "-DLINK_LIBRARIES:STRING=${RFA_COMMON_LIBRARY_RELEASE};${RFA_CONFIG_LIBRARY_RELEASE};${RFA_SESSIONLAYER_LIBRARY_RELEASE}"
          COMPILE_OUTPUT_VARIABLE COMPILE_OUTPUT
          RUN_OUTPUT_VARIABLE RUN_OUTPUT)

        IF(COMPILE_RESULT)
          IF(RUN_RESULT MATCHES "FAILED_TO_RUN")
            MESSAGE(STATUS "Detecting RFA - failed")
          ELSE()
            STRING(REGEX REPLACE "([0-9]+)\\.([0-9]+)\\.([0-9]+).*" "\\1" RFA_MAJOR_VERSION "${RUN_OUTPUT}")
            STRING(REGEX REPLACE "([0-9]+)\\.([0-9]+)\\.([0-9]+).*" "\\2" RFA_MINOR_VERSION "${RUN_OUTPUT}")
            STRING(REGEX REPLACE "([0-9]+)\\.([0-9]+)\\.([0-9]+).*" "\\3" RFA_SUBMINOR_VERSION "${RUN_OUTPUT}")
            MESSAGE(STATUS "Detecting RFA - done")
          ENDIF()
        ELSE()
          MESSAGE(STATUS "Check for RFA version - not found")
          MESSAGE(STATUS "Detecting RFA - failed")
        ENDIF()

  # ------------------------------------------------------------------------
  #  End finding boost libraries
  # ------------------------------------------------------------------------

  set(RFA_INCLUDE_DIRS
    ${RFA_INCLUDE_DIR}
    ${RWF_INCLUDE_DIR}
  )

  set(RFA_FOUND FALSE)
  if(RFA_INCLUDE_DIR)
    set( RFA_FOUND TRUE )

    if(RFA_MAJOR_VERSION LESS "${RFA_FIND_VERSION_MAJOR}" )
      set( RFA_FOUND FALSE )
      set(_RFA_VERSION_AGE "old")
    elseif(RFA_MAJOR_VERSION EQUAL "${RFA_FIND_VERSION_MAJOR}" )
      if(RFA_MINOR_VERSION LESS "${RFA_FIND_VERSION_MINOR}" )
        set( RFA_FOUND FALSE )
        set(_RFA_VERSION_AGE "old")
      elseif(RFA_MINOR_VERSION EQUAL "${RFA_FIND_VERSION_MINOR}" )
        if( RFA_FIND_VERSION_PATCH AND RFA_SUBMINOR_VERSION LESS "${RFA_FIND_VERSION_PATCH}" )
          set( RFA_FOUND FALSE )
          set(_RFA_VERSION_AGE "old")
        endif( RFA_FIND_VERSION_PATCH AND RFA_SUBMINOR_VERSION LESS "${RFA_FIND_VERSION_PATCH}" )
      endif( RFA_MINOR_VERSION LESS "${RFA_FIND_VERSION_MINOR}" )
    endif( RFA_MAJOR_VERSION LESS "${RFA_FIND_VERSION_MAJOR}" )

    if (NOT RFA_FOUND)
      _RFA_MARK_COMPONENTS_FOUND(OFF)
    endif()

    if (RFA_FOUND AND RFA_FIND_VERSION_EXACT)
      # If the user requested an exact version of Boost, check
      # that. We already know that the RFA version we have is >= the
      # requested version.
      set(_RFA_VERSION_AGE "new")

      # If the user didn't specify a patchlevel, it's 0.
      if (NOT RFA_FIND_VERSION_PATCH)
        set(RFA_FIND_VERSION_PATCH 0)
      endif (NOT RFA_FIND_VERSION_PATCH)

      # We'll set RFA_FOUND true again if we have an exact version match.
      set(RFA_FOUND FALSE)
      _RFA_MARK_COMPONENTS_FOUND(OFF)
      if(RFA_MAJOR_VERSION EQUAL "${RFA_FIND_VERSION_MAJOR}" )
        if(RFA_MINOR_VERSION EQUAL "${RFA_FIND_VERSION_MINOR}" )
          if(RFA_SUBMINOR_VERSION EQUAL "${RFA_FIND_VERSION_PATCH}" )
            set( RFA_FOUND TRUE )
            _RFA_MARK_COMPONENTS_FOUND(ON)
          endif(RFA_SUBMINOR_VERSION EQUAL "${RFA_FIND_VERSION_PATCH}" )
        endif( RFA_MINOR_VERSION EQUAL "${RFA_FIND_VERSION_MINOR}" )
      endif( RFA_MAJOR_VERSION EQUAL "${RFA_FIND_VERSION_MAJOR}" )
    endif (RFA_FOUND AND RFA_FIND_VERSION_EXACT)

    if(NOT RFA_FOUND)
      # State that we found a version of RFA that is too new or too old.
      set(RFA_ERROR_REASON
        "${RFA_ERROR_REASON}\nDetected version of RFA is too ${_RFA_VERSION_AGE}. Requested version was ${RFA_FIND_VERSION_MAJOR}.${RFA_FIND_VERSION_MINOR}")
      if (RFA_FIND_VERSION_PATCH)
        set(RFA_ERROR_REASON
          "${RFA_ERROR_REASON}.${RFA_FIND_VERSION_PATCH}")
      endif (RFA_FIND_VERSION_PATCH)
      if (NOT RFA_FIND_VERSION_EXACT)
        set(RFA_ERROR_REASON "${RFA_ERROR_REASON} (or newer)")
      endif (NOT RFA_FIND_VERSION_EXACT)
      set(RFA_ERROR_REASON "${RFA_ERROR_REASON}.")
    endif (NOT RFA_FOUND)

    # Always check for missing components
    set(_rfa_CHECKED_COMPONENT FALSE)
    set(_RFA_MISSING_COMPONENTS "")
    foreach(COMPONENT ${RFA_FIND_COMPONENTS})
      string(TOUPPER ${COMPONENT} COMPONENT)
      set(_rfa_CHECKED_COMPONENT TRUE)
      if(NOT RFA_${COMPONENT}_FOUND)
        string(TOLOWER ${COMPONENT} COMPONENT)
        list(APPEND _RFA_MISSING_COMPONENTS ${COMPONENT})
        set( RFA_FOUND FALSE)
      endif()
    endforeach(COMPONENT)

    if(RFA_DEBUG)
      message(STATUS "[ ${CMAKE_CURRENT_LIST_FILE}:${CMAKE_CURRENT_LIST_LINE} ] RFA_FOUND = ${RFA_FOUND}")
    endif()

    if (_RFA_MISSING_COMPONENTS)
      # We were unable to find some libraries, so generate a sensible
      # error message that lists the libraries we were unable to find.
      set(RFA_ERROR_REASON
        "${RFA_ERROR_REASON}\nThe following RFA libraries could not be found:\n")
      foreach(COMPONENT ${_RFA_MISSING_COMPONENTS})
        set(RFA_ERROR_REASON
          "${RFA_ERROR_REASON}        rfa_${COMPONENT}\n")
      endforeach(COMPONENT)

      list(LENGTH RFA_FIND_COMPONENTS RFA_NUM_COMPONENTS_WANTED)
      list(LENGTH _RFA_MISSING_COMPONENTS RFA_NUM_MISSING_COMPONENTS)
      if (${RFA_NUM_COMPONENTS_WANTED} EQUAL ${RFA_NUM_MISSING_COMPONENTS})
        set(RFA_ERROR_REASON
          "${RFA_ERROR_REASON}No RFA libraries were found. You may need to set RFA_LIBRARYDIR to the directory containing RFA libraries or RFA_ROOT to the location of RFA.")
      else (${RFA_NUM_COMPONENTS_WANTED} EQUAL ${RFA_NUM_MISSING_COMPONENTS})
        set(RFA_ERROR_REASON
          "${RFA_ERROR_REASON}Some (but not all) of the required RFA libraries were found. You may need to install these additional RFA libraries. Alternatively, set RFA_LIBRARYDIR to the directory containing RFA libraries or RFA_ROOT to the location of RFA.")
      endif (${RFA_NUM_COMPONENTS_WANTED} EQUAL ${RFA_NUM_MISSING_COMPONENTS})
    endif (_RFA_MISSING_COMPONENTS)

  else(RFA_INCLUDE_DIR)
    set( RFA_FOUND FALSE)
  endif(RFA_INCLUDE_DIR)

  # ------------------------------------------------------------------------
  #  Notification to end user about what was found
  # ------------------------------------------------------------------------

  if(RFA_FOUND)
      if(NOT RFA_FIND_QUIETLY)
        message(STATUS "RFA version: ${RFA_MAJOR_VERSION}.${RFA_MINOR_VERSION}.${RFA_SUBMINOR_VERSION}")
        if(RFA_FIND_COMPONENTS)
          message(STATUS "Found the following RFA libraries:")
        endif()
      endif(NOT RFA_FIND_QUIETLY)
      foreach( COMPONENT  ${RFA_FIND_COMPONENTS} )
        string( TOUPPER ${COMPONENT} UPPERCOMPONENT )
        if( RFA_${UPPERCOMPONENT}_FOUND )
          if(NOT RFA_FIND_QUIETLY)
            message (STATUS "  ${COMPONENT}")
          endif(NOT RFA_FIND_QUIETLY)
          set(RFA_LIBRARIES ${RFA_LIBRARIES} ${RFA_${UPPERCOMPONENT}_LIBRARY})
          set(RFA_SHARED_LIBRARIES ${RFA_SHARED_LIBRARIES} ${RFA_${UPPERCOMPONENT}_SHARED_LIBRARY})
        endif( RFA_${UPPERCOMPONENT}_FOUND )
      endforeach(COMPONENT)
  else()
    if(RFA_FIND_REQUIRED)
      message(SEND_ERROR "Unable to find the requested RFA libraries.\n${RFA_ERROR_REASON}")
    else()
      if(NOT RFA_FIND_QUIETLY)
        # we opt not to automatically output RFA_ERROR_REASON here as
        # it could be quite lengthy and somewhat imposing in its requests
        # Since RFA is not always a required dependency we'll leave this
        # up to the end-user.
        if(RFA_DEBUG OR RFA_DETAILED_FAILURE_MSG)
          message(STATUS "Could NOT find RFA\n${RFA_ERROR_REASON}")
        else()
          message(STATUS "Could NOT find RFA")
        endif()
      endif()
    endif(RFA_FIND_REQUIRED)
  endif()

  # show the RFA_INCLUDE_DIRS AND RFA_LIBRARIES variables only in the advanced view
  mark_as_advanced(RFA_INCLUDE_DIR
      RWF_INCLUDE_DIR
      RFA_INCLUDE_DIRS
      RFA_LIBRARY_DIRS
  )
