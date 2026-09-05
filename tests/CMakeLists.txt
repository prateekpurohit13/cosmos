# Tests assert() their expectations, so NDEBUG would silently compile every check away and
# report a vacuous pass. CMake caches CMAKE_BUILD_TYPE across `just build` and `just test`,
# so a prior Release configure would otherwise leak NDEBUG into the test binaries.
add_compile_options(-UNDEBUG)

add_executable(test_wrap_malloc test_wrap_malloc.cpp)
target_link_libraries(test_wrap_malloc PRIVATE cosmos)
# White-box test: pins the internal memory eligibility policy from wrapper_fault.hpp.
target_include_directories(test_wrap_malloc PRIVATE ${CMAKE_SOURCE_DIR}/src/cosmos/wrappers)

if (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    # Linker --wrap substitutes malloc/free AFTER optimization, but GCC/Clang
    # treat them as builtins with no observable side effects. Under -O2 the
    # optimizers cache/reorder loads of sim-heap bookkeeping across the wrapped
    # calls, and clang folds out-of-bounds header reads as UB. -fno-builtin
    # makes the wrapped symbols opaque calls again. See docs/linker-interposition.md.
    target_compile_options(test_wrap_malloc PRIVATE
        "-fno-builtin-malloc"
        "-fno-builtin-free"
        "-fno-builtin-calloc"
        "-fno-builtin-realloc"
    )
    target_link_options(test_wrap_malloc PRIVATE
        "-Wl,--gc-sections"
        "-Wl,--wrap=malloc"
        "-Wl,--wrap=free"
        "-Wl,--wrap=calloc"
        "-Wl,--wrap=realloc"
    )
endif()

add_test(NAME test_wrap_malloc COMMAND test_wrap_malloc)

add_executable(test_random test_random.cpp)
target_link_libraries(test_random PRIVATE cosmos)
add_test(NAME test_random COMMAND test_random)

add_executable(test_seed_derivation test_seed_derivation.cpp)
target_link_libraries(test_seed_derivation PRIVATE cosmos)
add_test(NAME test_seed_derivation COMMAND test_seed_derivation)

add_executable(test_time test_time.cpp)
target_link_libraries(test_time PRIVATE cosmos)

if (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(test_time PRIVATE
        "-fno-builtin-malloc"
        "-fno-builtin-free"
    )
    target_link_options(test_time PRIVATE
        "-Wl,--gc-sections"
        "-Wl,--wrap=malloc"
        "-Wl,--wrap=free"
        "-Wl,--wrap=clock_gettime"
        "-Wl,--wrap=gettimeofday"
        "-Wl,--wrap=nanosleep"
        "-Wl,--wrap=clock_nanosleep"
    )
endif()

add_test(NAME test_time COMMAND test_time)

add_executable(test_faults test_faults.cpp)
target_link_libraries(test_faults PRIVATE cosmos)
add_test(NAME test_faults COMMAND test_faults)

add_executable(test_faultinjection_phase1_gate test_faultinjection_phase1_gate.cpp)
target_link_libraries(test_faultinjection_phase1_gate PRIVATE cosmos)
add_test(NAME test_faultinjection_phase1_gate COMMAND test_faultinjection_phase1_gate)

add_executable(test_ledger test_ledger.cpp)
target_link_libraries(test_ledger PRIVATE cosmos)
add_test(NAME test_ledger COMMAND test_ledger)

add_executable(test_fault_draw test_fault_draw.cpp)
target_link_libraries(test_fault_draw PRIVATE cosmos)
add_test(NAME test_fault_draw COMMAND test_fault_draw)

add_executable(test_fault_injector test_fault_injector.cpp)
target_link_libraries(test_fault_injector PRIVATE cosmos)
add_test(NAME test_fault_injector COMMAND test_fault_injector)

add_executable(test_simulator test_simulator.cpp)
target_link_libraries(test_simulator PRIVATE cosmos)

if (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    # Same reason as test_wrap_malloc above: the wrapped symbols must stay opaque calls.
    target_compile_options(test_simulator PRIVATE
        "-fno-builtin-malloc"
        "-fno-builtin-free"
    )
    target_link_options(test_simulator PRIVATE
        "-Wl,--gc-sections"
        "-Wl,--wrap=malloc"
        "-Wl,--wrap=free"
    )
endif()

add_test(NAME test_simulator COMMAND test_simulator)

add_executable(test_wrap_random test_wrap_random.cpp)
target_link_libraries(test_wrap_random PRIVATE cosmos)
# White-box test: exercises the shared reentrancy guard from wrapper_fault.hpp.
target_include_directories(test_wrap_random PRIVATE ${CMAKE_SOURCE_DIR}/src/cosmos/wrappers)

if (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    # Wrapped symbols must stay opaque calls; random-family builtins would otherwise fold.
    target_compile_options(test_wrap_random PRIVATE
        "-fno-builtin-malloc"
        "-fno-builtin-free"
        "-fno-builtin-calloc"
        "-fno-builtin-realloc"
        "-fno-builtin-random"
        "-fno-builtin-rand"
        "-fno-builtin-srand"
        "-fno-builtin-srandom"
    )
    target_link_options(test_wrap_random PRIVATE
        "-Wl,--gc-sections"
        "-Wl,--wrap=malloc"
        "-Wl,--wrap=free"
        "-Wl,--wrap=calloc"
        "-Wl,--wrap=realloc"
        "-Wl,--wrap=getrandom"
        "-Wl,--wrap=random"
        "-Wl,--wrap=rand"
        "-Wl,--wrap=srandom"
        "-Wl,--wrap=srand"
    )
endif()

add_test(NAME test_wrap_random COMMAND test_wrap_random)

add_executable(test_wrap_storage test_wrap_storage.cpp)
target_link_libraries(test_wrap_storage PRIVATE cosmos)
# White-box test: pins the internal storage eligibility policy from wrapper_fault.hpp.
target_include_directories(test_wrap_storage PRIVATE ${CMAKE_SOURCE_DIR}/src/cosmos/wrappers)

if (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    # Wrapped symbols must stay opaque calls so the linker substitution reaches them.
    target_compile_options(test_wrap_storage PRIVATE
        "-fno-builtin-malloc"
        "-fno-builtin-free"
        "-fno-builtin-calloc"
        "-fno-builtin-realloc"
        "-fno-builtin-open"
        "-fno-builtin-read"
        "-fno-builtin-write"
        "-fno-builtin-fsync"
    )
    target_link_options(test_wrap_storage PRIVATE
        "-Wl,--gc-sections"
        "-Wl,--wrap=malloc"
        "-Wl,--wrap=free"
        "-Wl,--wrap=calloc"
        "-Wl,--wrap=realloc"
        "-Wl,--wrap=open"
        "-Wl,--wrap=read"
        "-Wl,--wrap=write"
        "-Wl,--wrap=fsync"
    )
endif()

add_test(NAME test_wrap_storage COMMAND test_wrap_storage)
