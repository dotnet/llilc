# If we are not building as a part of LLVM, build MSILCJit as an
# standalone project, using LLVM as an external library:
if( CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR )
  project(MSILCJit)
  cmake_minimum_required(VERSION 2.8.8)

  set(MSILC_WITH_LLVM "" CACHE PATH "Path to the directory where LLVM was built or installed.")

  # Probe for LLVM. First with CMake (unless the user has specified an LLVM install), then manually.
  if( NOT MSILC_WITH_LLVM STREQUAL "" )
    find_package(LLVM QUIET CONFIG)
  endif()

  if( NOT LLVM_FOUND )
    set(MSILC_LLVM_CONFIG_SEARCH_PATHS "")

    if( NOT MSILC_WITH_LLVM STREQUAL "" )
      get_filename_component(MSILC_WITH_LLVM_ABS "${MSILC_WITH_LLVM}" ABSOLUTE CACHE)

      list(APPEND MSILC_LLVM_CONFIG_SEARCH_PATHS "${MSILC_WITH_LLVM_ABS}/bin")

      if( WIN32 )
        list(APPEND MSILC_LLVM_CONFIG_SEARCH_PATHS
          "${MSILC_WITH_LLVM_ABS}/bin/Debug"
          "${MSILC_WITH_LLVM_ABS}/Debug/bin"
          "${MSILC_WITH_LLVM_ABS}/bin/Release"
          "${MSILC_WITH_LLVM_ABS}/Release/bin")
      endif()
    endif()

    find_path(MSILC_LLVM_CONFIG_DIRECTORY "llvm-config${CMAKE_EXECUTABLE_SUFFIX}"
      HINTS ${MSILC_LLVM_CONFIG_SEARCH_PATHS}
      DOC "Path to llvm-config"
      NO_DEFAULT_PATH)
    find_path(MSILC_LLVM_CONFIG_DIRECTORY "llvm-config${CMAKE_EXECUTABLE_SUFFIX}"
      DOC "Path to llvm-config")

    if( NOT EXISTS "${MSILC_LLVM_CONFIG_DIRECTORY}" )
      message(FATAL_ERROR "Could not find llvm-config. Please set MSILC_WITH_LLVM to a directory where LLVM has been built or installed.")
    endif()

    exec_program("${MSILC_LLVM_CONFIG_DIRECTORY}/llvm-config${CMAKE_EXECUTABLE_SUFFIX} --obj-root" OUTPUT_VARIABLE MSILC_WITH_LLVM_ABS)

    if( NOT EXISTS "${MSILC_WITH_LLVM_ABS}" )
      message(FATAL_ERROR "Could not find LLVM. Please set MSILC_WITH_LLVM to a directory where LLVM has been built or installed.")
    endif()

    list(APPEND CMAKE_MODULE_PATH "${MSILC_WITH_LLVM_ABS}/share/llvm/cmake")
  else()
    list(APPEND CMAKE_MODULE_PATH "${LLVM_CMAKE_DIR}")
  endif()

  list(APPEND CMAKE_MODULE_PATH "${MSILC_WITH_LLVM_ABS}/share/llvm/cmake")

  option(LLVM_INSTALL_TOOLCHAIN_ONLY "Only include toolchain files in the 'install' target." OFF)

  include(LLVMConfig)
  include(AddLLVM)
  include(TableGen)
  include(HandleLLVMOptions)

  set(PACKAGE_VERSION "${LLVM_PACKAGE_VERSION}")

  set(CMAKE_INCLUDE_CURRENT_DIR ON)
  include_directories("${LLVM_INCLUDE_DIRS}")
  link_directories("${LLVM_LIBRARY_DIRS}")

  exec_program("${MSILC_LLVM_CONFIG_PATH} --bindir" OUTPUT_VARIABLE LLVM_BINARY_DIR)
  set(LLVM_TABLEGEN_EXE "${LLVM_TOOLS_BINARY_DIR}/llvm-tblgen${CMAKE_EXECUTABLE_SUFFIX}")

  # Define the default arguments to use with 'lit', and an option for the user
  # to override.
  set(LIT_ARGS_DEFAULT "-sv")
  if (MSVC OR XCODE)
    set(LIT_ARGS_DEFAULT "${LIT_ARGS_DEFAULT} --no-progress-bar")
  endif()
  set(LLVM_LIT_ARGS "${LIT_ARGS_DEFAULT}" CACHE STRING "Default options for lit")

  set( CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin )
  set( CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib )
  set( CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib )

  set( MSILCJIT_BUILT_STANDALONE 1 )

  find_package(LibXml2)
  if (LIBXML2_FOUND)
    set(MSILCJIT_HAVE_LIBXML 1)
  endif ()
endif()

