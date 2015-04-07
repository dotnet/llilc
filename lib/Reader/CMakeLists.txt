get_filename_component(LLILC_INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../../include ABSOLUTE)

include_directories(${LLILC_INCLUDES}/clr
                    ${LLILC_INCLUDES}/Pal
                    ${LLILC_INCLUDES}/Reader
                    ${LLILC_INCLUDES}/Jit)

if( WIN32 )
  set(CMAKE_CXX_FLAGS "-EHsc")
endif()

add_llilcjit_library(LLILCReader
  abi.cpp
  abisignature.cpp
  reader.cpp
  readerir.cpp
  GenIRStubs.cpp
  )

if( NOT LLILC_BUILT_STANDALONE )
  add_dependencies(LLILCReader intrinsics_gen)
endif()
