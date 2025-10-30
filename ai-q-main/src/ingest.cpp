#include "ingest.hpp"

#include "byte_source.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace std;

namespace {

// --- Helpers for replayable byte streams in tests ---

class VectorByteSource final : public ByteSource {
public:
    explicit VectorByteSource(const vector<uint8_t>& data) : data_(data), offset_(0) {}

    size_t read(uint8_t* buffer, size_t maxLen) override {
        if (offset_ >= data_.size() || maxLen == 0) {
            return 0;
        }
        size_t remaining = data_.size() - offset_;
        size_t toCopy = remaining < maxLen ? remaining : maxLen;
        memcpy(buffer, data_.data() + offset_, toCopy);
        offset_ += toCopy;
        return toCopy;
    }

private:
    const vector<uint8_t>& data_;
    size_t offset_;
};

// ---------- SHA-256 utilities ----------
constexpr array<uint32_t, 64> kSha256K = {
    0x428a2f98UL, 0x71374491UL, 0xb5c0fbcfUL, 0xe9b5dba5UL, 0x3956c25bUL, 0x59f111f1UL, 0x923f82a4UL, 0xab1c5ed5UL,
    0xd807aa98UL, 0x12835b01UL, 0x243185beUL, 0x550c7dc3UL, 0x72be5d74UL, 0x80deb1feUL, 0x9bdc06a7UL, 0xc19bf174UL,
    0xe49b69c1UL, 0xefbe4786UL, 0x0fc19dc6UL, 0x240ca1ccUL, 0x2de92c6fUL, 0x4a7484aaUL, 0x5cb0a9dcUL, 0x76f988daUL,
    0x983e5152UL, 0xa831c66dUL, 0xb00327c8UL, 0xbf597fc7UL, 0xc6e00bf3UL, 0xd5a79147UL, 0x06ca6351UL, 0x14292967UL,
    0x27b70a85UL, 0x2e1b2138UL, 0x4d2c6dfcUL, 0x53380d13UL, 0x650a7354UL, 0x766a0abbUL, 0x81c2c92eUL, 0x92722c85UL,
    0xa2bfe8a1UL, 0xa81a664bUL, 0xc24b8b70UL, 0xc76c51a3UL, 0xd192e819UL, 0xd6990624UL, 0xf40e3585UL, 0x106aa070UL,
    0x19a4c116UL, 0x1e376c08UL, 0x2748774cUL, 0x34b0bcb5UL, 0x391c0cb3UL, 0x4ed8aa4aUL, 0x5b9cca4fUL, 0x682e6ff3UL,
    0x748f82eeUL, 0x78a5636fUL, 0x84c87814UL, 0x8cc70208UL, 0x90befffaUL, 0xa4506cebUL, 0xbef9a3f7UL, 0xc67178f2UL};

inline uint32_t rotr(uint32_t value, uint32_t bits) {
    return (value >> bits) | (value << (32 - bits));
}

array<uint32_t, 8> sha256StateInit() {
    return {0x6a09e667UL, 0xbb67ae85UL, 0x3c6ef372UL, 0xa54ff53aUL,
            0x510e527fUL, 0x9b05688cUL, 0x1f83d9abUL, 0x5be0cd19UL};
}

/**
 * Internal: Updates SHA-256 state with a message block.
 */
void sha256ProcessBlock(array<uint32_t, 8>& state, const uint8_t* block) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = (static_cast<uint32_t>(block[i * 4]) << 24) |
               (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
               (static_cast<uint32_t>(block[i * 4 + 2]) << 8) |
               (static_cast<uint32_t>(block[i * 4 + 3]));
    }
    for (int i = 16; i < 64; ++i) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = state[0];
    uint32_t b = state[1];
    uint32_t c = state[2];
    uint32_t d = state[3];
    uint32_t e = state[4];
    uint32_t f = state[5];
    uint32_t g = state[6];
    uint32_t h = state[7];

