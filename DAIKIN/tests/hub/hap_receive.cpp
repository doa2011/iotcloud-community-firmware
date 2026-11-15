#include "hub_home_http.h"
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <openssl/evp.h>
#include <string>
#include <vector>

constexpr size_t iotcloud::HomeRecord::Limit;
constexpr size_t iotcloud::HomeRecord::Overhead;

static uint32_t clockNow = 123;
static bool storageHealthy = true;
static unsigned badRequests = 0;
static uint32_t millis() { return clockNow; }
struct HostSpan {
  bool iotcloudStorageHealthy() const { return storageHealthy; }
} homeSpan;

struct HostClient {
  std::vector<uint8_t> input;
  size_t offset = 0;
  size_t maxRead = size_t(-1);
  bool stopped = false;
  int available() const { return int(input.size() - offset); }
  int read(uint8_t *out, size_t wanted) {
    const auto n = std::min(std::min(wanted, maxRead), input.size() - offset);
    if (!n) return 0;
    memcpy(out, input.data() + offset, n);
    offset += n;
    return int(n);
  }
  void add(const std::vector<uint8_t> &bytes) {
    input.insert(input.end(), bytes.begin(), bytes.end());
  }
  void add(const std::string &bytes) {
    input.insert(input.end(), bytes.begin(), bytes.end());
  }
  void stop() { stopped = true; }
};

struct Nonce {
  uint8_t value[12] = {};
  uint8_t *get() { return value; }
  bool inc() { return iotcloud::homeNonceIncrement(value); }
};

struct HAPClient {
  iotcloud::HomeRequest request;
  iotcloud::HomeRecord encrypted;
  iotcloud::HomeInputWindow inputWindow;
  HostClient client;
  void *cPair = nullptr;
  Nonce c2aNonce;
  uint8_t c2aKey[32] = {};
  int badRequestError() { ++badRequests; return 400; }
  bool receiveRequest();
};

static int crypto_aead_chacha20poly1305_ietf_decrypt(
    uint8_t *plain, unsigned long long *, uint8_t *, const uint8_t *cipher,
    unsigned long long cipherSize, const uint8_t *aad,
    unsigned long long aadSize, const uint8_t *nonce, const uint8_t *key) {
  if (cipherSize < 16) return -1;
  const auto dataSize = cipherSize - 16;
  auto *ctx = EVP_CIPHER_CTX_new();
  assert(ctx);
  int n = 0, tail = 0;
  const bool ok = EVP_DecryptInit_ex(ctx, EVP_chacha20_poly1305(), nullptr,
                                      key, nonce) == 1 &&
      EVP_DecryptUpdate(ctx, nullptr, &n, aad, int(aadSize)) == 1 &&
      EVP_DecryptUpdate(ctx, plain, &n, cipher, int(dataSize)) == 1 &&
      size_t(n) == dataSize &&
      EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_TAG, 16,
                          const_cast<uint8_t *>(cipher + dataSize)) == 1 &&
      EVP_DecryptFinal_ex(ctx, plain + dataSize, &tail) == 1 && tail == 0;
  EVP_CIPHER_CTX_free(ctx);
  return ok ? 0 : -1;
}

#include "receive-request.inc"

static std::vector<uint8_t> seal(const std::string &plain, const uint8_t *nonce,
                                const uint8_t *key) {
  assert(!plain.empty() && plain.size() <= iotcloud::HomeRecord::Limit);
  std::vector<uint8_t> record(plain.size() + iotcloud::HomeRecord::Overhead);
  record[0] = uint8_t(plain.size());
  record[1] = uint8_t(plain.size() >> 8);
  auto *ctx = EVP_CIPHER_CTX_new();
  assert(ctx);
  int n = 0, tail = 0;
  assert(EVP_EncryptInit_ex(ctx, EVP_chacha20_poly1305(), nullptr, key, nonce) == 1);
  assert(EVP_EncryptUpdate(ctx, nullptr, &n, record.data(), 2) == 1);
  assert(EVP_EncryptUpdate(ctx, record.data() + 2, &n,
                           reinterpret_cast<const uint8_t *>(plain.data()),
                           int(plain.size())) == 1 && size_t(n) == plain.size());
  assert(EVP_EncryptFinal_ex(ctx, record.data() + 2 + n, &tail) == 1 && tail == 0);
  assert(EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, 16,
                             record.data() + 2 + plain.size()) == 1);
  EVP_CIPHER_CTX_free(ctx);
  return record;
}

