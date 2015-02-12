add_msilcjit_library(msilcReader
  reader.cpp
  readerir.cpp
  GenIRStubs.cpp
  )

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../../include/clr
                    ${CMAKE_CURRENT_SOURCE_DIR}/../../include/Pal
                    ${CMAKE_CURRENT_SOURCE_DIR}/../../include/Reader
                    ${CMAKE_CURRENT_SOURCE_DIR}/../../include/Jit)

if ( WIN32 )
  set(CMAKE_CXX_FLAGS "-EHsc")
endif()
