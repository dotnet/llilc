get_filename_component(LLILC_INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../../include ABSOLUTE)

include_directories(${LLILC_INCLUDES}/clr
                    ${LLILC_INCLUDES}/Pal
                    ${LLILC_INCLUDES}/GcInfo
                    ${LLILC_INCLUDES}/Jit
                    ${LLILC_INCLUDES}/Reader)

set(LLVM_LINK_COMPONENTS
  Analysis
  CodeGen
  Core
  DebugInfoDWARF
  ExecutionEngine
  IPO
  IRReader
  OrcJIT
  MC
  Support
  native
  )

set(LLILCJIT_LINK_LIBRARIES LLILCReader GcInfo)

if (WIN32)
  # Create .def file containing a list of exports preceeded by
  # 'EXPORTS'.  The file "LLILCJit.exports" already contains the list, so we
  # massage it into the correct format here to create "LLILCJit.exports.def".
  set(LLILCJIT_EXPORTS_DEF ${CMAKE_CURRENT_BINARY_DIR}/LLILCJit.exports.def)
  set(LLILCJIT_EXPORTS_DEF_TEMP ${LLILCJIT_EXPORTS_DEF}.txt)
  file(READ "LLILCJit.exports" exports_list)
  file(WRITE ${LLILCJIT_EXPORTS_DEF_TEMP} "LIBRARY LLILCJIT\n")
  file(APPEND ${LLILCJIT_EXPORTS_DEF_TEMP} "EXPORTS\n")
  file(APPEND ${LLILCJIT_EXPORTS_DEF_TEMP} ${exports_list})

  # Copy the file only if it has changed.
  execute_process(COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${LLILCJIT_EXPORTS_DEF_TEMP} ${LLILCJIT_EXPORTS_DEF})

  set(SHARED_LIB_SOURCES ${SOURCES} ${LLILCJIT_EXPORTS_DEF})

  # For windows we need to specify the CoreCLR environment
  set(LLILC_TARGET_TRIPLE "${LLVM_DEFAULT_TARGET_TRIPLE}-coreclr")
else()
  if (UNIX)
    set(LLILCJIT_LINK_LIBRARIES ${LLILCJIT_LINK_LIBRARIES} coreclr)
  endif()

  set(SHARED_LIB_SOURCES ${SOURCES})

  # For non-windows we can use the default triple for now
  set(LLILC_TARGET_TRIPLE "${LLVM_DEFAULT_TARGET_TRIPLE}")
endif()

add_definitions(-DSTANDALONE_BUILD)

message(STATUS "LLILC_TARGET_TRIPLE is ${LLILC_TARGET_TRIPLE}")
add_definitions(-DLLILC_TARGET_TRIPLE="${LLILC_TARGET_TRIPLE}")

set(LLVM_EXPORTED_SYMBOL_FILE ${LLILCJIT_EXPORTS_DEF})

add_llilcjit_library(
  llilcjit
  SHARED
  jitpch.cpp
  LLILCJit.cpp
  EEMemoryManager.cpp
  jitoptions.cpp
  utility.cpp
  ${LLILCJIT_EXPORTS_DEF}
  )

add_dependencies(llilcjit LLILCReader GcInfo)

target_link_libraries(
  llilcjit
  PRIVATE
  ${LLILCJIT_LINK_LIBRARIES}
  )
