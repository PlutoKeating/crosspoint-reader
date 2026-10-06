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

# Heap/failure/deadline policy for cloud requests (header-only).
target_sources(ProjectStickCoreTest PRIVATE NetPolicyTest.cpp)

# ProjectStickHost page decisions (header-only).
target_sources(ProjectStickCoreTest PRIVATE HostPolicyTest.cpp)

# Wi-Fi on demand (header-only).
target_sources(ProjectStickCoreTest PRIVATE WifiPolicyTest.cpp)

# Hot paths must not allocate (2.2.2 crash: bad_alloc in now()).
target_sources(ProjectStickCoreTest PRIVATE AllocationFreeTest.cpp)

# BLE transfer protocol 4 (lib/ProjectStick/StudioTransfer): vectors shared
# with the mini program (BleV4Vectors.h, gen_ble_v4_vectors.py); uzlib inflate.
target_sources(ProjectStickCoreTest PRIVATE ${REPO_ROOT}/lib/ProjectStick/StudioTransfer.cpp
               ${REPO_ROOT}/lib/uzlib/src/tinflate.c StudioTransferTest.cpp)
target_include_directories(ProjectStickCoreTest PRIVATE ${REPO_ROOT}/lib/uzlib/src)

# Firmware over BLE (op fw4, lib/ProjectStick/FirmwareTransfer): the mini
# program's vector, read from fixtures/ at run time.
target_sources(ProjectStickCoreTest PRIVATE ${REPO_ROOT}/lib/ProjectStick/FirmwareTransfer.cpp FirmwareTransferTest.cpp)
target_compile_definitions(ProjectStickCoreTest PRIVATE
                           FW4_VECTOR_PATH="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/ble-fw4-vector.json")
