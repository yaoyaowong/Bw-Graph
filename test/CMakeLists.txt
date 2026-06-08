# Find all test files recursively (fixed: proper GLOB_RECURSE pattern)
file(GLOB_RECURSE TEST_FILES 
    "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/**/test_*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/**/test_*.cpp"
)

# Alternative approach - explicitly search in common test directories
if(NOT TEST_FILES)
    # If no files found with recursive search, try specific directories
    file(GLOB TEST_FILES_CC "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cc")
    file(GLOB TEST_FILES_CPP "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp")
    file(GLOB TEST_SUBDIRS_CC "${CMAKE_CURRENT_SOURCE_DIR}/*/test_*.cc")
    file(GLOB TEST_SUBDIRS_CPP "${CMAKE_CURRENT_SOURCE_DIR}/*/test_*.cpp")
    
    # Combine all found files
    set(TEST_FILES ${TEST_FILES_CC} ${TEST_FILES_CPP} ${TEST_SUBDIRS_CC} ${TEST_SUBDIRS_CPP})
endif()

# Debug: Print found test files
message(STATUS "Found test files:")
foreach(TEST_FILE ${TEST_FILES})
    message(STATUS "  ${TEST_FILE}")
endforeach()

# Process each test file
foreach(TEST_FILE ${TEST_FILES})
    # Extract filename without extension as test name
    get_filename_component(TEST_NAME ${TEST_FILE} NAME_WE)
    
    # Create executable using just the filename
    add_executable(${TEST_NAME} ${TEST_FILE})
    
    # Link against bwgraph library and GTest
    target_link_libraries(${TEST_NAME} PRIVATE 
        bwgraph
        GTest::gtest_main
        GTest::gtest
        Threads::Threads
    )
    
    # Add include directories for headers
    target_include_directories(${TEST_NAME} PRIVATE 
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/include
    )
    
    # Register test with CTest
    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
    
    message(STATUS "Added test: ${TEST_NAME}")
endforeach()

# If no test files found, show warning with helpful info
if(NOT TEST_FILES)
    message(WARNING "No test files found in ${CMAKE_CURRENT_SOURCE_DIR}")
    message(STATUS "Current source dir: ${CMAKE_CURRENT_SOURCE_DIR}")
    message(STATUS "Looking for files matching: test_*.cc or test_*.cpp")
    
    # List all files in current directory for debugging
    file(GLOB ALL_FILES "${CMAKE_CURRENT_SOURCE_DIR}/*")
    message(STATUS "All files in current directory:")
    foreach(FILE ${ALL_FILES})
        message(STATUS "  ${FILE}")
    endforeach()
endif()