if( UNIX )
  set(MSILC_WITH_CORECLR  "" CACHE PATH "Path to the directory where CoreCLR was built or installed.")

  set(MSILC_CORECLR_SO_SEARCH_PATHS "")

  if( NOT MSILC_WITH_CORECLR STREQUAL "" )
    get_filename_component(MSILC_WITH_CORECLR_ABS "${MSILC_WITH_CORECLR}" ABSOLUTE CACHE)

    list(APPEND MSILC_CORECLR_SO_SEARCH_PATHS "${MSILC_WITH_CORECLR_ABS}")
  endif()

  find_path(MSILC_CORECLR_SO_PATH "libcoreclr.so"
    HINTS ${MSILC_CORECLR_SO_SEARCH_PATHS}
    DOC "Path to libcoreclr.so"
    NO_DEFAULT_PATH)
  find_path(MSILC_CORECLR_SO_PATH "libcoreclr.so"
    DOC "Path to libcoreclr.so")

  if( EXISTS "${MSILC_CORECLR_SO_PATH}" )
    get_filename_component(MSILC_WITH_CORECLR_ABS "${MSILC_CORECLR_SO_PATH}" DIRECTORY CACHE)
    link_directories("${MSILC_WITH_CORECLR_ABS}")
  endif()
endif()

set(MSILCJIT_RESOURCE_DIR "" CACHE STRING
  "Relative directory from the MSILCJit binary to its resource files.")

set(C_INCLUDE_DIRS "" CACHE STRING
  "Colon separated list of directories MSILCJit will search for headers.")

set(GCC_INSTALL_PREFIX "" CACHE PATH "Directory where gcc is installed." )
set(DEFAULT_SYSROOT "" CACHE PATH
  "Default <path> to all compiler invocations for --sysroot=<path>." )

set(MSILCJIT_VENDOR "" CACHE STRING
  "Vendor-specific text for showing with version information.")

if( MSILCJIT_VENDOR )
  add_definitions( -DMSILCJIT_VENDOR="${MSILCJIT_VENDOR} " )
endif()

set(MSILCJIT_REPOSITORY_STRING "" CACHE STRING
  "Vendor-specific text for showing the repository the source is taken from.")

if(MSILCJIT_REPOSITORY_STRING)
  add_definitions(-DMSILCJIT_REPOSITORY_STRING="${MSILCJIT_REPOSITORY_STRING}")
endif()

set(MSILCJIT_VENDOR_UTI "org.llvm.msilcjit" CACHE STRING
  "Vendor-specific uti.")

set(MSILCJIT_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(MSILCJIT_BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR})

if( CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR AND NOT MSVC_IDE )
  message(FATAL_ERROR "In-source builds are not allowed. CMake would overwrite "
"the makefiles distributed with LLVM. Please create a directory and run cmake "
"from there, passing the path to this source directory as the last argument. "
"This process created the file `CMakeCache.txt' and the directory "
"`CMakeFiles'. Please delete them.")
endif()

if( NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR )
  file(GLOB_RECURSE
    tablegenned_files_on_include_dir
    "${MSILCJIT_SOURCE_DIR}/include/*.inc")
  if( tablegenned_files_on_include_dir )
    message(FATAL_ERROR "Apparently there is a previous in-source build, "
"probably as the result of running `configure' and `make' on "
"${MSILCJIT_SOURCE_DIR}. This may cause problems. The suspicious files are:\n"
"${tablegenned_files_on_include_dir}\nPlease clean the source directory.")
  endif()
endif()

# Compute the MSILCJit version from the LLVM version.
string(REGEX MATCH "[0-9]+\\.[0-9]+(\\.[0-9]+)?" MSILCJIT_VERSION
  ${PACKAGE_VERSION})
message(STATUS "MSILCJit version: ${MSILCJIT_VERSION}")

string(REGEX REPLACE "([0-9]+)\\.[0-9]+(\\.[0-9]+)?" "\\1" MSILCJIT_VERSION_MAJOR
  ${MSILCJIT_VERSION})
string(REGEX REPLACE "[0-9]+\\.([0-9]+)(\\.[0-9]+)?" "\\1" MSILCJIT_VERSION_MINOR
  ${MSILCJIT_VERSION})
if (${MSILCJIT_VERSION} MATCHES "[0-9]+\\.[0-9]+\\.[0-9]+")
  set(MSILCJIT_HAS_VERSION_PATCHLEVEL 1)
  string(REGEX REPLACE "[0-9]+\\.[0-9]+\\.([0-9]+)" "\\1" MSILCJIT_VERSION_PATCHLEVEL
    ${MSILCJIT_VERSION})
else()
  set(MSILCJIT_HAS_VERSION_PATCHLEVEL 0)
endif()

if (CMAKE_BUILD_TYPE STREQUAL "DEBUG")
  set(DEBUG)
endif()

if (DEBUG)
  add_definitions( -DDEBUG -D_DEBUG )
