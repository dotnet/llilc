project(coredistools)

get_filename_component(LLILC_INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../../include ABSOLUTE)

include_directories(${LLVM_INCLUDE_DIRS}
                    ${LLILC_INCLUDES}/CoreDisTools)

add_definitions(${LLVM_DEFINITIONS})

if (WIN32)
  # Create .def file containing a list of exports preceeded by
  # 'EXPORTS'.  The file "coredistools.exports" already contains the list, 
  # so we massage it into the correct format here to create 
  # "coredistools.exports.def".
  set(COREDISTOOLS_EXPORTS_DEF ${CMAKE_CURRENT_BINARY_DIR}/coredistools.exports.def)
  set(COREDISTOOLS_EXPORTS_DEF_TEMP ${COREDISTOOLS_EXPORTS_DEF}.txt)
  file(READ "coredistools.exports" exports_list)
  file(WRITE ${COREDISTOOLS_EXPORTS_DEF_TEMP} "LIBRARY COREDISTOOLS\n")
  file(APPEND ${COREDISTOOLS_EXPORTS_DEF_TEMP} "EXPORTS\n")
  file(APPEND ${COREDISTOOLS_EXPORTS_DEF_TEMP} ${exports_list})
  # Copy the file only if it has changed.
  execute_process(COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${COREDISTOOLS_EXPORTS_DEF_TEMP} ${COREDISTOOLS_EXPORTS_DEF})
endif()

# Now build our tools
add_library(coredistools
   SHARED
   coredistools.cpp
   ${COREDISTOOLS_EXPORTS_DEF}
)

# Find the libraries that correspond to the LLVM components
# that we wish to use
llvm_map_components_to_libnames(llvm_libs
  ${LLVM_TARGETS_TO_BUILD}
)

# Link against LLVM libraries
target_link_libraries(coredistools
${llvm_libs})

# Output the export header corresponding to the coredistools library
add_custom_command(TARGET coredistools POST_BUILD 
  COMMAND ${CMAKE_COMMAND} -E copy_if_different 
  ${LLILC_INCLUDES}/CoreDisTools/coredistools.h 
  $<TARGET_FILE_DIR:coredistools>/include/coredistools.h)