static std::vector<std::vector<uint8_t>> sealRecords(const std::string &plain,
                                                     const uint8_t *key) {
  std::vector<std::vector<uint8_t>> records;
  uint8_t nonce[12] = {};
  for (size_t offset = 0; offset < plain.size();) {
    const auto n = std::min(iotcloud::HomeRecord::Limit, plain.size() - offset);
    records.push_back(seal(plain.substr(offset, n), nonce, key));
    assert(iotcloud::homeNonceIncrement(nonce));
    offset += n;
  }
  return records;
}

static std::string postRequest(const std::string &body) {
  return "POST /pair-setup HTTP/1.1\r\ncontent-type: application/pairing+tlv8\r\n" +
      std::string("content-length: ") + std::to_string(body.size()) + "\r\n\r\n" + body;
}

static void plaintext() {
  const std::string wire = "GET /accessories HTTP/1.1\r\n\r\n";
  HAPClient partial;
  partial.inputWindow.opened(clockNow);
  partial.client.maxRead = 7;
  partial.client.add(wire.substr(0, 11));
  assert(!partial.receiveRequest());
  assert(partial.request.bytes == wire.substr(0, 11));
  assert(!partial.client.stopped && !badRequests);
  partial.client.add(wire.substr(11));
  assert(partial.receiveRequest());
  assert(partial.request.complete() && partial.request.type == iotcloud::HomeRequest::Other);
  assert(partial.request.bytes == wire && !partial.client.stopped);

  HAPClient fragmented;
  fragmented.inputWindow.opened(clockNow);
  fragmented.client.maxRead = 3;
  fragmented.client.add(wire);
  assert(fragmented.receiveRequest());
  assert(fragmented.request.complete() && fragmented.request.bytes == wire);
  assert(!fragmented.client.stopped && !badRequests);
}

static void encrypted() {
  const std::string wire = postRequest(std::string(1100, 'x'));
  HAPClient hap;
  hap.inputWindow.opened(clockNow);
  hap.cPair = &hap;
  hap.client.maxRead = 173;
  for (size_t i = 0; i < sizeof hap.c2aKey; ++i) hap.c2aKey[i] = uint8_t(i * 13 + 5);
  const auto records = sealRecords(wire, hap.c2aKey);
  assert(records.size() == 2);
  hap.client.add(records[0]);
  assert(!hap.receiveRequest());
  assert(hap.request.bytes == wire.substr(0, iotcloud::HomeRecord::Limit));
  assert(hap.encrypted.bytes.empty() && !hap.client.stopped);
  assert(hap.c2aNonce.value[4] == 1);
  hap.client.add(records[1]);
  assert(hap.receiveRequest());
  assert(hap.request.complete() && hap.request.type == iotcloud::HomeRequest::Pairing);
  assert(hap.request.bytes == wire && hap.encrypted.bytes.empty());
  assert(hap.c2aNonce.value[4] == 2 && !hap.client.stopped && !badRequests);
}

static void failures() {
  const std::string wire = "GET /accessories HTTP/1.1\r\n\r\n";
  HAPClient badTag;
  badTag.cPair = &badTag;
  badTag.inputWindow.opened(clockNow);
  const auto good = seal(wire, badTag.c2aNonce.value, badTag.c2aKey);
  auto corrupt = good;
  corrupt.back() ^= 0x80;
  badTag.client.add(corrupt);
  assert(!badTag.receiveRequest());
  assert(badTag.client.stopped && badTag.request.bytes.empty());
  assert(!badTag.c2aNonce.value[4]);

  HAPClient exhausted;
  exhausted.cPair = &exhausted;
  exhausted.inputWindow.opened(clockNow);
  memset(exhausted.c2aNonce.value + 4, 0xff, 8);
  const auto finalNonceRecord = seal(wire, exhausted.c2aNonce.value, exhausted.c2aKey);
  exhausted.client.add(finalNonceRecord);
  assert(!exhausted.receiveRequest());
  assert(exhausted.client.stopped && exhausted.request.bytes.empty());
  for (size_t i = 4; i < 12; ++i) assert(exhausted.c2aNonce.value[i] == 0);
}

int main(int argc, char **argv) {
  assert(argc == 2);
  const std::string scenario = argv[1];
  if (scenario == "plaintext") plaintext();
  else if (scenario == "encrypted") encrypted();
  else if (scenario == "failures") failures();
  else assert(false);
  puts("production HAP receive frame passed");
}
