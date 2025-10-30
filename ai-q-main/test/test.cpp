#include "../src/ingest.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <cassert>

using namespace std;


namespace {

/**
 * A simple in-memory ByteSource for testing.
 */
class MemoryByteSource final : public ByteSource {
public:
    explicit MemoryByteSource(vector<uint8_t> data) : data_(std::move(data)), offset_(0) {}

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

    size_t size() const { return data_.size(); }

private:
    vector<uint8_t> data_;
    size_t offset_;
};

/**
 * Loads a binary file from a few candidate paths.
 */
vector<uint8_t> loadFile(const string& path) {
    const vector<string> candidates = {
        path,
        string("../") + path,
        string("ai-q-main/") + path,
        string("../ai-q-main/") + path};

    ifstream in;
    string used;
    for (const auto& p : candidates) {
        in.open(p.c_str(), ios::binary);
        if (in) { used = p; break; }
        in.clear();
    }
    if (!in) {
        throw runtime_error("failed to open " + path);
    }
    vector<uint8_t> data;
    in.seekg(0, ios::end);
    streampos sz = in.tellg();
    if (sz < 0) throw runtime_error("failed to stat file size: " + used);
    data.resize(static_cast<size_t>(sz));
    in.seekg(0, ios::beg);
    in.read(reinterpret_cast<char*>(data.data()), data.size());
    if (!in) throw runtime_error("failed to read " + used);
    return data;
}

/**
 * Test-mock implementation of IngestSink which records results and bytes.
 */
class RecordingSink final : public IngestSink {
public:
    void persist(const UploadMeta& meta, const IngestResult& result, ByteSource& data) override {
        lastMeta = meta;
        lastResult = result;
        forwarded.clear();
        forwardedBytes = 0;
        uint8_t buffer[4096];
        while (true) {
            size_t n = data.read(buffer, sizeof(buffer));
            if (n == 0) break;
            forwarded.insert(forwarded.end(), buffer, buffer + n);
            forwardedBytes += n;
        }
    }
    UploadMeta lastMeta;
    IngestResult lastResult;
    vector<uint8_t> forwarded;
    size_t forwardedBytes{0};
};

// --- Utility asserts for error matching, stream size tracking ---

bool containsError(const IngestResult& result, const string& message) {
    return find(result.errors.begin(), result.errors.end(), message) != result.errors.end();
}

bool forwardedMatches(const RecordingSink& sink, size_t expectedBytes) {
    return sink.forwardedBytes == expectedBytes &&
           sink.forwarded.size() == expectedBytes &&
           static_cast<size_t>(sink.lastResult.size) == expectedBytes;
}

// ======================== Happy Paths ========================

void testPdfHappy() {
    auto data = loadFile("test/resources/sample.pdf");
    MemoryByteSource src(data);
    UploadMeta meta{"sample.pdf", "application/pdf", true, static_cast<int64_t>(data.size())};
    IngestConfig cfg{static_cast<int64_t>(data.size() + 1024),
                     {"application/pdf", "application/vnd.openxmlformats-officedocument.wordprocessingml.document", "image/png"}};
    RecordingSink sink;
    ingest(meta, cfg, src, sink);
    assert(sink.lastResult.ok);
    assert(sink.lastResult.errors.empty());
    assert(sink.lastResult.detectedMime == "application/pdf");
    assert(forwardedMatches(sink, data.size()));
    assert(sink.lastResult.size <= cfg.maxContentLength);
}

// ======================== Negative Paths ========================

void testClaimedMimeMismatch() {
    auto data = loadFile("test/resources/sample.pdf");
    MemoryByteSource src(data);
    UploadMeta meta{"sample.pdf", "image/png", true, static_cast<int64_t>(data.size())};
    IngestConfig cfg{static_cast<int64_t>(data.size() + 1024),
                     {"application/pdf", "application/vnd.openxmlformats-officedocument.wordprocessingml.document", "image/png"}};
    RecordingSink sink;
    ingest(meta, cfg, src, sink);
    assert(!sink.lastResult.ok);
    assert(containsError(sink.lastResult, "claimedMime does not match detectedMime"));
    assert(!containsError(sink.lastResult, "detectedMime not accepted"));
    assert(forwardedMatches(sink, data.size()));
}

void testContentLengthMismatchPlusOne() {
    auto data = loadFile("test/resources/sample.docx");
    MemoryByteSource src(data);
    UploadMeta meta{"sample.docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document", true, static_cast<int64_t>(data.size()) + 1};
    IngestConfig cfg{static_cast<int64_t>(data.size() + 1024),
                     {"application/pdf", "application/vnd.openxmlformats-officedocument.wordprocessingml.document", "image/png"}};
    RecordingSink sink;
    ingest(meta, cfg, src, sink);
    assert(!sink.lastResult.ok);
    assert(containsError(sink.lastResult, "contentLength mismatch"));
    assert(forwardedMatches(sink, data.size()));
}

void testMimeNotAccepted() {
    auto data = loadFile("test/resources/sample.png");
    MemoryByteSource src(data);
    UploadMeta meta{"sample.png", "image/png", true, static_cast<int64_t>(data.size())};
    IngestConfig cfg{static_cast<int64_t>(data.size() + 1024),
                     {"application/pdf", "application/vnd.openxmlformats-officedocument.wordprocessingml.document"}};
    RecordingSink sink;
    ingest(meta, cfg, src, sink);
    assert(!sink.lastResult.ok);
    assert(containsError(sink.lastResult, "detectedMime not accepted"));
    assert(forwardedMatches(sink, data.size()));
}

void testContentLengthMismatchMinusOne() {
    auto data = loadFile("test/resources/sample.docx");
    MemoryByteSource src(data);
    int64_t reported = static_cast<int64_t>(data.size()) - 1;
    if (reported < 0) reported = 0;
    UploadMeta meta{"sample.docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document", true, reported};
    IngestConfig cfg{static_cast<int64_t>(data.size() + 1024),
                     {"application/pdf", "application/vnd.openxmlformats-officedocument.wordprocessingml.document", "image/png"}};
    RecordingSink sink;
    ingest(meta, cfg, src, sink);
    assert(!sink.lastResult.ok);
    assert(containsError(sink.lastResult, "contentLength mismatch"));
    assert(forwardedMatches(sink, data.size()));
}

void testExceedsMaxContentLength() {
    auto data = loadFile("test/resources/sample.pdf");
    MemoryByteSource src(data);
    UploadMeta meta{"sample.pdf", "application/pdf", true, static_cast<int64_t>(data.size())};
    IngestConfig cfg{static_cast<int64_t>(data.size() - 1),
                     {"application/pdf", "application/vnd.openxmlformats-officedocument.wordprocessingml.document", "image/png"}};
    RecordingSink sink;
    ingest(meta, cfg, src, sink);
    assert(!sink.lastResult.ok);
    assert(containsError(sink.lastResult, "exceeds maxContentLength"));
    assert(forwardedMatches(sink, data.size()));
}

// ======================== Edge Cases ========================

void testNoContentLengthMaxEnforced() {
    auto data = loadFile("test/resources/sample.pdf");
    MemoryByteSource src(data);
    UploadMeta meta{"sample.pdf", "application/pdf", false, 0};
    IngestConfig cfg{static_cast<int64_t>(data.size() - 1),
                     {"application/pdf", "application/vnd.openxmlformats-officedocument.wordprocessingml.document", "image/png"}};
    RecordingSink sink;
    ingest(meta, cfg, src, sink);
    assert(!sink.lastResult.ok);
    assert(containsError(sink.lastResult, "exceeds maxContentLength"));
    assert(forwardedMatches(sink, data.size()));
}

void testTinyInput() {
    vector<uint8_t> empty;
    size_t expectedSize = empty.size();
    MemoryByteSource src(empty);
    UploadMeta meta{"empty.bin", "application/octet-stream", false, 0};
    IngestConfig cfg{1024, {"application/octet-stream", "application/pdf"}};
    RecordingSink sink;
    ingest(meta, cfg, src, sink);
    assert(sink.lastResult.ok);
    assert(sink.lastResult.errors.empty());
    assert(sink.lastResult.detectedMime == "application/octet-stream");
    assert(forwardedMatches(sink, expectedSize));
    assert(sink.lastResult.size == 0);
}

} // end namespace

int main() {
    testPdfHappy();
    testClaimedMimeMismatch();
    testContentLengthMismatchPlusOne();
    testContentLengthMismatchMinusOne();
    testExceedsMaxContentLength();
    testNoContentLengthMaxEnforced();
    testMimeNotAccepted();
    testTinyInput();
    cout << "All ingest tests passed\n";
    return 0;
}
