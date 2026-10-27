add_library(cosmos STATIC
    cosmos.cpp
    coverage.cpp
    task.cpp
    net.cpp
    storage.cpp
    snapshot.cpp
    wrappers/wrap_memory.cpp
    wrappers/wrap_threads.cpp
    wrappers/wrap_time.cpp
    wrappers/wrap_net.cpp
    wrappers/wrap_storage.cpp
    wrappers/wrap_random.cpp
)

target_include_directories(cosmos PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# Each __wrap_* symbol must live in its own linker section so that the sim
# binary's -Wl,--gc-sections can discard wrappers for POSIX functions the
# application never calls. This is what makes SUBSET wrapping work: pulling
# __wrap_malloc must not drag in __wrap_calloc (and its __real_calloc ref),
# which would otherwise be an undefined symbol unless --wrap=calloc is also
# passed. See docs/design.md §2 (build matrix) and docs/architecture.md §3.
target_compile_options(cosmos PRIVATE -ffunction-sections -fdata-sections)

# Level-2 exploration needs compiler-assisted coverage. GCC has no trace-pc-guard; it uses the
# return-address form, while Clang uses guard ids. Both implementations live in coverage.cpp. The
# INTERFACE option instruments application and test code that links libcosmos; production binaries
# do not link it and therefore pay nothing.
set(COSMOS_COVERAGE_FLAG "")
set(COSMOS_COVERAGE_DISABLE_FLAG "")
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(COSMOS_COVERAGE_FLAG "-fsanitize-coverage=trace-pc-guard")
    set(COSMOS_COVERAGE_DISABLE_FLAG "-fno-sanitize-coverage=trace-pc-guard")
    target_compile_definitions(cosmos PUBLIC COSMOS_COVERAGE_GUARD=1)
elseif (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(COSMOS_COVERAGE_FLAG "-fsanitize-coverage=trace-pc")
    set(COSMOS_COVERAGE_DISABLE_FLAG "-fno-sanitize-coverage=trace-pc")
    target_compile_definitions(cosmos PUBLIC COSMOS_COVERAGE_PC=1)
endif()

if (COSMOS_COVERAGE_FLAG)
    # Instrument consumers (applications, tests, tools), not libcosmos itself. The corpus is a
    # map of application control flow; instrumenting the wrappers would also make partial-wrap
    # test links retain unused wrapper sections and their __real_* references.
    target_compile_options(cosmos INTERFACE ${COSMOS_COVERAGE_FLAG})
    # The callbacks cannot be instrumented, or trace-pc would recurse into itself. The source
    # property is appended after any target options, so the disable flag wins in both compilers.
    set_source_files_properties(coverage.cpp PROPERTIES
        COMPILE_OPTIONS "${COSMOS_COVERAGE_DISABLE_FLAG}")
endif()

# GCC/Clang treat malloc/free as builtins with known semantics and delete pairs they can prove
# are dead, so at -O1 and above the call never reaches __wrap_malloc and the allocation is
# silently untracked. They can also fold malloc+memset(0) into calloc, bypassing the tracked
# path's OOM decision point. INTERFACE so every binary linking cosmos inherits it, not just
# this library.
target_compile_options(cosmos INTERFACE
    -fno-builtin-malloc
    -fno-builtin-free
    -fno-builtin-calloc
    -fno-builtin-realloc
    -fno-builtin-random
    -fno-builtin-rand
    -fno-builtin-srand
    -fno-builtin-srandom
)
