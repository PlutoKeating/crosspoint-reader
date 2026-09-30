#include <gtest/gtest.h>

#include "BleSetupProtocol.h"

// Vectors shared with the mini program (generated from the spec with Python's
// hmac/cryptography; see Project.StockStick docs/product/BLE-SETUP.md).
namespace {
const std::string K = "000102030405060708090a0b0c0d0e0f";
const std::string N = "a0a1a2a3a4a5a6a7a8a9aaabacadaeaf";
const std::string DEVICE = "6fbf4024-92c6-405d-85a0-a859649e2a5d";
const std::string SECRET(64, '1');
}  // namespace

TEST(BleSetupProtocol, SetupAndScanProofs) {
  EXPECT_EQ(ble_setup::mac(K, ble_setup::setupProofMessage(N, DEVICE)),
            "40e4f652d95fecf521331ea97f8d9bbc82d06a0cfcc063f7e38adca54c099b83");
  EXPECT_EQ(ble_setup::mac(K, ble_setup::scanMessage(N)),
            "ee4e73f405bd9cf3d57dd2cbabd943501a82f2395965bfc5ad55506e599a43ce");
  EXPECT_EQ(ble_setup::mac(SECRET, ble_setup::scanMessage(N)),
            "623f0f059fbfe276d5d01d88dc8e079eee07cdc2ed771d7b077546b789cba984");
  EXPECT_EQ(ble_setup::mac("", "x"), "");
  EXPECT_EQ(ble_setup::mac("0G", "x"), "");
}

TEST(BleSetupProtocol, WifiSealAndProof) {
  std::string pw;
  ASSERT_TRUE(ble_setup::seal(K, "wifi3", N, "pass word!", pw));
  EXPECT_EQ(pw, "4cc7bab35e5904d3e26a");
  EXPECT_EQ(ble_setup::mac(K, ble_setup::wifiMessage(N, "Home WiFi \xe5\xae\xb6", pw)),
            "2d1e5f86638b73e7718664c64b935f56e194e172c93ecab25b881dd031b0cc52");
  std::string plain;
  ASSERT_TRUE(ble_setup::unseal(K, "wifi3", N, pw, plain));
  EXPECT_EQ(plain, "pass word!");
  ASSERT_TRUE(ble_setup::seal(SECRET, "wifi3", N, "pass word!", pw));
  EXPECT_EQ(pw, "694b57273274cf14dd11");
  // Open network: empty password, empty ciphertext.
  ASSERT_TRUE(ble_setup::seal(K, "wifi3", N, "", pw));
  EXPECT_EQ(pw, "");
  EXPECT_EQ(ble_setup::mac(K, ble_setup::wifiMessage(N, "Open", "")),
            "8ae3d0180162bc1d22e37488725729ae12086de86ae9fe95ae6aa78248a22bc8");
  EXPECT_FALSE(ble_setup::unseal(K, "wifi3", "short", "00", plain));
}

TEST(BleSetupProtocol, BindCiphertextAndProof) {
  const std::string token = "AbCdEfGhIjKlMnOpQrStUvWxYz0123456789-_abcde";
  const std::string owner = "1d0e15e3-3577-4041-bd4e-73686756fdef";
  const std::string ct =
      "e5138299751735306284f821fbf2f52f2e6f0ae2a74815040ec5b1b886cde0a5bc62b7b91b41f32c6fe96e3fd2757c4821b51b2697f9fad"
      "eeb1f55519a6bbfd54273169bc7a23b8158f00e2fbd71467e109f459e45e2ddb4e528b2b4a6565e8f19d6d70215d390923668b77e";
  std::string sealed;
  ASSERT_TRUE(ble_setup::seal(K, "bind3", N, token + "|" + SECRET, sealed));
  EXPECT_EQ(sealed, ct);
  EXPECT_EQ(ble_setup::mac(K, ble_setup::bindMessage(N, owner, 3, ct)),
            "1fc7818920ad7655addb1726aa987fae2a3cedfb5e969067a33a622d3a3d9ed9");
  std::string plain, gotToken, gotSecret;
  ASSERT_TRUE(ble_setup::unseal(K, "bind3", N, ct, plain));
  ASSERT_TRUE(ble_setup::splitBindPlaintext(plain, gotToken, gotSecret));
  EXPECT_EQ(gotToken, token);
  EXPECT_EQ(gotSecret, SECRET);
  EXPECT_FALSE(ble_setup::splitBindPlaintext("short|" + SECRET, gotToken, gotSecret));
  EXPECT_FALSE(ble_setup::splitBindPlaintext(token + "|zz", gotToken, gotSecret));
}

