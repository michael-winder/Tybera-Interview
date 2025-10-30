#pragma once

#include "byte_source.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

/**
 * Metadata about an upload request.
 */
struct UploadMeta {
    std::string filename;
    std::string claimedMime;
    // contentLength presence modeled without std::optional for wider compiler support
    bool hasContentLength;
    std::int64_t contentLength;
};

/**
 * Validation and policy configuration for document ingest.
 */
struct IngestConfig {
    std::int64_t maxContentLength;
    std::vector<std::string> acceptedMimes;
};

/**
 * The result model for ingest validation and reporting.
 */
struct IngestResult {
    std::string detectedMime;
    std::int64_t size;
    std::string sha256;
    bool ok;
    std::vector<std::string> errors;
};

/**
 * Sink API for forwarding uploads downstream (mocked in tests).
 */
class IngestSink {
public:
    virtual ~IngestSink() = default;
    /**
     * Persists the validated upload downstream. Implementation consumes the byte stream.
     */
    virtual void persist(const UploadMeta& meta,
                        const IngestResult& result,
                        ByteSource& data) = 0;
};

/**
 * Consumes the source, computes validation, and forwards bytes to the sink.
 */
void ingest(const UploadMeta& meta,
            const IngestConfig& cfg,
            ByteSource& source,
            IngestSink& sink);

