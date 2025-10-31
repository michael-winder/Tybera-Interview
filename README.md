Coding assignment for Tybera - Michael Winder

# Document Ingest Component (C++)

This project implements the ingest layer for a protocol-agnostic document pipeline. It consumes a non-rewindable byte stream, computes file characteristics, validates them against request metadata and policy, and forwards the same bytes to a downstream sink.

## Project Layout

- `src/byte_source.hpp` / `src/byte_source.cpp`: Defines the `ByteSource` interface plus a helper that drains a source into memory while enforcing a size ceiling.
- `src/ingest.hpp` / `src/ingest.cpp`: Public ingest API along with SHA-256 hashing, MIME sniffing for PDF/DOCX/PNG, validation helpers, and sink invocation.
- `test/test.cpp`: Self-contained unit tests with an in-memory `ByteSource`, fixtures for the sample files, and a `RecordingSink` to assert forwarding behavior.
- `test/resources/`: Sample documents used by the tests.

## Build

The code targets C++17 and uses only the standard library. To build the test runner:

```bash
clang++ -std=c++17 -O2 -Isrc src/byte_source.cpp src/ingest.cpp test/test.cpp -o test/ingest_tests