TEST(BleSetupProtocol, OtaProof) {
  const std::string url = "https://stockstick.plutokeating.beer/firmware/2.4.0/stockstick-2.4.0.bin";
  std::string sha;
  for (int i = 0; i < 32; ++i) sha += "ab";
  const auto message = ble_setup::otaMessage(N, "2.4.0", sha, 3400469, url);
  EXPECT_EQ(message, "ota3|" + N + "|2.4.0|" + sha + "|3400469|" + url);
  EXPECT_EQ(ble_setup::mac(SECRET, message), "a379b3f844093dddde5017052e1e0108f899fbda3a36d91c0427dc0c6985e75b");
}

TEST(BleSetupProtocol, QrPayloadAndName) {
  const auto payload = ble_setup::setupQrPayload(DEVICE, K);
  EXPECT_EQ(payload, "stockstick://setup?d=6fbf4024-92c6-405d-85a0-a859649e2a5d&k=000102030405060708090a0b0c0d0e0f");
  EXPECT_LE(payload.size(), 106u);  // fits QR version 5-L in byte mode
  EXPECT_EQ(ble_setup::advertisedName(DEVICE), "StockStick-6FBF");
}

TEST(BleSetupProtocol, NetworksNormalized) {
  const auto networks = ble_setup::normalizeNetworks({{"a", -70, true},
                                                      {"", -20, true},
                                                      {"b", -40, false},
                                                      {"a", -50, false},
                                                      {"c", -90, true},
                                                      {"d", -60, true},
                                                      {"e", -65, true},
                                                      {"f", -30, true}});
  ASSERT_EQ(networks.size(), ble_setup::MAX_NETWORKS);
  EXPECT_EQ(networks[0].ssid, "f");
  EXPECT_EQ(networks[1].ssid, "b");
  EXPECT_EQ(networks[2].ssid, "a");
  EXPECT_EQ(networks[2].rssi, -50);
  EXPECT_FALSE(networks[2].locked);
}

TEST(BleSetupProtocol, StatusCappedAt512Bytes) {
  EXPECT_EQ(ble_setup::withNetworks("{\"a\":1}", {{"x\"y", -40, true}}), "{\"a\":1,\"networks\":[{\"s\":\"x\\\"y\",\"r\":-40,\"l\":1}]}");
  EXPECT_EQ(ble_setup::withNetworks("{}", {}), "{\"networks\":[]}");
  const std::string base = "{\"pad\":\"" + std::string(300, 'p') + "\"}";
  std::vector<ble_setup::Network> networks;
  for (int i = 0; i < 5; ++i) networks.push_back({std::string(32, static_cast<char>('A' + i)), -40 - i, true});
  const auto out = ble_setup::withNetworks(base, networks);
  EXPECT_LE(out.size(), ble_setup::STATUS_LIMIT);
  EXPECT_NE(out.find("AAAA"), std::string::npos);  // strongest kept
  EXPECT_EQ(out.find("EEEE"), std::string::npos);  // weakest dropped
  const std::string huge = "{\"pad\":\"" + std::string(600, 'p') + "\"}";
  EXPECT_EQ(ble_setup::withNetworks(huge, networks), huge);
}
