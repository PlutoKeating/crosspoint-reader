add_executable(ProjectStickCoreTest
  ProjectStickCoreTest.cpp
  ${REPO_ROOT}/lib/ProjectStick/ProjectStickCore.cpp
)

target_include_directories(ProjectStickCoreTest PRIVATE
  ${REPO_ROOT}/lib/ProjectStick
)

target_link_libraries(ProjectStickCoreTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ProjectStickCoreTest)
