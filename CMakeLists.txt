# Do nothing if building the host tools for a cross-compile; LLILC does not need to be
# included in the host tools, and the recursive invocation that builds them does not
# pass the required WITH_CORECLR variable, so we would fail if we tried.

if ( NOT LLVM_TARGET_IS_CROSSCOMPILE_HOST )
  # If we are not building as a part of LLVM, build LLILCJit as an
  # standalone project, using LLVM as an external library:
  if( CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR )
    cmake_minimum_required(VERSION 2.8.12)

    project(LLILC)
    if( APPLE )
      set(MACOSX_RPATH ON)
      if( POLICY CMP0042 )
        cmake_policy(SET CMP0042 NEW)
      endif()
    endif()

    set(CMAKE_EXPORT_COMPILE_COMMANDS 1)

    set(WITH_LLVM "" CACHE PATH "Path to the directory where LLVM was built or installed.")

    # Probe for LLVM. First with CMake (unless the user has specified an LLVM install), then manually.
    if( WITH_LLVM STREQUAL "" )
      find_package(LLVM QUIET CONFIG)
    endif()

    if( NOT LLVM_FOUND )
      set(LLVM_CONFIG_SEARCH_PATHS "")

      if( NOT WITH_LLVM STREQUAL "" )
        get_filename_component(WITH_LLVM_ABS "${WITH_LLVM}" ABSOLUTE)

        list(APPEND LLVM_CONFIG_SEARCH_PATHS "${WITH_LLVM_ABS}/bin")

        if( WIN32 )
          list(APPEND LLVM_CONFIG_SEARCH_PATHS
            "${WITH_LLVM_ABS}/bin/Debug"
            "${WITH_LLVM_ABS}/Debug/bin"
            "${WITH_LLVM_ABS}/bin/RelWithDebInfo"
            "${WITH_LLVM_ABS}/RelWithDebInfo/bin"
            "${WITH_LLVM_ABS}/bin/Release"
            "${WITH_LLVM_ABS}/Release/bin")
        endif()
      endif()

      find_path(LLVM_CONFIG_DIRECTORY "llvm-config${CMAKE_EXECUTABLE_SUFFIX}"
        HINTS ${LLVM_CONFIG_SEARCH_PATHS}
        DOC "Path to llvm-config"
        NO_DEFAULT_PATH)
      find_path(LLVM_CONFIG_DIRECTORY "llvm-config${CMAKE_EXECUTABLE_SUFFIX}"
        DOC "Path to llvm-config")

      if( NOT EXISTS "${LLVM_CONFIG_DIRECTORY}" )
        message(FATAL_ERROR "Could not find llvm-config. Please set WITH_LLVM to a directory where LLVM has been built or installed.")
      endif()

     exec_program("${LLVM_CONFIG_DIRECTORY}/llvm-config${CMAKE_EXECUTABLE_SUFFIX} --obj-root" OUTPUT_VARIABLE WITH_LLVM_ABS)

      if( NOT EXISTS "${WITH_LLVM_ABS}" )
        message(FATAL_ERROR "Could not find LLVM. Please set WITH_LLVM to a directory where LLVM has been built or installed.")
      endif()

      list(APPEND CMAKE_MODULE_PATH "${WITH_LLVM_ABS}/lib/cmake/llvm")
    else()
      list(APPEND CMAKE_MODULE_PATH "${LLVM_CMAKE_DIR}")
    endif()

    option(LLVM_INSTALL_TOOLCHAIN_ONLY "Only include toolchain files in the 'install' target." OFF)

    include(LLVMConfig)
    include(AddLLVM)
    include(TableGen)
    include(HandleLLVMOptions)

    set(PACKAGE_VERSION "${LLVM_PACKAGE_VERSION}")

    set(CMAKE_INCLUDE_CURRENT_DIR ON)
    include_directories("${LLVM_INCLUDE_DIRS}")
    link_directories("${LLVM_LIBRARY_DIRS}")

    exec_program("${LLVM_CONFIG_PATH} --bindir" OUTPUT_VARIABLE LLVM_BINARY_DIR)
    set(LLVM_TABLEGEN_EXE "${LLVM_TOOLS_BINARY_DIR}/llvm-tblgen${CMAKE_EXECUTABLE_SUFFIX}")

    # Define the default arguments to use with 'lit', and an option for the user
    # to override.
    set(LIT_ARGS_DEFAULT "-sv")
    if (MSVC OR XCODE)
      set(LIT_ARGS_DEFAULT "${LIT_ARGS_DEFAULT} --no-progress-bar")
    endif()
    set(LLVM_LIT_ARGS "${LIT_ARGS_DEFAULT}" CACHE STRING "Default options for lit")

    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

    # They are used as destination of target generators.
    set(LLVM_RUNTIME_OUTPUT_INTDIR ${CMAKE_BINARY_DIR}/${CMAKE_CFG_INTDIR}/bin)
    set(LLVM_LIBRARY_OUTPUT_INTDIR ${CMAKE_BINARY_DIR}/${CMAKE_CFG_INTDIR}/lib${LLVM_LIBDIR_SUFFIX})

    set(LLVM_DEFAULT_TARGET_TRIPLE "${TARGET_TRIPLE}")
    set(LLILC_BUILT_STANDALONE 1)

    find_package(LibXml2)
    if (LIBXML2_FOUND)
      set(LLILC_HAVE_LIBXML 1)
    endif ()
  endif()

  set(WITH_CORECLR  "" CACHE PATH "Path to the directory where CoreCLR was built or installed.")
  set(CORECLR_SEARCH_PATHS "")
  set(CORECLR_INCLUDE "")
  set(CORECLR_GCINFO "")

  if( NOT WITH_CORECLR STREQUAL "" )
    get_filename_component(WITH_CORECLR_ABS "${WITH_CORECLR}" ABSOLUTE)
    list(APPEND CORECLR_SEARCH_PATHS "${WITH_CORECLR_ABS}")
    set(CORECLR_INCLUDE "${WITH_CORECLR_ABS}/inc")
    set(CORECLR_GCINFO "${WITH_CORECLR_ABS}/gcinfo")  
  endif()

  # Check for CoreCLR headers
  find_path(CORINFO_H "corinfo.h" HINTS "${CORECLR_INCLUDE}" NO_CMAKE_FIND_ROOT_PATH)
  if(CORINFO_H STREQUAL CORINFO_H-NOTFOUND)
    message(FATAL_ERROR "Cannot find corinfo.h in ${CORECLR_INCLUDE}. Please set WITH_CORECLR to a directory where CoreCLR was built or installed.")
    unset(WITH_CORECLR CACHE)
  endif()

  get_filename_component(CORECLR_INCLUDE "${CORECLR_INCLUDE}" DIRECTORY CACHE)

  find_path(CORJIT_H "corjit.h" PATHS "${CORECLR_INCLUDE}" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
  if(CORJIT_H STREQUAL CORJIT_H-NOTFOUND)
    message(FATAL_ERROR "Cannot find corjit.h. Please set WITH_CORECLR to a directory where CoreCLR was built or installed.")
  endif()

  find_path(CORERROR_H "corerror.h" PATHS "${CORECLR_INCLUDE}" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
  if(CORERROR_H STREQUAL CORERROR_H-NOTFOUND)
    message(FATAL_ERROR "Cannot find corerror.h. Please set WITH_CORECLR to a directory where CoreCLR was built or installed.")
  endif()

  find_path(CORHDR_H "corhdr.h" PATHS "${CORECLR_INCLUDE}" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
  if(CORHDR_H STREQUAL CORHDR_H-NOTFOUND)
    message(FATAL_ERROR "Cannot find corhdr.h. Please set WITH_CORECLR to a directory where CoreCLR was built or installed.")
  endif()

  find_path(COR_H "cor.h" PATHS "${CORECLR_INCLUDE}" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
  if(COR_H STREQUAL COR_H-NOTFOUND)
    message(FATAL_ERROR "Cannot find cor.h. Please set WITH_CORECLR to a directory where CoreCLR was built or installed.")
  endif()

  find_path(OPCODE_DEF "opcode.def" PATHS "${CORECLR_INCLUDE}" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
  if(OPCODE_DEF STREQUAL OPCODE_DEF-NOTFOUND)
    message(FATAL_ERROR "Cannot find opcode.def. Please set WITH_CORECLR to a directory where CoreCLR was built or installed.")
  endif()

  find_path(OPENUM_H "openum.h" PATHS "${CORECLR_INCLUDE}" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
  if(OPENUM_H STREQUAL OPENUM_H-NOTFOUND)
    message(FATAL_ERROR "Cannot find openum.h. Please set WITH_CORECLR to a directory where CoreCLR was built or installed.")
  endif()

  find_path(GCINFOTYPES_H "gcinfotypes.h" PATHS "${CORECLR_INCLUDE}" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
  if(GCINFOTYPES_H STREQUAL GCINFOENCODER_H-NOTFOUND)
    message(FATAL_ERROR "Cannot find gcinfotypes.h. Please set WITH_CORECLR to a directory where CoreCLR was built or installed.")
  endif()

  find_path(GCINFOENCODER_H "gcinfoencoder.h" PATHS "${CORECLR_INCLUDE}" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
  if(GCINFOENCODER_H STREQUAL GCINFOENCODER_H-NOTFOUND)
    message(FATAL_ERROR "Cannot find gcinfoencoder.h. Please set WITH_CORECLR to a directory where CoreCLR was built or installed.")
  endif()

  find_path(GCINFOENCODER_CPP "gcinfoencoder.cpp" PATHS "${CORECLR_GCINFO}" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
  if(GCINFOENCODER_CPP STREQUAL GCINFOENCODER_CPP-NOTFOUND)
    message(FATAL_ERROR "Cannot find gcinfoencoder.cpp. Please set WITH_CORECLR to a directory where CoreCLR was built or installed.")
  endif()

  include_directories("${CORECLR_INCLUDE}")

  if( UNIX )
    find_library(CORECLR_PATH "coreclr"
      HINTS ${CORECLR_SEARCH_PATHS}
      DOC "Path to libcoreclr"
      NO_DEFAULT_PATH
      NO_CMAKE_FIND_ROOT_PATH)
    find_library(CORECLR_PATH "coreclr"
      DOC "Path to libcoreclr")

    if( NOT ${CORECLR_PATH} STREQUAL CORECLR_PATH-NOTFOUND AND EXISTS "${CORECLR_PATH}" )
      get_filename_component(WITH_CORECLR_ABS "${CORECLR_SO_PATH}" DIRECTORY CACHE)
      link_directories("${WITH_CORECLR_ABS}")
    else()
      message(FATAL_ERROR "Could not find libcoreclr. Please set WITH_CORECLR to a directory where CoreCLR was built or installed.")
    endif()
  endif()

  set(LLILC_RESOURCE_DIR "" CACHE STRING
    "Relative directory from the LLILCJit binary to its resource files.")

  set(C_INCLUDE_DIRS "" CACHE STRING
    "Colon separated list of directories LLILCJit will search for headers.")

  set(GCC_INSTALL_PREFIX "" CACHE PATH "Directory where gcc is installed." )
  set(DEFAULT_SYSROOT "" CACHE PATH
    "Default <path> to all compiler invocations for --sysroot=<path>." )

  set(LLILC_VENDOR "" CACHE STRING
    "Vendor-specific text for showing with version information.")

  if( LLILC_VENDOR )
    add_definitions( -DLLILC_VENDOR="${LLILC_VENDOR} " )
  endif()

  set(LLILC_REPOSITORY_STRING "" CACHE STRING
    "Vendor-specific text for showing the repository the source is taken from.")

  if(LLILC_REPOSITORY_STRING)
    add_definitions(-DLLILC_REPOSITORY_STRING="${LLILC_REPOSITORY_STRING}")
  endif()

  set(LLILC_VENDOR_UTI "org.llvm.llilc" CACHE STRING
    "Vendor-specific uti.")

  set(LLILC_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
  set(LLILC_BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR})

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
      "${LLILC_SOURCE_DIR}/include/*.inc")
    if( tablegenned_files_on_include_dir )
      message(FATAL_ERROR "Apparently there is a previous in-source build, "
  "probably as the result of running `configure' and `make' on "
  "${LLILC_SOURCE_DIR}. This may cause problems. The suspicious files are:\n"
  "${tablegenned_files_on_include_dir}\nPlease clean the source directory.")
    endif()
  endif()

  # Compute the LLILCJit version from the LLVM version.
  string(REGEX MATCH "[0-9]+\\.[0-9]+(\\.[0-9]+)?" LLILC_VERSION
    ${PACKAGE_VERSION})
  message(STATUS "LLILC version: ${LLILC_VERSION}")

  string(REGEX REPLACE "([0-9]+)\\.[0-9]+(\\.[0-9]+)?" "\\1" LLILC_VERSION_MAJOR
    ${LLILC_VERSION})
  string(REGEX REPLACE "[0-9]+\\.([0-9]+)(\\.[0-9]+)?" "\\1" LLILC_VERSION_MINOR
    ${LLILC_VERSION})
  if (${LLILC_VERSION} MATCHES "[0-9]+\\.[0-9]+\\.[0-9]+")
    set(LLILC_HAS_VERSION_PATCHLEVEL 1)
    string(REGEX REPLACE "[0-9]+\\.[0-9]+\\.([0-9]+)" "\\1" LLILC_VERSION_PATCHLEVEL
      ${LLILC_VERSION})
  else()
    set(LLILC_HAS_VERSION_PATCHLEVEL 0)
  endif()

  if ((CMAKE_BUILD_TYPE STREQUAL "DEBUG") OR (CMAKE_BUILD_TYPE STREQUAL ""))
    set(DEBUG 1)
  endif()

  if (DEBUG)
    add_definitions( -D_DEBUG )
  endif()

  # Add appropriate flags for MSVC
  if (MSVC)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -EHsc")
    # Force -WX on even if it's not enabled for LLVM.
    if (NOT LLVM_ENABLE_WERROR)
      set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -WX")
    endif()
  endif()

  # Add appropriate flags for GCC
  if (LLVM_COMPILER_IS_GCC_COMPATIBLE)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-common -Woverloaded-virtual -Wcast-qual -fno-strict-aliasing -fno-rtti")

    if (DEBUG)
      set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g")
    endif ()

    # Enable -pedantic for LLILCJit even if it's not enabled for LLVM.
    if (NOT LLVM_ENABLE_PEDANTIC)
      set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pedantic -Wno-long-long")
    endif ()

    check_cxx_compiler_flag("-Werror -Wnested-anon-types" CXX_SUPPORTS_NO_NESTED_ANON_TYPES_FLAG)
    if (CXX_SUPPORTS_NO_NESTED_ANON_TYPES_FLAG)
      set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-nested-anon-types" )
    endif()

    # remove -Wnon-virtual-dtor for now, since CLR headers are not clean
    string(REGEX REPLACE "-Wnon-virtual-dtor" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")

    # if supported, add -Wno-unknown-pragmas, since we use pragma region in LLILC
    check_cxx_compiler_flag("-Werror -Wno-unknown-pragmas" CXX_SUPPORTS_NO_UNKNOWN_PRAGMAS_FLAG)
    if (CXX_SUPPORTS_NO_UNKNOWN_PRAGMAS_FLAG)
      set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-unknown-pragmas")
    endif()

    # if supported, add -Wno-covered-switch-default, since we use default NYI's 
    # to catch upstream contract changes for enums used in switches
    check_cxx_compiler_flag("-Werror -Wno-covered-switch-default" CXX_SUPPORTS_NO_COVERED_SWITCH_DEFAULT)
    if (CXX_SUPPORTS_NO_COVERED_SWITCH_DEFAULT)
      set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-covered-switch-default")
    endif()

    # force on -Werror to catch any newly introduced warnings
    if (NOT LLVM_ENABLE_WERROR)
      set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Werror")
    endif()

  endif ()

  if (APPLE)
    set(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} -Wl,-flat_namespace -Wl,-undefined -Wl,suppress")
  endif ()

  function(llilc_tablegen)
    # Syntax:
    # llilc-tablegen output-file [tablegen-arg ...] SOURCE source-file
    # [[TARGET cmake-target-name] [DEPENDS extra-dependency ...]]
    #
    # Generates a custom command for invoking tblgen as
    #
    # tblgen source-file -o=output-file tablegen-arg ...
    #
    # and, if cmake-target-name is provided, creates a custom target for
    # executing the custom command depending on output-file. It is
    # possible to list more files to depend after DEPENDS.

    cmake_parse_arguments( CTG "SOURCE;TARGET;DEPENDS" "" ${ARGN} )

    if( NOT CTG_SOURCE )
      message(FATAL_ERROR "SOURCE source-file required by llilc_tablegen")
    endif()

    set( LLVM_TARGET_DEFINITIONS ${CTG_SOURCE} )
    tablegen( LLILCJit ${CTG_DEFAULT_ARGS} )

    list( GET CTG_DEFAULT_ARGS 0 output_file )
    if( CTG_TARGET )
      add_custom_target( ${CTG_TARGET} DEPENDS ${output_file} ${CTG_DEPENDS} )
      set_target_properties( ${CTG_TARGET} PROPERTIES FOLDER "LLILCJit tablegenning")
    endif()
  endfunction(llilc_tablegen)

  # FIXME: Generalize and move to llvm.
  function(add_llilc_symbol_exports target_name export_file)
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
  endfunction(add_llilc_symbol_exports)

  macro(add_llilcjit_library name)
    llvm_process_sources(srcs ${ARGN})
    if(MSVC_IDE OR XCODE)
      # Add public headers
      file(RELATIVE_PATH lib_path
        ${LLILC_SOURCE_DIR}/lib/
        ${CMAKE_CURRENT_SOURCE_DIR}
      )
      if(NOT lib_path MATCHES "^[.][.]")
        file( GLOB_RECURSE headers
          ${LLILC_SOURCE_DIR}/include/${lib_path}/*.h
          ${LLILC_SOURCE_DIR}/include/${lib_path}/*.def
        )
        set_source_files_properties(${headers} PROPERTIES HEADER_FILE_ONLY ON)

        file( GLOB_RECURSE tds
          ${LLILC_SOURCE_DIR}/include/${lib_path}/*.td
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
      add_llilcjit_symbol_exports( ${name} ${EXPORTED_SYMBOL_FILE} ) 
    endif()

    if (NOT LLVM_INSTALL_TOOLCHAIN_ONLY OR ${name} STREQUAL "libllilcjit")
      install(TARGETS ${name}
        LIBRARY DESTINATION lib${LLVM_LIBDIR_SUFFIX}
        ARCHIVE DESTINATION lib${LLVM_LIBDIR_SUFFIX}
        RUNTIME DESTINATION bin)
    endif()

    set_target_properties(${name} PROPERTIES FOLDER "LLILCJit libraries")
  endmacro(add_llilcjit_library)

  macro(add_llilcjit_executable name)
    add_llvm_executable( ${name} ${ARGN} )
    set_target_properties(${name} PROPERTIES FOLDER "LLILCJit executables")
  endmacro(add_llilcjit_executable)

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
  add_definitions( -DFEATURE_READYTORUN_COMPILER )

  if (UNIX)
    link_directories("${WITH_CORECLR_ABS}")

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

  # LLILCJit version information
  set(LLILC_EXECUTABLE_VERSION
       "${LLILC_VERSION_MAJOR}.${LLILC_VERSION_MINOR}" CACHE STRING
      "Version number that will be placed into the llilcjit executable, in the form XX.YY")
  set(LIBLLILC_LIBRARY_VERSION
       "${LLILC_VERSION_MAJOR}.${LLILC_VERSION_MINOR}" CACHE STRING
      "Version number that will be placed into the libllilcjit library , in the form XX.YY")
  mark_as_advanced(LLILC_EXECUTABLE_VERSION LIBLLILC_LIBRARY_VERSION)

  add_subdirectory(include)
  add_subdirectory(lib)
  add_subdirectory(tools)

  option (LLILC_ENABLE_DOXYGEN "Use doxygen to generate LLILC API documentation." OFF)

  if(LLVM_ENABLE_DOXYGEN)
    set(LLILC_ENABLE_DOXYGEN ON)
  endif()

  if (LLILC_ENABLE_DOXYGEN)
    if (LLILC_BUILT_STANDALONE OR NOT LLVM_ENABLE_DOXYGEN)
      find_package(Doxygen REQUIRED)

      find_program(LLVM_PATH_DOT dot)
      mark_as_advanced(LLVM_PATH_DOT)
      if (LLVM_PATH_DOT)
        set(HAVE_DOT 1 CACHE INTERNAL "Is dot available?")
        mark_as_advanced(HAVE_DOT)
      else()
        message(FATAL_ERROR "Could not find dot. Please install graphviz.")
      endif()
    endif()

    if (DOXYGEN_FOUND)
      set(abs_top_srcdir ${CMAKE_CURRENT_SOURCE_DIR})
      set(abs_top_builddir ${CMAKE_CURRENT_BINARY_DIR})
    
      if (HAVE_DOT)
        set(DOT ${LLVM_PATH_DOT})
      endif()

      configure_file(${CMAKE_CURRENT_SOURCE_DIR}/Documentation/doxygen.cfg.in
        ${CMAKE_CURRENT_BINARY_DIR}/Documentation/doxygen.cfg @ONLY)

      set(abs_top_srcdir)
      set(abs_top_builddir)
      set(DOT)

      add_custom_target(doxygen-llilc
        COMMAND ${DOXYGEN_EXECUTABLE} ${CMAKE_CURRENT_BINARY_DIR}/Documentation/doxygen.cfg
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        COMMENT "Generating LLILC doxygen documentation." VERBATIM)
    endif()
  endif()

  option(LLILC_INCLUDE_TESTS
         "Generate build targets for the LLILCJit unit tests."
         ${LLVM_INCLUDE_TESTS})

  if( LLILC_INCLUDE_TESTS )
    add_subdirectory(test)
  endif()

  # Workaround for MSVS10 to avoid the Dialog Hell
  # FIXME: This could be removed with future version of CMake.
  if( LLILC_BUILT_STANDALONE AND MSVC_VERSION EQUAL 1600 )
    set(LLILC_SLN_FILENAME "${CMAKE_CURRENT_BINARY_DIR}/llilcjit.sln")
    if( EXISTS "${LLILC_SLN_FILENAME}" )
      file(APPEND "${LLILC_SLN_FILENAME}" "\n# This should be regenerated!\n")
    endif()
  endif()

  set(BUG_REPORT_URL "http://llvm.org/bugs/FixME:::" CACHE STRING // Lub FixMe:
    "Default URL where bug reports are to be submitted.")

  set(LLILC_ORDER_FILE "" CACHE FILEPATH
    "Order file to use when compiling LLILCJit in order to improve startup time.")

endif( NOT LLVM_TARGET_IS_CROSSCOMPILE_HOST )
