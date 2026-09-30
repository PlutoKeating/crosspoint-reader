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

target_sources(ProjectStickCoreTest PRIVATE ${REPO_ROOT}/lib/ProjectStick/StudioProgram.cpp StudioProgramTest.cpp)
target_sources(ProjectStickCoreTest PRIVATE ${REPO_ROOT}/lib/ProjectStick/StickFirmware.cpp StickFirmwareTest.cpp)

# BLE setup protocol 3: crypto via OpenSSL on the host.
find_package(OpenSSL REQUIRED)
target_sources(ProjectStickCoreTest PRIVATE ${REPO_ROOT}/lib/ProjectStick/BleSetupProtocol.cpp
               ${REPO_ROOT}/lib/ProjectStick/BleCrypto.cpp BleSetupProtocolTest.cpp)
target_compile_definitions(ProjectStickCoreTest PRIVATE BLE_CRYPTO_OPENSSL)
target_link_libraries(ProjectStickCoreTest PRIVATE OpenSSL::Crypto)

# Shared cloud API backoff (header-only).
target_sources(ProjectStickCoreTest PRIVATE ApiBackoffTest.cpp)
