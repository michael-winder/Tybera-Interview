#include "byte_source.hpp"

#include <array>
#include <stdexcept>

using namespace std;
using std::array;
using std::vector;

/**
 * Utility: drains all bytes from a single-read ByteSource up to maxBytes.
 */
vector<uint8_t> consumeToBuffer(ByteSource& src, size_t maxBytes) {
    vector<uint8_t> data;
    const size_t initialReserve = maxBytes < (64 * 1024) ? maxBytes : (64 * 1024);
    if (initialReserve > 0) {
        data.reserve(initialReserve);
    }
    array<uint8_t, 64 * 1024> scratch{};
    size_t total = 0;
    while (true) {
        size_t readCount = src.read(scratch.data(), scratch.size());
        if (readCount == 0)
            break;
        total += readCount;
        if (total > maxBytes)
            throw runtime_error("byte source exceeds buffer ceiling");
        data.insert(data.end(), scratch.begin(), scratch.begin() + readCount);
    }
    return data;
}
