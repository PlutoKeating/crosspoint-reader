add_executable(BuiltinCjkTest
  BuiltinCjkTest.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFont.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(BuiltinCjkTest PRIVATE
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/Utf8
)

target_link_libraries(BuiltinCjkTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BuiltinCjkTest)
