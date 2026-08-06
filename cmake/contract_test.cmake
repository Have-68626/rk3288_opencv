# Q-CQ-B06-003 编译器契约测试：带异常语法（throw）目标在 -fno-exceptions 下
# 必须编译失败，允许异常时编译成功——flag 与语法一致性的回归保护。
# 运行：cmake -DCMAKE_CXX_COMPILER=<compiler> -P cmake/contract_test.cmake
if(NOT DEFINED CMAKE_CXX_COMPILER)
    set(CMAKE_CXX_COMPILER "c++")
endif()

set(_work "${CMAKE_CURRENT_LIST_DIR}/../build/contract_test_build")
file(MAKE_DIRECTORY "${_work}")
set(_src "${_work}/throw_test.cpp")
file(WRITE "${_src}" "void f() { throw 1; }\n")

# -fno-exceptions 下应失败（无法使用异常语法）
execute_process(COMMAND ${CMAKE_CXX_COMPILER} -std=c++17 -fno-exceptions -c "${_src}" -o "${_work}/noexc.o"
    RESULT_VARIABLE _noexc_rc OUTPUT_QUIET ERROR_QUIET)
# 默认（允许异常）下应成功
execute_process(COMMAND ${CMAKE_CXX_COMPILER} -std=c++17 -c "${_src}" -o "${_work}/exc.o"
    RESULT_VARIABLE _exc_rc OUTPUT_QUIET ERROR_QUIET)

file(REMOVE_RECURSE "${_work}")

if(_noexc_rc EQUAL 0)
    message(FATAL_ERROR "CONTRACT_TEST_FAIL: -fno-exceptions 未阻止异常语法 throw")
endif()
if(NOT _exc_rc EQUAL 0)
    message(FATAL_ERROR "CONTRACT_TEST_FAIL: 允许异常时 throw 编译失败 rc=${_exc_rc}")
endif()
message(STATUS "CONTRACT_TEST_PASS: -fno-exceptions 阻止 throw，默认允许")
