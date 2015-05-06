get_filename_component(LLILC_INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../../include ABSOLUTE)

include_directories(${LLILC_INCLUDES}/clr
                    ${LLILC_INCLUDES}/GcInfo
                    ${LLILC_INCLUDES}/Pal)

if( WIN32 )
  set(CMAKE_CXX_FLAGS "-EHsc")
endif()
					
if ("${CMAKE_SIZEOF_VOID_P}" EQUAL "8")
  add_definitions(-D_TARGET_AMD64_=1)
endif()
					
if(CLR_CMAKE_PLATFORM_UNIX)
    add_compile_options(-fPIC)
endif(CLR_CMAKE_PLATFORM_UNIX)

add_library(GcInfo
    STATIC
	gcinfoencoder.cpp
)