endif()

# Add appropriate flags for GCC
if (LLVM_COMPILER_IS_GCC_COMPATIBLE)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-common -Woverloaded-virtual -Wcast-qual -fno-strict-aliasing -fno-rtti")

  if (DEBUG)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g")
  endif ()

  # Enable -pedantic for MSILCJit even if it's not enabled for LLVM.
  if (NOT LLVM_ENABLE_PEDANTIC)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pedantic -Wno-long-long")
  endif ()

  check_cxx_compiler_flag("-Werror -Wnested-anon-types" CXX_SUPPORTS_NO_NESTED_ANON_TYPES_FLAG)
  if( CXX_SUPPORTS_NO_NESTED_ANON_TYPES_FLAG )
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-nested-anon-types" )
  endif()
endif ()

if (APPLE)
  set(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} -Wl,-flat_namespace -Wl,-undefined -Wl,suppress")
endif ()

include(LLVMParseArguments)

function(msilcjit_tablegen)
  # Syntax:
  # msilcjit-tablegen output-file [tablegen-arg ...] SOURCE source-file
  # [[TARGET cmake-target-name] [DEPENDS extra-dependency ...]]
  #
  # Generates a custom command for invoking tblgen as
  #
  # tblgen source-file -o=output-file tablegen-arg ...
  #
  # and, if cmake-target-name is provided, creates a custom target for
  # executing the custom command depending on output-file. It is
  # possible to list more files to depend after DEPENDS.

  parse_arguments( CTG "SOURCE;TARGET;DEPENDS" "" ${ARGN} )

  if( NOT CTG_SOURCE )
    message(FATAL_ERROR "SOURCE source-file required by msilcjit_tablegen")
  endif()

  set( LLVM_TARGET_DEFINITIONS ${CTG_SOURCE} )
  tablegen( MSILCJit ${CTG_DEFAULT_ARGS} )

  list( GET CTG_DEFAULT_ARGS 0 output_file )
  if( CTG_TARGET )
    add_custom_target( ${CTG_TARGET} DEPENDS ${output_file} ${CTG_DEPENDS} )
    set_target_properties( ${CTG_TARGET} PROPERTIES FOLDER "MSILCJit tablegenning")
  endif()
endfunction(msilcjit_tablegen)

