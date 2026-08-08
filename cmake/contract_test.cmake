# Q-CQ-B06-003 编译器契约测试：带异常语法（throw）目标在异常禁用 flag 下
# 必须编译失败（Clang/GNU -fno-exceptions），MSVC 下必须触发 C4530 警告 ——
# flag 与语法一致性的回归保护。
# 运行：cmake -DCMAKE_CXX_COMPILER=<compiler> -DRK_CONTRACT_MSVC=<0|1>
#       [-DWORK_DIR=<构建树子目录>] -P cmake/contract_test.cmake
# clang-cl（MSVC=TRUE 但 CMAKE_CXX_COMPILER_ID=Clang）：/EH- 对 throw 是硬错误而非
# C4530 警告，故调用方（cmake/core.cmake）不传 RK_CONTRACT_MSVC=1，走 Clang/GNU 分支（RW8 ③）。
if(NOT DEFINED CMAKE_CXX_COMPILER)
    set(CMAKE_CXX_COMPILER "c++")
endif()

# 工作目录：优先取调用方传入的构建树目录（见 cmake/core.cmake 的 add_test -DWORK_DIR），
# 不在源码树内写死 build/；独立运行（未传 WORK_DIR）时回退到系统临时目录。
if(DEFINED WORK_DIR)
    set(_work "${WORK_DIR}")
else()
    set(_work "$ENV{TEMP}/rk_contract_test_build")
    if(_work STREQUAL "/rk_contract_test_build")
        set(_work "/tmp/rk_contract_test_build")
    endif()
endif()
file(MAKE_DIRECTORY "${_work}")
set(_src "${_work}/throw_test.cpp")
# 同时含 throw 与 try/catch：MSVC C4530 针对"异常处理程序未启用展开语义"，
# 仅 throw（现代 cl 19.44 默认展开）不触发，try/catch 才稳定判别。
file(WRITE "${_src}" "void f() { try { throw 1; } catch (...) { } }\n")

if(RK_CONTRACT_MSVC)
    # MSVC 契约探针（B06-004）：/EHs-（显式禁用 C++ 异常展开）下 throw 必触发
    # C4530 警告；/EHsc（项目实际启用，见 rk_apply_compiler_contract）下必须无 C4530。
    # 注：/EH- 不是合法 MSVC 选项（cl 19.x 报 D9002 忽略）；cl 19.44 缺省 /EH 已默认
    # 展开异常（不报 C4530），须用 /EHs- 显式禁用才能判别。
    # 源无头文件依赖，直接调用 cl.exe 完整路径即可，无需 vcvars 环境。
    execute_process(COMMAND ${CMAKE_CXX_COMPILER} /std:c++17 /EHs- /c "${_src}" /Fo${_work}/noexc.obj
        RESULT_VARIABLE _noexc_rc OUTPUT_VARIABLE _noexc_out ERROR_VARIABLE _noexc_err)
    execute_process(COMMAND ${CMAKE_CXX_COMPILER} /std:c++17 /EHsc /c "${_src}" /Fo${_work}/exc.obj
        RESULT_VARIABLE _exc_rc OUTPUT_VARIABLE _exc_out ERROR_VARIABLE _exc_err)
    string(FIND "${_noexc_out}${_noexc_err}" "C4530" _noexc_c4530)
    string(FIND "${_exc_out}${_exc_err}" "C4530" _exc_c4530)
    # 两次编译均应成功（/EHs- 仅警告不报错）；C4530 只允许出现在 /EHs- 下。
    # 同时校验 rc，避免 cl 无法运行（缺 vcvars 等）时误判为通过。
    if(NOT _noexc_rc EQUAL 0 OR _noexc_c4530 EQUAL -1)
        message(FATAL_ERROR "CONTRACT_TEST_FAIL: /EHs- 未触发 C4530（rc=${_noexc_rc}）\n诊断输出:\n${_noexc_out}${_noexc_err}")
    endif()
    if(NOT _exc_rc EQUAL 0 OR NOT _exc_c4530 EQUAL -1)
        message(FATAL_ERROR "CONTRACT_TEST_FAIL: /EHsc 下 throw 编译未保持干净（rc=${_exc_rc}）\n诊断输出:\n${_exc_out}${_exc_err}")
    endif()
else()
    # Clang/GNU：-fno-exceptions 下 throw 必须编译失败；默认（允许异常）下必须成功。
    # 编译诊断捕获到变量，仅断言失败时回显 —— 成功运行保持静默。
    execute_process(COMMAND ${CMAKE_CXX_COMPILER} -std=c++17 -fno-exceptions -c "${_src}" -o "${_work}/noexc.o"
        RESULT_VARIABLE _noexc_rc OUTPUT_VARIABLE _noexc_out ERROR_VARIABLE _noexc_err)
    execute_process(COMMAND ${CMAKE_CXX_COMPILER} -std=c++17 -c "${_src}" -o "${_work}/exc.o"
        RESULT_VARIABLE _exc_rc OUTPUT_VARIABLE _exc_out ERROR_VARIABLE _exc_err)
    if(_noexc_rc EQUAL 0)
        message(FATAL_ERROR "CONTRACT_TEST_FAIL: -fno-exceptions 未阻止异常语法 throw\n诊断输出:\n${_noexc_out}${_noexc_err}")
    endif()
    if(NOT _exc_rc EQUAL 0)
        message(FATAL_ERROR "CONTRACT_TEST_FAIL: 允许异常时 throw 编译失败 rc=${_exc_rc}\n诊断输出:\n${_exc_out}${_exc_err}")
    endif()
endif()

# 全部断言通过后才清理工作目录 —— 失败时 FATAL_ERROR 提前退出，保留证据供诊断
file(REMOVE_RECURSE "${_work}")
if(RK_CONTRACT_MSVC)
    message(STATUS "CONTRACT_TEST_PASS: /EHs- 触发 C4530，/EHsc 干净")
else()
    message(STATUS "CONTRACT_TEST_PASS: -fno-exceptions 阻止 throw，默认允许")
endif()
