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

# Build every BENCH configuration declared in an example's `bench` sidecar.
# Gates never build, and until NEW-45 nothing built a bench directory either:
# display/acid_box's M2_BT_OUT builds sat broken for two days before an unrelated
# workstream happened to rebuild them.  Run at close-out, NEVER concurrently with
# the QEMU sweep -- CLAUDE.md records a sweep invalidated by a concurrent licence
# audit, and this is heavier than that.
add_custom_target(bench_check
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tools/build-bench-configs.sh" -n
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    USES_TERMINAL
    COMMENT "Building every declared bench configuration..."
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
