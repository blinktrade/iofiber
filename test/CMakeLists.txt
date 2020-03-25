cmake_minimum_required(VERSION 3.7.2)
project(iofiber)

# Dependencies
find_package(Boost 1.70 COMPONENTS
  system
  context
  regex
  unit_test_framework
  REQUIRED)
find_package(Threads)

if(CMAKE_CXX_COMPILER_ID MATCHES "(Clang|GNU)")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra")
endif()

include_directories(
  "${CMAKE_CURRENT_SOURCE_DIR}/../include"
  "${Boost_INCLUDE_DIR}"
  )

enable_testing()

macro(add_test_target target target_SRC)
  add_executable("${target}" "${target}.cpp" ${target_SRC})

  set_property(TARGET "${target}" PROPERTY CXX_STANDARD 14)
  set_property(TARGET "${target}" PROPERTY CXX_STANDARD_REQUIRED ON)

  target_compile_definitions("${target}"
    PRIVATE BOOST_ASIO_NO_DEPRECATED BOOST_ASIO_NO_DYNAMIC_BUFFER_V1)

  target_link_libraries("${target}"
    ${CMAKE_THREAD_LIBS_INIT}
    ${Boost_SYSTEM_LIBRARY}
    ${Boost_CONTEXT_LIBRARY}
    ${Boost_REGEX_LIBRARY}
    ${Boost_UNIT_TEST_FRAMEWORK_LIBRARY})

  add_test(NAME "${target}" COMMAND $<TARGET_FILE:${target}>
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
endmacro()

add_test_target("fiber" "")
add_test_target("sync" "")

include(CTest)
