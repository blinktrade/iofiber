project(examples)

cmake_minimum_required(VERSION 3.1.0)

find_package(Boost 1.70 COMPONENTS
  context
  REQUIRED)

find_package(Threads)

link_directories("${target}" ${Boost_LIBRARIES_DIRS})

set(examples
  "sleepsort"
  "sleepsort-interruptable"
)

macro(add_example_target target)
  add_executable("${target}" "${target}.cpp")

  set_property(TARGET "${target}" PROPERTY CXX_STANDARD 11)
  set_property(TARGET "${target}" PROPERTY CXX_STANDARD_REQUIRED ON)

  target_include_directories("${target}"
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../include" ${Boost_INCLUDE_DIR})

  target_link_libraries("${target}"
    ${Boost_CONTEXT_LIBRARY}
    ${CMAKE_THREAD_LIBS_INIT})
endmacro()

foreach(example ${examples})
  add_example_target("${example}")
endforeach()
