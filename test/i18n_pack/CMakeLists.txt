# Regenerate the built-in catalogue (gitignored, same location as the firmware
# build) so the test never depends on a prior PlatformIO build.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(I18N_DIR ${REPO_ROOT}/lib/I18n)
file(GLOB I18N_TRANSLATIONS ${I18N_DIR}/translations/*.yaml)
add_custom_command(
  OUTPUT ${I18N_DIR}/I18nKeys.h ${I18N_DIR}/I18nStrings.h ${I18N_DIR}/I18nStrings.cpp
  COMMAND ${Python3_EXECUTABLE} ${REPO_ROOT}/scripts/gen_i18n.py ${I18N_DIR}/translations ${I18N_DIR}
          --src-dirs src lib
  DEPENDS ${REPO_ROOT}/scripts/gen_i18n.py ${I18N_TRANSLATIONS}
  WORKING_DIRECTORY ${REPO_ROOT}
)

add_executable(I18nPackTest
  I18nPackTest.cpp
  ${I18N_DIR}/I18n.cpp
  ${I18N_DIR}/I18nStrings.cpp
)

target_include_directories(I18nPackTest PRIVATE ${I18N_DIR})

target_link_libraries(I18nPackTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(I18nPackTest)

# Every built-in string must render with the UI font stack (Ubuntu with the
# Noto Sans SC 12 fallback), or the device shows missing-glyph boxes.
target_sources(I18nPackTest PRIVATE
  BuiltinCoverageTest.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFont.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)
target_include_directories(I18nPackTest PRIVATE ${REPO_ROOT}/lib/EpdFont ${REPO_ROOT}/lib/Utf8)
