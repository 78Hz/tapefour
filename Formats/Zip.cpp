#include "Zip.h"

#include <array>
#include <ctime>

namespace tfmt
{
std::filesystem::path pathFromUtf8 (const std::string& utf8)
{
   #if defined (__cpp_char8_t)
    return std::filesystem::path (std::u8string (utf8.begin(), utf8.end()));
   #else
    return std::filesystem::u8path (utf8);
   #endif
}

std::string utf8FromPath (const std::filesystem::path& p)
{
    auto u = p.u8string();
    return std::string (u.begin(), u.end());
}

uint32_t crc32 (const void* data, size_t size, uint32_t previous) noexcept
{
    static const auto table = []
    {
        std::array<uint32_t, 256> t {};
        for (uint32_t i = 0; i < 256; ++i)
        {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    uint32_t c = previous ^ 0xffffffffu;
    auto* p = static_cast<const uint8_t*> (data);
    for (size_t i = 0; i < size; ++i) c = table[(c ^ p[i]) & 0xff] ^ (c >> 8);
    return c ^ 0xffffffffu;
}

namespace
{
    constexpr uint32_t localSig = 0x04034b50, centralSig = 0x02014b50, endSig = 0x06054b50;
    constexpr uint16_t utf8Flag = 0x0800;
    constexpr uint64_t limit = 0xffffffffull;

    void put16 (std::string& s, uint16_t v) { s += (char) (v & 0xff); s += (char) (v >> 8); }
    void put32 (std::string& s, uint32_t v) { for (int i = 0; i < 4; ++i) s += (char) ((v >> (8 * i)) & 0xff); }
    uint16_t get16 (const uint8_t* p) { return (uint16_t) (p[0] | (p[1] << 8)); }
    uint32_t get32 (const uint8_t* p) { return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24); }
}

//==============================================================================
ZipWriter::ZipWriter (const std::filesystem::path& file)
{
    out.open (file, std::ios::binary | std::ios::trunc);
    if (! out) fail ("could not create " + utf8FromPath (file));

    auto now = std::time (nullptr);
    std::tm t {};
   #if defined (_WIN32)
    localtime_s (&t, &now);
   #else
    localtime_r (&now, &t);
   #endif
    dosTime = (uint16_t) ((t.tm_hour << 11) | (t.tm_min << 5) | (t.tm_sec / 2));
    dosDate = (uint16_t) (((std::max (1980, t.tm_year + 1900) - 1980) << 9) | ((t.tm_mon + 1) << 5) | t.tm_mday);
}

ZipWriter::~ZipWriter()
{
    if (! finished) finish();
}

bool ZipWriter::begin (const std::string& name, uint32_t& headerOffset)
{
    if (! ok()) return false;
    if (! ZipReader::isSafeName (name) || name.size() > 0xffff) { fail ("bad entry name " + name); return false; }
    auto at = (uint64_t) out.tellp();
    if (at > limit) { fail ("archive over 4 GiB"); return false; }
    headerOffset = (uint32_t) at;

    std::string h;
    put32 (h, localSig);
    put16 (h, 20);          // version needed
    put16 (h, utf8Flag);
    put16 (h, 0);           // stored
    put16 (h, dosTime);
    put16 (h, dosDate);
    put32 (h, 0);           // crc, sizes: filled in by end()
    put32 (h, 0);
    put32 (h, 0);
    put16 (h, (uint16_t) name.size());
    put16 (h, 0);
    h += name;
    out.write (h.data(), (std::streamsize) h.size());
    if (! out) { fail ("write failed"); return false; }
    return true;
}

bool ZipWriter::end (Entry& e, uint32_t headerOffset, uint64_t written, uint32_t crc)
{
    if (written > limit) { fail ("entry over 4 GiB: " + e.name); return false; }
    e.crc = crc;
    e.size = (uint32_t) written;
    e.offset = headerOffset;

    // Go back and fill in the checksum and sizes.
    auto here = out.tellp();
    std::string patch;
    put32 (patch, e.crc);
    put32 (patch, e.size);
    put32 (patch, e.size);
    out.seekp ((std::streamoff) headerOffset + 14);
    out.write (patch.data(), (std::streamsize) patch.size());
    out.seekp (here);
    if (! out) { fail ("write failed"); return false; }
    entries.push_back (e);
    return true;
}

bool ZipWriter::addData (const std::string& name, const void* data, size_t size)
{
    uint32_t headerOffset = 0;
    if (! begin (name, headerOffset)) return false;
    out.write (static_cast<const char*> (data), (std::streamsize) size);
    if (! out) { fail ("write failed"); return false; }
    Entry e;
    e.name = name;
    return end (e, headerOffset, size, crc32 (data, size));
}

bool ZipWriter::addFile (const std::string& name, const std::filesystem::path& source)
{
    std::ifstream in (source, std::ios::binary);
    if (! in) { fail ("could not open " + utf8FromPath (source)); return false; }
    uint32_t headerOffset = 0;
    if (! begin (name, headerOffset)) return false;

    std::vector<char> buffer (1 << 20);
    uint64_t total = 0;
    uint32_t crc = 0;
    while (in)
    {
        in.read (buffer.data(), (std::streamsize) buffer.size());
        auto got = (size_t) in.gcount();
        if (got == 0) break;
        crc = crc32 (buffer.data(), got, crc);
        out.write (buffer.data(), (std::streamsize) got);
        if (! out) { fail ("write failed"); return false; }
        total += got;
    }
    Entry e;
    e.name = name;
    return end (e, headerOffset, total, crc);
}

bool ZipWriter::finish()
{
    if (finished) return ok();
    finished = true;
    if (! ok()) { out.close(); return false; }

    auto dirStart = (uint64_t) out.tellp();
    std::string d;
    for (auto& e : entries)
    {
        put32 (d, centralSig);
        put16 (d, 20);          // made by
        put16 (d, 20);          // needed
        put16 (d, utf8Flag);
        put16 (d, 0);
        put16 (d, dosTime);
        put16 (d, dosDate);
        put32 (d, e.crc);
        put32 (d, e.size);
        put32 (d, e.size);
        put16 (d, (uint16_t) e.name.size());
        put16 (d, 0);           // extra
        put16 (d, 0);           // comment
        put16 (d, 0);           // disk
        put16 (d, 0);           // internal attributes
        put32 (d, 0);           // external attributes
        put32 (d, e.offset);
        d += e.name;
    }
    if (dirStart + d.size() > limit || entries.size() > 0xffff) { fail ("archive over the zip limits"); out.close(); return false; }
    const auto dirSize = (uint32_t) d.size();
    put32 (d, endSig);
    put16 (d, 0);           // this disk
    put16 (d, 0);           // directory's disk
    put16 (d, (uint16_t) entries.size());
    put16 (d, (uint16_t) entries.size());
    put32 (d, dirSize);
    put32 (d, (uint32_t) dirStart);
    put16 (d, 0);           // comment
    out.write (d.data(), (std::streamsize) d.size());
    out.close();
    if (! out) { fail ("write failed"); return false; }
    return true;
}

//==============================================================================
bool ZipReader::isSafeName (const std::string& name) noexcept
{
    if (name.empty() || name.front() == '/' || name.find ('\\') != std::string::npos || name.find (':') != std::string::npos)
        return false;
    size_t start = 0;
    for (;;)
    {
        auto slash = name.find ('/', start);
        auto part = name.substr (start, slash == std::string::npos ? std::string::npos : slash - start);
        if (part.empty() || part == "." || part == "..") return false;
        if (slash == std::string::npos) return true;
        start = slash + 1;
    }
}

ZipReader::ZipReader (const std::filesystem::path& file) : path (file)
{
    std::ifstream in (file, std::ios::binary);
    if (! in) { fail ("could not open " + utf8FromPath (file)); return; }
    in.seekg (0, std::ios::end);
    auto total = (uint64_t) in.tellg();
    if (total < 22) { fail ("not a zip archive"); return; }

    // The end record is in the last 22 bytes, plus a comment of up to 64 KiB.
    auto tail = (size_t) std::min<uint64_t> (total, 22 + 65535);
    std::vector<uint8_t> buf (tail);
    in.seekg ((std::streamoff) (total - tail));
    in.read (reinterpret_cast<char*> (buf.data()), (std::streamsize) tail);
    long at = -1;
    for (long i = (long) tail - 22; i >= 0; --i)
        if (get32 (&buf[(size_t) i]) == endSig) { at = i; break; }
    if (at < 0) { fail ("not a zip archive"); return; }

    const auto* e = &buf[(size_t) at];
    auto count = get16 (e + 10);
    auto dirSize = get32 (e + 12), dirOffset = get32 (e + 16);
    if ((uint64_t) dirOffset + dirSize > total) { fail ("damaged archive directory"); return; }

    std::vector<uint8_t> dir (dirSize);
    in.seekg ((std::streamoff) dirOffset);
    in.read (reinterpret_cast<char*> (dir.data()), (std::streamsize) dirSize);
    if (! in) { fail ("damaged archive directory"); return; }

    size_t p = 0;
    for (int i = 0; i < count; ++i)
    {
        if (p + 46 > dir.size() || get32 (&dir[p]) != centralSig) { fail ("damaged archive directory"); return; }
        auto flags = get16 (&dir[p + 8]), method = get16 (&dir[p + 10]);
        auto crc = get32 (&dir[p + 16]), csize = get32 (&dir[p + 20]), usize = get32 (&dir[p + 24]);
        auto nameLen = get16 (&dir[p + 28]), extraLen = get16 (&dir[p + 30]), commentLen = get16 (&dir[p + 32]);
        auto localOffset = get32 (&dir[p + 42]);
        if (p + 46 + nameLen > dir.size()) { fail ("damaged archive directory"); return; }
        std::string name (reinterpret_cast<const char*> (&dir[p + 46]), nameLen);
        p += 46u + nameLen + extraLen + commentLen;

        if (name.back() == '/') continue;   // a folder
        if ((flags & 1) != 0) { fail ("encrypted entry: " + name); return; }
        if (method != 0 || csize != usize) { fail ("compressed entry (cassettes are stored): " + name); return; }
        if (! isSafeName (name)) { fail ("unsafe entry name: " + name); return; }

        // The data starts after the local header, whose name and extra field may differ in length.
        uint8_t local[30];
        in.seekg ((std::streamoff) localOffset);
        in.read (reinterpret_cast<char*> (local), 30);
        if (! in || get32 (local) != localSig) { fail ("damaged entry: " + name); return; }
        Entry entry;
        entry.name = name;
        entry.dataOffset = (uint64_t) localOffset + 30 + get16 (local + 26) + get16 (local + 28);
        entry.size = usize;
        entry.crc = crc;
        if (entry.dataOffset + entry.size > total) { fail ("damaged entry: " + name); return; }
        entries.push_back (entry);
    }
}

const ZipReader::Entry* ZipReader::find (const std::string& name) const noexcept
{
    for (auto& e : entries)
        if (e.name == name) return &e;
    return nullptr;
}

bool ZipReader::read (const std::string& name, std::string& out, uint32_t maxSize)
{
    auto* e = find (name);
    if (e == nullptr) { fail ("missing " + name); return false; }
    if (e->size > maxSize) { fail ("too large: " + name); return false; }
    std::ifstream in (path, std::ios::binary);
    out.resize (e->size);
    in.seekg ((std::streamoff) e->dataOffset);
    in.read (out.data(), (std::streamsize) e->size);
    if (! in) { fail ("could not read " + name); return false; }
    if (crc32 (out.data(), out.size()) != e->crc) { fail ("checksum mismatch: " + name); return false; }
    return true;
}

bool ZipReader::extract (const std::string& name, const std::filesystem::path& target)
{
    auto* e = find (name);
    if (e == nullptr) { fail ("missing " + name); return false; }
    std::ifstream in (path, std::ios::binary);
    std::ofstream out (target, std::ios::binary | std::ios::trunc);
    if (! in || ! out) { fail ("could not extract " + name); return false; }
    in.seekg ((std::streamoff) e->dataOffset);
    std::vector<char> buffer (1 << 20);
    uint64_t left = e->size;
    uint32_t crc = 0;
    while (left > 0)
    {
        auto chunk = (size_t) std::min<uint64_t> (left, buffer.size());
        in.read (buffer.data(), (std::streamsize) chunk);
        if (! in) { fail ("could not read " + name); return false; }
        crc = crc32 (buffer.data(), chunk, crc);
        out.write (buffer.data(), (std::streamsize) chunk);
        left -= chunk;
    }
    out.close();
    if (! out) { fail ("could not write " + utf8FromPath (target)); return false; }
    if (crc != e->crc) { fail ("checksum mismatch: " + name); return false; }
    return true;
}
} // namespace tfmt
