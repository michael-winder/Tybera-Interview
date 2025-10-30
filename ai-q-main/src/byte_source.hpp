#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

using namespace std;
using std::vector;

/**
 * ByteSource models a finite, non-rewindable byte stream.
 */
class ByteSource {
public:
    virtual ~ByteSource() = default;

    /**
     * Reads up to maxLen bytes into buffer.
     * Returns 0 on EOF. Throws on I/O errors.
     */
    virtual size_t read(uint8_t* buffer, size_t maxLen) = 0;
};

/**
 * Utility: Drains the provided ByteSource into a new buffer, up to maxBytes.
 * Throws if the data exceeds maxBytes.
 */
vector<uint8_t> consumeToBuffer(ByteSource& src, size_t maxBytes);

