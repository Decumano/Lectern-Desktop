include(CMakeFindDependencyMacro)

####### Expanded from @PACKAGE_INIT@ by configure_package_config_file() #######
####### Any changes to this file will be overwritten by the next CMake run ####
####### The input file was cprConfig-ssl.cmake.in                            ########

get_filename_component(PACKAGE_PREFIX_DIR "${CMAKE_CURRENT_LIST_DIR}/../../" ABSOLUTE)

macro(set_and_check _var _file)
  set(${_var} "${_file}")
  if(NOT EXISTS "${_file}")
    message(FATAL_ERROR "File or directory ${_file} referenced by variable ${_var} does not exist !")
  endif()
endmacro()

macro(check_required_components _NAME)
  foreach(comp ${${_NAME}_FIND_COMPONENTS})
    if(NOT ${_NAME}_${comp}_FOUND)
      if(${_NAME}_FIND_REQUIRED_${comp})
        set(${_NAME}_FOUND FALSE)
      endif()
    endif()
  endforeach()
endmacro()

####################################################################################

find_dependency(Threads REQUIRED)
if("BUILTIN" STREQUAL "BOOST_ASIO")
    # A caller-owned io_context crosses the compiled cpr boundary. Match the
    # Boost headers used to build cpr rather than mixing context definitions.
    set(_cpr_boost_components)
    if("OFF")
        list(APPEND _cpr_boost_components filesystem)
    endif()
    find_dependency(Boost  EXACT REQUIRED COMPONENTS ${_cpr_boost_components})
    unset(_cpr_boost_components)
elseif("OFF")
    find_dependency(Boost 1.77 REQUIRED COMPONENTS filesystem)
endif()
find_dependency(CURL REQUIRED)
find_dependency(OpenSSL REQUIRED)

include(${CMAKE_CURRENT_LIST_DIR}/cprTargets.cmake)

check_required_components(cpr)
