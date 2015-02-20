include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../../include/clr
                    ${CMAKE_CURRENT_SOURCE_DIR}/../../include/Pal
                    ${CMAKE_CURRENT_SOURCE_DIR}/../../include/Reader
                    ${CMAKE_CURRENT_SOURCE_DIR}/../../include/Jit)

if( WIN32 )
  set(CMAKE_CXX_FLAGS "-EHsc")
endif()

add_llilcjit_library(LLILCReader
  reader.cpp
  readerir.cpp
  GenIRStubs.cpp
  )

if( NOT LLILC_BUILT_STANDALONE )
  add_dependencies(LLILCReader intrinsics_gen)
endif()
