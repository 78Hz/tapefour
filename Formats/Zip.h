#pragma once

/*
    The container a .cass file is: a standard zip archive whose entries are stored, not
    compressed, so any zip tool can open one and a reader can play a track straight from
    its place in the file. Plain C++17.

    Limits (version 1): each entry and the whole archive under 4 GiB (no zip64). A C-90
    cassette is about 690 MB.
*/

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace tfmt
{
/** A path from UTF-8 text, on every platform. */
std::filesystem::path pathFromUtf8 (const std::string& utf8);
std::string utf8FromPath (const std::filesystem::path& p);

/** CRC-32 as zip uses it; call with the previous value to continue over more data. */
uint32_t crc32 (const void* data, size_t size, uint32_t previous = 0) noexcept;

class ZipWriter
{
public:
    explicit ZipWriter (const std::filesystem::path& file);
    ~ZipWriter();

    bool ok() const noexcept { return problem.empty(); }
    const std::string& error() const noexcept { return problem; }

    bool addFile (const std::string& name, const std::filesystem::path& source);
    bool addData (const std::string& name, const void* data, size_t size);
    /** Writes the directory and closes the file; without it the archive is incomplete. */
    bool finish();

private:
    struct Entry { std::string name; uint32_t crc = 0, size = 0, offset = 0; };
    bool begin (const std::string& name, uint32_t& headerOffset);
    bool end (Entry& e, uint32_t headerOffset, uint64_t written, uint32_t crc);
    void fail (const std::string& why) { if (problem.empty()) problem = why; }

    std::ofstream out;
    std::vector<Entry> entries;
    std::string problem;
    uint16_t dosTime = 0, dosDate = 0;
    bool finished = false;
};

class ZipReader
{
public:
    struct Entry
    {
        std::string name;
        uint64_t dataOffset = 0;   // where the entry's bytes start in the archive
        uint32_t size = 0, crc = 0;
    };

    explicit ZipReader (const std::filesystem::path& file);

    bool ok() const noexcept { return problem.empty(); }
    const std::string& error() const noexcept { return problem; }

    const std::vector<Entry>& getEntries() const noexcept { return entries; }
    const Entry* find (const std::string& name) const noexcept;

    /** The whole entry into memory (up to maxSize bytes), its checksum verified. */
    bool read (const std::string& name, std::string& out, uint32_t maxSize = 16u << 20);
    /** The entry into a file, its checksum verified. */
    bool extract (const std::string& name, const std::filesystem::path& target);

    /** A name that is safe to unpack: relative, no "..", no backslashes or drive letters. */
    static bool isSafeName (const std::string& name) noexcept;

private:
    void fail (const std::string& why) { if (problem.empty()) problem = why; }

    std::filesystem::path path;
    std::vector<Entry> entries;
    std::string problem;
};
} // namespace tfmt