    for (int i = 0; i < 64; ++i) {
        uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t temp1 = h + S1 + ch + kSha256K[i] + w[i];
        uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = S0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

/**
 * Computes the SHA-256 hex digest of the given buffer.
 */
string sha256Hex(const vector<uint8_t>& data) {
    auto state = sha256StateInit();
    uint64_t bitLength = static_cast<uint64_t>(data.size()) * 8;

    size_t processed = 0;
    size_t blockSize = 64;

    while (processed + blockSize <= data.size()) {
        sha256ProcessBlock(state, data.data() + processed);
        processed += blockSize;
    }

    uint8_t finalBlock[128] = {0};
    size_t remaining = data.size() - processed;
    memcpy(finalBlock, data.data() + processed, remaining);
    finalBlock[remaining] = 0x80;

    size_t paddingIndex = ((remaining + 9) <= blockSize) ? blockSize - 8 : (2 * blockSize) - 8;
    for (size_t i = 0; i < 8; ++i) {
        finalBlock[paddingIndex + i] = static_cast<uint8_t>((bitLength >> (56 - i * 8)) & 0xFF);
    }

    sha256ProcessBlock(state, finalBlock);
    if (paddingIndex != blockSize - 8) {
        sha256ProcessBlock(state, finalBlock + blockSize);
    }

    ostringstream oss;
    oss << hex << setfill('0');
    for (uint32_t word : state) {
        oss << setw(8) << word;
    }
    return oss.str();
}

// ------------ MIME detection ----------
/**
 * Detects the MIME type of a file's bytes by sniffing content.
 */
string detectMime(const vector<uint8_t>& bytes) {
    if (bytes.size() >= 4) {
        if (bytes[0] == '%' && bytes[1] == 'P' && bytes[2] == 'D' && bytes[3] == 'F') {
            return "application/pdf";
        }
    }
    if (bytes.size() >= 8) {
        array<uint8_t, 8> pngMagic = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
        if (equal(pngMagic.begin(), pngMagic.end(), bytes.begin())) {
            return "image/png";
        }
    }
    if (bytes.size() >= 4) {
        if (bytes[0] == 'P' && bytes[1] == 'K' && bytes[2] == 0x03 && bytes[3] == 0x04) {
            string signature(reinterpret_cast<const char*>(bytes.data()),
                                        min<size_t>(bytes.size(), static_cast<size_t>(4096)));
            if (signature.find("word/") != string::npos ||
                signature.find("[Content_Types].xml") != string::npos) {
                return "application/vnd.openxmlformats-officedocument.wordprocessingml.document";
            }
        }
    }
    return "application/octet-stream";
}

/**
 * Removes parameters and trims the MIME-type string for easier comparison.
 */
string stripMime(string_view mime) {
    size_t semi = mime.find(';');
    string base(mime.substr(0, semi));
    auto notSpace = [](unsigned char ch) { return !isspace(ch); };
    base.erase(base.begin(), find_if(base.begin(), base.end(), notSpace));
    base.erase(find_if(base.rbegin(), base.rend(), notSpace).base(), base.end());
    transform(base.begin(), base.end(), base.begin(), [](unsigned char ch) { return tolower(ch); });
    return base;
}

/**
 * MIME comparison, ignoring parameters.
 */
bool equalsIgnoreParams(string_view lhs, string_view rhs) {
    return stripMime(lhs) == stripMime(rhs);
}

/**
 * Returns true if the detected type is in the acceptedMimes set.
 */
bool isAcceptedMime(const string& detected, const vector<string>& accepted) {
    for (const auto& candidate : accepted) {
        if (equalsIgnoreParams(detected, candidate)) {
            return true;
        }
    }
    return false;
}

/**
 * Gathers validation errors for contentLength and maxContentLength.
 */
void validateLengths(const UploadMeta& meta, int64_t size, int64_t maxContentLength, vector<string>& errors) {
    if (meta.hasContentLength) {
        if (meta.contentLength < 0) {
            errors.emplace_back("contentLength is negative");
        } else if (size != meta.contentLength) {
            errors.emplace_back("contentLength mismatch");
        }
    }
    if (maxContentLength >= 0 && size > maxContentLength) {
        errors.emplace_back("exceeds maxContentLength");
    }
}

/**
 * Gathers validation errors about allowed MIME and claimed-vs-detected.
 */
void validateMime(const UploadMeta& meta, const string& detected,
                  const vector<string>& accepted, vector<string>& errors) {
    if (!meta.claimedMime.empty() && !equalsIgnoreParams(meta.claimedMime, detected)) {
        errors.emplace_back("claimedMime does not match detectedMime");
    }
    if (!accepted.empty() && !isAcceptedMime(detected, accepted)) {
        errors.emplace_back("detectedMime not accepted");
    }
}

} // namespace (internal)

// --------------- INGEST API ---------------
/**
 * Ingests an upload: consumes the source, computes validation and result info, and forwards the same bytes to the sink.
 * All error info is aggregated in result.errors—sink is always called.
 */
void ingest(const UploadMeta& meta, const IngestConfig& cfg, ByteSource& source, IngestSink& sink) {
    vector<uint8_t> buffer = consumeToBuffer(source, numeric_limits<size_t>::max());

    if (buffer.size() > static_cast<size_t>(numeric_limits<int64_t>::max())) {
        throw runtime_error("payload size exceeds supported range");
    }
    int64_t size = static_cast<int64_t>(buffer.size());

    IngestResult result;
    result.detectedMime = detectMime(buffer);
    result.size = size;
    result.sha256 = sha256Hex(buffer);

    validateLengths(meta, size, cfg.maxContentLength, result.errors);
    validateMime(meta, result.detectedMime, cfg.acceptedMimes, result.errors);
    result.ok = result.errors.empty();

    VectorByteSource replay(buffer);
    sink.persist(meta, result, replay);
}
