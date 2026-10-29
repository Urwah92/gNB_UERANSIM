add_executable(rls-codec-tests rls_codec_test.cpp)
target_link_libraries(rls-codec-tests PRIVATE common-lib)
target_compile_options(rls-codec-tests PRIVATE -Wall -Wextra -pedantic)
add_test(NAME rls-codec COMMAND rls-codec-tests)

add_executable(mobility-tests mobility_test.cpp)
target_include_directories(mobility-tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_options(mobility-tests PRIVATE -Wall -Wextra -pedantic)
add_test(NAME mobility COMMAND mobility-tests)

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME build-relocation COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/build_relocation_test.py
        ${PROJECT_SOURCE_DIR}/tools/configure-build.sh)
    set_tests_properties(build-relocation PROPERTIES TIMEOUT 30)
    add_test(NAME ue-config COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/ue_config_test.py
        $<TARGET_FILE:nr-ue>
        ${PROJECT_SOURCE_DIR}/config/examples/ue-5g.yaml
        ${PROJECT_SOURCE_DIR}/config/examples/ue-wifi.yaml)
    foreach(role source-gnb target-gnb)
        add_test(NAME config-${role} COMMAND nr-gnb --check-config -c ${PROJECT_SOURCE_DIR}/config/examples/${role}.yaml)
    endforeach()
    add_test(NAME config-wifi-ap COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/gnb_config_test.py
        $<TARGET_FILE:nr-gnb> ${PROJECT_SOURCE_DIR}/config/examples/wifi-ap.yaml)

    option(UERANSIM_NETWORK_TESTS "Test AP/UE using real loopback UDP with all host network commands mocked" OFF)
    if(UERANSIM_NETWORK_TESTS)
        add_test(NAME wifi-runtime COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/wifi_runtime_test.py
            $<TARGET_FILE:nr-gnb> $<TARGET_FILE:nr-ue>)
        set_tests_properties(wifi-runtime PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE LABELS "runtime;loopback")
        add_test(NAME wifi-startup COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/wifi_startup_test.py
            $<TARGET_FILE:nr-gnb> $<TARGET_FILE:nr-ue>)
        set_tests_properties(wifi-startup PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE LABELS "runtime;loopback")
    endif()
endif()

add_executable(gnb-handover-tests gnb_handover_test.cpp)
target_link_libraries(gnb-handover-tests PRIVATE gnb pthread)
target_compile_options(gnb-handover-tests PRIVATE -Wall -Wextra -pedantic)
add_test(NAME gnb-handover COMMAND gnb-handover-tests)

add_executable(ue-handover-tests ue_handover_test.cpp)
target_link_libraries(ue-handover-tests PRIVATE ue pthread)
target_compile_options(ue-handover-tests PRIVATE -Wall -Wextra -pedantic)
add_test(NAME ue-handover COMMAND ue-handover-tests)

add_executable(process-tests process_test.cpp)
target_link_libraries(process-tests PRIVATE utils pthread)
target_compile_options(process-tests PRIVATE -Wall -Wextra -pedantic)
add_test(NAME process COMMAND process-tests)
