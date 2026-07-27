add_executable(ProjectStickCoreTest
  ProjectStickCoreTest.cpp
  ${REPO_ROOT}/lib/ProjectStick/ProjectStickCore.cpp
  ${REPO_ROOT}/lib/ProjectStick/ProjectStickStream.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
)

target_include_directories(ProjectStickCoreTest PRIVATE
  ${REPO_ROOT}/lib/ProjectStick
  ${REPO_ROOT}/lib/JsonParser
)

target_link_libraries(ProjectStickCoreTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ProjectStickCoreTest)