# FIXME: Generalize and move to llvm.
function(add_msilcjit_symbol_exports target_name export_file)
  # Makefile.rules contains special cases for different platforms.
  # We restrict ourselves to Darwin for the time being.
  if(${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
    add_custom_command(OUTPUT symbol.exports
      COMMAND sed -e "s/^/_/" < ${export_file} > symbol.exports
      DEPENDS ${export_file}
      VERBATIM
      COMMENT "Creating export file for ${target_name}")
    add_custom_target(${target_name}_exports DEPENDS symbol.exports)
    set_property(DIRECTORY APPEND
      PROPERTY ADDITIONAL_MAKE_CLEAN_FILES symbol.exports)

    get_property(srcs TARGET ${target_name} PROPERTY SOURCES)
    foreach(src ${srcs})
      get_filename_component(extension ${src} EXT)
      if(extension STREQUAL ".cpp")
        set(first_source_file ${src})
        break()
      endif()
    endforeach()
  
    # Force re-linking when the exports file changes. Actually, it
    # forces recompilation of the source file. The LINK_DEPENDS target
    # property only works for makefile-based generators.
    set_property(SOURCE ${first_source_file} APPEND PROPERTY
      OBJECT_DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/symbol.exports)
  
    set_property(TARGET ${target_name} APPEND_STRING PROPERTY
                 LINK_FLAGS " -Wl,-exported_symbols_list,${CMAKE_CURRENT_BINARY_DIR}/symbol.exports")
    add_dependencies(${target_name} ${target_name}_exports)
  endif()
endfunction(add_msilcjit_symbol_exports)

macro(add_msilcjit_library name)
  llvm_process_sources(srcs ${ARGN})
  if(MSVC_IDE OR XCODE)
    # Add public headers
    file(RELATIVE_PATH lib_path
      ${MSILCJIT_SOURCE_DIR}/lib/
      ${CMAKE_CURRENT_SOURCE_DIR}
    )
    if(NOT lib_path MATCHES "^[.][.]")
      file( GLOB_RECURSE headers
        ${MSILCJIT_SOURCE_DIR}/include/${lib_path}/*.h
        ${MSILCJIT_SOURCE_DIR}/include/${lib_path}/*.def
      )
      set_source_files_properties(${headers} PROPERTIES HEADER_FILE_ONLY ON)

      file( GLOB_RECURSE tds
        ${MSILCJIT_SOURCE_DIR}/include/${lib_path}/*.td
      )
      source_group("TableGen descriptions" FILES ${tds})
      set_source_files_properties(${tds}} PROPERTIES HEADER_FILE_ONLY ON)

      set(srcs ${srcs} ${headers} ${tds})
    endif()
  endif(MSVC_IDE OR XCODE)
  if (MODULE)
    set(libkind MODULE)
  elseif (SHARED_LIBRARY)
    set(libkind SHARED)
  else()
    set(libkind)
  endif()
  add_library( ${name} ${libkind} ${srcs} )
  if( LLVM_COMMON_DEPENDS )
    add_dependencies( ${name} ${LLVM_COMMON_DEPENDS} )
  endif( LLVM_COMMON_DEPENDS )

  llvm_config( ${name} ${LLVM_LINK_COMPONENTS} )
  target_link_libraries( ${name} ${LLVM_COMMON_LIBS} )
  
  if (SHARED_LIBRARY AND EXPORTED_SYMBOL_FILE)
    add_msilcjit_symbol_exports( ${name} ${EXPORTED_SYMBOL_FILE} ) 
  endif()

  if (NOT LLVM_INSTALL_TOOLCHAIN_ONLY OR ${name} STREQUAL "libmsilcjit")
    install(TARGETS ${name}
      LIBRARY DESTINATION lib${LLVM_LIBDIR_SUFFIX}
      ARCHIVE DESTINATION lib${LLVM_LIBDIR_SUFFIX}
      RUNTIME DESTINATION bin)
  endif()

  set_target_properties(${name} PROPERTIES FOLDER "MSILCJit libraries")
endmacro(add_msilcjit_library)

macro(add_msilcjit_executable name)
  add_llvm_executable( ${name} ${ARGN} )
  set_target_properties(${name} PROPERTIES FOLDER "msilcjit executables")
endmacro(add_msilcjit_executable)

include_directories(BEFORE
  ${CMAKE_CURRENT_BINARY_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  )

if (NOT LLVM_INSTALL_TOOLCHAIN_ONLY)
  install(DIRECTORY include/
    DESTINATION include
    FILES_MATCHING
    PATTERN "*.def"
    PATTERN "*.h"
    PATTERN "config.h" EXCLUDE
    PATTERN ".svn" EXCLUDE
    )

  install(DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/include/
    DESTINATION include
    FILES_MATCHING
    PATTERN "CMakeFiles" EXCLUDE
    PATTERN "*.inc"
    )
endif()

add_definitions( -D_GNU_SOURCE )
add_definitions( -DFEATURE_CORECLR )

if (UNIX)
  link_directories("${MSILC_WITH_CORECLR_ABS}")

  add_definitions( -DPLATFORM_UNIX )
  include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include/Pal/Rt)
else()
  add_definitions( -DWIN32_LEAN_AND_MEAN )
  add_definitions( -DNOMINMAX )
endif()

if ("${CMAKE_SIZEOF_VOID_P}" EQUAL "8")
  add_definitions( -DBIT64 )
  add_definitions( -D_WIN64 )
endif()

# MSILCJit version information
set(MSILCJIT_EXECUTABLE_VERSION
     "${MSILCJIT_VERSION_MAJOR}.${MSILCJIT_VERSION_MINOR}" CACHE STRING
    "Version number that will be placed into the msilcjit executable, in the form XX.YY")
set(LIBMSILCJIT_LIBRARY_VERSION
     "${MSILCJIT_VERSION_MAJOR}.${MSILCJIT_VERSION_MINOR}" CACHE STRING
    "Version number that will be placed into the libmsilcjit library , in the form XX.YY")
mark_as_advanced(MSILCJIT_EXECUTABLE_VERSION LIBMSILCJIT_LIBRARY_VERSION)

add_subdirectory(include)
add_subdirectory(lib)
add_subdirectory(tools)

option(MSILCJIT_INCLUDE_TESTS
       "Generate build targets for the MSILCJit unit tests."
       ${LLVM_INCLUDE_TESTS})

if( MSILCJIT_INCLUDE_TESTS )
  add_subdirectory(test)
endif()

# Workaround for MSVS10 to avoid the Dialog Hell
# FIXME: This could be removed with future version of CMake.
if( MSILCJIT_BUILT_STANDALONE AND MSVC_VERSION EQUAL 1600 )
  set(MSILCJIT_SLN_FILENAME "${CMAKE_CURRENT_BINARY_DIR}/msilcjit.sln")
  if( EXISTS "${MSILCJIT_SLN_FILENAME}" )
    file(APPEND "${MSILCJIT_SLN_FILENAME}" "\n# This should be regenerated!\n")
  endif()
endif()

set(BUG_REPORT_URL "http://llvm.org/bugs/FixME:::" CACHE STRING // Lub FixMe:
  "Default URL where bug reports are to be submitted.")

set(MSILCJIT_ORDER_FILE "" CACHE FILEPATH
  "Order file to use when compiling MSILCJit in order to improve startup time.")
