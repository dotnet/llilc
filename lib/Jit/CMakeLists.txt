include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../../include/clr
                    ${CMAKE_CURRENT_SOURCE_DIR}/../../include/Pal
                    ${CMAKE_CURRENT_SOURCE_DIR}/../../include/Jit
                    ${CMAKE_CURRENT_SOURCE_DIR}/../../include/Reader)

set(LLVM_LINK_COMPONENTS
  Analysis
  CodeGen
  Core
  ExecutionEngine
  IRReader
  MCJIT
  MC
  Support
  native
  )

if ( WIN32 )
  set(CMAKE_CXX_FLAGS "-EHsc")

  # Create .def file containing a list of exports preceeded by
  # 'EXPORTS'.  The file "MSILCJit.exports" already contains the list, so we
  # massage it into the correct format here to create "MSILCJit.exports.def".
  set(MSILCJIT_EXPORTS_DEF ${CMAKE_CURRENT_BINARY_DIR}/MSILCJit.exports.def)
  set(MSILCJIT_EXPORTS_DEF_TEMP ${MSILCJIT_EXPORTS_DEF}.txt)
  file(READ "MSILCJit.exports" exports_list)
  file(WRITE ${MSILCJIT_EXPORTS_DEF_TEMP} "LIBRARY MSILCJIT\n")
  file(APPEND ${MSILCJIT_EXPORTS_DEF_TEMP} "EXPORTS\n")
  file(APPEND ${MSILCJIT_EXPORTS_DEF_TEMP} ${exports_list})

  # Copy the file only if it has changed.
  execute_process(COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${MSILCJIT_EXPORTS_DEF_TEMP} ${MSILCJIT_EXPORTS_DEF})

  set(SHARED_LIB_SOURCES ${SOURCES} ${MSILCJIT_EXPORTS_DEF})
else()
  set(SHARED_LIB_SOURCES ${SOURCES})
endif()

set(LLVM_EXPORTED_SYMBOL_FILE ${MSILCJIT_EXPORTS_DEF})

add_msilcjit_library(
  msilcjit
  SHARED
  jitpch.cpp
  MSILCJit.cpp
  EEMemoryManager.cpp
  ${MSILCJIT_EXPORTS_DEF}
  )

add_dependencies(
  msilcjit
  msilcReader
  )

target_link_libraries(
  msilcjit
  ${cmake_2_8_12_PRIVATE}
  msilcReader
  )
