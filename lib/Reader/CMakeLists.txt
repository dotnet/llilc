include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../../include/clr
                    ${CMAKE_CURRENT_SOURCE_DIR}/../../include/Pal
                    ${CMAKE_CURRENT_SOURCE_DIR}/../../include/Reader
                    ${CMAKE_CURRENT_SOURCE_DIR}/../../include/Jit)

if( WIN32 )
  set(CMAKE_CXX_FLAGS "-EHsc")
endif()

add_msilcjit_library(msilcReader
  reader.cpp
  readerir.cpp
  GenIRStubs.cpp
  )

if( NOT MSILCJIT_BUILT_STANDALONE )
  add_dependencies(msilcReader intrinsics_gen)
endif()
