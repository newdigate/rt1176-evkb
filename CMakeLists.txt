cmake_minimum_required(VERSION 3.20)
project(rt1170_evkb_root NONE)

# Optional arguments forwarded to tools/run-all-qemu-gates.sh (e.g. -DQEMU_GATE_ARGS="blink")
set(QEMU_GATE_ARGS "" CACHE STRING "Arguments passed to run-all-qemu-gates.sh")

# Build missing example ELFs and execute all QEMU gates
add_custom_target(qemu_run_all
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tools/run-all-qemu-gates.sh" -b ${QEMU_GATE_ARGS}
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    USES_TERMINAL
    COMMENT "Building missing targets and running QEMU gates..."
)

# Run QEMU gates without auto-building missing targets
add_custom_target(qemu_check
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tools/run-all-qemu-gates.sh" ${QEMU_GATE_ARGS}
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    USES_TERMINAL
    COMMENT "Running QEMU gates against pre-built targets..."
)

# Build all example ELFs without executing QEMU gates
add_custom_target(build_all_examples
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tools/run-all-qemu-gates.sh" -b --build-only ${QEMU_GATE_ARGS}
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    USES_TERMINAL
    COMMENT "Building all example ELFs across boards..."
)

# CTest integration
include(CTest)
if(BUILD_TESTING)
    add_test(
        NAME qemu_gates
        COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tools/run-all-qemu-gates.sh"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    )
endif()
