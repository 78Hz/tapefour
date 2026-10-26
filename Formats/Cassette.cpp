#include "Cassette.h"
#include "Zip.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <ctime>
#include <random>
#include <set>

namespace tfmt
{
namespace fs = std::filesystem;

std::string CassFiles::pcm (int track) { return "track" + std::to_string (track + 1) + ".pcm"; }
std::string CassFiles::map (int track) { return "track" + std::to_string (track + 1) + ".map"; }

const char* layoutName (Layout l) noexcept
{
    switch (l)
    {
        case Layout::fourTrackOneWay: return "4-track-one-way";
        case Layout::stereoTwoSides:  return "stereo-two-sides";
    }
    return "4-track-one-way";
}

std::optional<Layout> layoutFromName (const std::string& s) noexcept
{
    if (s == "4-track-one-way") return Layout::fourTrackOneWay;
    if (s == "stereo-two-sides") return Layout::stereoTwoSides;
    return std::nullopt;
}

bool Formulation::operator== (const Formulation& o) const
{
    return name == o.name && line == o.line && position == o.position && basedOn == o.basedOn && accent == o.accent
        && iecType == o.iecType && eqMicroseconds == o.eqMicroseconds && sameNumber (mol100, o.mol100) && sameNumber (mol400, o.mol400)
        && sameNumber (sol10k, o.sol10k) && sameNumber (snrDbA, o.snrDbA) && sameNumber (modNoiseDb, o.modNoiseDb)
        && sameNumber (sensitivityDb, o.sensitivityDb);
}

bool Condition::operator== (const Condition& o) const
{
    return grade == o.grade && sameNumber (hissDb, o.hissDb) && sameNumber (dropoutsPerMinute, o.dropoutsPerMinute)
        && sameNumber (trebleLossDb, o.trebleLossDb) && sameNumber (modNoiseDb, o.modNoiseDb) && sameNumber (shellFlutterPct, o.shellFlutterPct);
}

bool TrackInfo::operator== (const TrackInfo& o) const
{
    return pcm == o.pcm && map == o.map && encoding == o.encoding && sameNumber (fluxScale, o.fluxScale);
}

const std::vector<std::string>& Condition::grades()
{
    static const std::vector<std::string> g { "new", "used", "worn", "chewed" };
    return g;
}

Condition Condition::forGrade (const std::string& grade)
{
    // Starting points, kept on the gentle side of what real cassettes do (docs/formats/cass-v1.md).
    Condition c;
    c.grade = grade;
    if (grade == "used")        { c.hissDb = 0.5; c.dropoutsPerMinute = 0.15; c.trebleLossDb = 0.5; c.modNoiseDb = 1.0; c.shellFlutterPct = 0.01; }
    else if (grade == "worn")   { c.hissDb = 1.5; c.dropoutsPerMinute = 0.8;  c.trebleLossDb = 1.5; c.modNoiseDb = 3.0; c.shellFlutterPct = 0.03; }
    else if (grade == "chewed") { c.hissDb = 3.0; c.dropoutsPerMinute = 4.0;  c.trebleLossDb = 4.0; c.modNoiseDb = 6.0; c.shellFlutterPct = 0.08; }
    else                        { c.grade = "new"; c.dropoutsPerMinute = 0.02; }
    return c;
}

std::string isoNow()
{
    auto t = std::time (nullptr);
    std::tm u {};
   #if defined (_WIN32)
    gmtime_s (&u, &t);
   #else
    gmtime_r (&t, &u);
   #endif
    char buf[32];
    std::strftime (buf, sizeof (buf), "%Y-%m-%dT%H:%M:%SZ", &u);
    return buf;
}

std::string newUuid()
{
    std::random_device rd;
    std::mt19937_64 gen ((uint64_t) rd() << 32 ^ rd() ^ (uint64_t) std::chrono::high_resolution_clock::now().time_since_epoch().count());
    uint8_t b[16];
    for (int i = 0; i < 16; i += 8)
    {
        auto v = gen();
        for (int k = 0; k < 8; ++k) b[i + k] = (uint8_t) (v >> (8 * k));
    }
    b[6] = (uint8_t) ((b[6] & 0x0f) | 0x40);   // version 4
    b[8] = (uint8_t) ((b[8] & 0x3f) | 0x80);   // variant
    const char* hex = "0123456789abcdef";
    std::string s;
    for (int i = 0; i < 16; ++i)
    {
        if (i == 4 || i == 6 || i == 8 || i == 10) s += '-';
        s += hex[b[i] >> 4];
        s += hex[b[i] & 15];
    }
    return s;
}

CassetteInfo CassetteInfo::make (const Formulation& f, int minutes, Layout layout, const Condition& c, const std::string& madeBy)
{
    CassetteInfo info;
    info.id = newUuid();
    info.created = isoNow();
    info.madeBy = madeBy;
    info.formulation = f;
    info.minutesPerSide = minutes;
    info.layout = layout;
    // A side's minutes of tape at the reference speed: a 4-track tape runs one way over all of it,
    // a stereo cassette runs both sides over the same length.
    info.cellsPerTrack = (int64_t) minutes * 60 * info.cellRate;
    for (int t = 0; t < 4; ++t)
        info.tracks.push_back ({ CassFiles::pcm (t), CassFiles::map (t), "pcm-s16le", 0.5 });
    info.condition = c;
    return info;
}

//==============================================================================
namespace
{
    // Known members are read from these lists; anything else in the object is kept in "unknown".
    Json leftovers (const Json& j, std::initializer_list<const char*> known)
    {
        auto out = Json::object();
        for (auto& [k, v] : j.members())
        {
            bool isKnown = false;
            for (auto* name : known) isKnown = isKnown || k == name;
            if (! isKnown) out.set (k) = v;
        }
        return out;
    }

    void appendUnknown (Json& j, const Json& unknown)
    {
        for (auto& [k, v] : unknown.members())
            if (! j.has (k)) j.set (k) = v;
    }

    Json toJson (const Formulation& f)
    {
        auto j = Json::object();
        j.set ("name", f.name);
        j.set ("line", f.line);
        j.set ("position", f.position);
        j.set ("iecType", f.iecType);
        j.set ("eqMicroseconds", f.eqMicroseconds);
        j.set ("mol100", f.mol100);
        j.set ("mol400", f.mol400);
        j.set ("sol10k", f.sol10k);
        j.set ("snrDbA", f.snrDbA);
        j.set ("modNoiseDb", f.modNoiseDb);
        j.set ("sensitivityDb", f.sensitivityDb);
        j.set ("basedOn", f.basedOn);
        j.set ("accent", f.accent);
        return j;
    }

    Formulation formulationFrom (const Json& j)
    {
        Formulation f;
        f.name = j["name"].string();
        f.line = j["line"].string();
        f.position = j["position"].string();
        f.iecType = (int) j["iecType"].integer (1);
        f.eqMicroseconds = (int) j["eqMicroseconds"].integer (120);
        f.mol100 = j["mol100"].number();
        f.mol400 = j["mol400"].number();
        f.sol10k = j["sol10k"].number();
        f.snrDbA = j["snrDbA"].number();
        f.modNoiseDb = j["modNoiseDb"].number();
        f.sensitivityDb = j["sensitivityDb"].number();
        f.basedOn = j["basedOn"].string();
        f.accent = j["accent"].string();
        return f;
    }

    Json toJson (const Condition& c)
    {
        auto j = Json::object();
        j.set ("grade", c.grade);
        j.set ("hissDb", c.hissDb);
        j.set ("dropoutsPerMinute", c.dropoutsPerMinute);
        j.set ("trebleLossDb", c.trebleLossDb);
        j.set ("modNoiseDb", c.modNoiseDb);
        j.set ("shellFlutterPct", c.shellFlutterPct);
        return j;
    }

    Condition conditionFrom (const Json& j)
    {
        Condition c;
        c.grade = j["grade"].string ("new");
        c.hissDb = j["hissDb"].number();
        c.dropoutsPerMinute = j["dropoutsPerMinute"].number();
        c.trebleLossDb = j["trebleLossDb"].number();
        c.modNoiseDb = j["modNoiseDb"].number();
        c.shellFlutterPct = j["shellFlutterPct"].number();
        return c;
    }

    bool readText (const fs::path& file, std::string& out, size_t maxSize = 16u << 20)
    {
        std::ifstream in (file, std::ios::binary);
        if (! in) return false;
        in.seekg (0, std::ios::end);
        auto size = (size_t) in.tellg();
        if (size > maxSize) return false;
        in.seekg (0);
        out.resize (size);
        in.read (out.data(), (std::streamsize) size);
        return (bool) in;
    }
}

Json toJson (const CassetteInfo& c)
{
    auto j = Json::object();
    j.set ("format", "cass");
    j.set ("version", c.version);
    j.set ("id", c.id);
    j.set ("created", c.created);
    j.set ("madeBy", c.madeBy);
    j.set ("label", c.label);
    j.set ("formulation", toJson (c.formulation));
    auto& len = j.set ("length");
    len.set ("minutesPerSide", c.minutesPerSide);
    len.set ("cellRate", c.cellRate);
    len.set ("referenceSpeedIps", c.referenceSpeedIps);
    len.set ("cellsPerTrack", c.cellsPerTrack);
    len.set ("blockCells", c.blockCells);
    j.set ("layout", layoutName (c.layout));
    auto& tracks = j.set ("tracks", Json::array());
    for (auto& t : c.tracks)
    {
        auto tj = Json::object();
        tj.set ("pcm", t.pcm);
        tj.set ("map", t.map);
        tj.set ("encoding", t.encoding);
        tj.set ("fluxScale", t.fluxScale);
        tracks.push (tj);
    }
    auto& rec = j.set ("recorders", Json::array());
    for (auto& r : c.recorders) rec.push (r);
    j.set ("condition", toJson (c.condition));
    auto& inlay = j.set ("inlay");
    inlay.set ("layout", c.inlayLayout);
    auto& lines = inlay.set ("lines", Json::array());
    for (auto& l : c.inlay) lines.push (l);
    appendUnknown (j, c.unknown);
    return j;
}

bool fromJson (const Json& j, CassetteInfo& c, std::string& error)
{
    if (! j.isObject() || j["format"].string() != "cass") { error = "not a cassette description"; return false; }
    c = {};
    c.version = (int) j["version"].integer (0);
    if (c.version < 1) { error = "no format version"; return false; }
    // A newer minor addition is read as far as this version understands it; the rest is kept.
    c.id = j["id"].string();
    c.created = j["created"].string();
    c.madeBy = j["madeBy"].string();
    c.label = j["label"].string();
    c.formulation = formulationFrom (j["formulation"]);
    const auto& len = j["length"];
    c.minutesPerSide = (int) len["minutesPerSide"].integer (30);
    c.cellRate = (int) len["cellRate"].integer (32000);
    c.referenceSpeedIps = len["referenceSpeedIps"].number (1.875);
    c.cellsPerTrack = len["cellsPerTrack"].integer (0);
    c.blockCells = (int) len["blockCells"].integer (3200);
    auto layout = layoutFromName (j["layout"].string());
    if (! layout) { error = "unknown track layout: " + j["layout"].string(); return false; }
    c.layout = *layout;
    for (auto& t : j["tracks"].elements())
        c.tracks.push_back ({ t["pcm"].string(), t["map"].string(), t["encoding"].string ("pcm-s16le"), t["fluxScale"].number (0.5) });
    for (auto& r : j["recorders"].elements()) c.recorders.push_back (r.string());
    c.condition = conditionFrom (j["condition"]);
    c.inlayLayout = j["inlay"]["layout"].string();
    for (auto& l : j["inlay"]["lines"].elements()) c.inlay.push_back (l.string());
    c.unknown = leftovers (j, { "format", "version", "id", "created", "madeBy", "label", "formulation", "length",
                                "layout", "tracks", "recorders", "condition", "inlay" });

    // Sanity, since files come from anywhere.
    if (c.id.empty()) { error = "no id"; return false; }
    if (c.cellRate < 1000 || c.cellRate > 400000 || c.blockCells < 1) { error = "bad cell rate"; return false; }
    if (c.cellsPerTrack < 0 || c.cellsPerTrack > (int64_t) c.cellRate * 60 * 240) { error = "bad length"; return false; }
    if (c.tracks.empty() || c.tracks.size() > 16) { error = "bad track list"; return false; }
    for (auto& t : c.tracks)
    {
        if (t.encoding != "pcm-s16le") { error = "unknown track encoding: " + t.encoding; return false; }
        if (! ZipReader::isSafeName (t.pcm) || ! ZipReader::isSafeName (t.map)) { error = "bad track file name"; return false; }
        if (! (t.fluxScale > 0.0 && t.fluxScale <= 1.0)) { error = "bad flux scale"; return false; }
    }
    return true;
}

Json toJson (const CassetteState& s)
{
    auto j = Json::object();
    auto& tabs = j.set ("writeProtect");
    tabs.set ("A", s.protectA);
    tabs.set ("B", s.protectB);
    j.set ("position", s.position);
    j.set ("lastUsed", s.lastUsed);
    appendUnknown (j, s.unknown);
    return j;
}

bool fromJson (const Json& j, CassetteState& s, std::string& error)
{
    if (! j.isObject()) { error = "not a cassette state"; return false; }
    s = {};
    s.protectA = j["writeProtect"]["A"].boolean();
    s.protectB = j["writeProtect"]["B"].boolean();
    s.position = j["position"].number();
    s.lastUsed = j["lastUsed"].string();
    s.unknown = leftovers (j, { "writeProtect", "position", "lastUsed" });
    return true;
}

//==============================================================================
bool writeTextAtomically (const fs::path& file, const std::string& text)
{
    auto tmp = file;
    tmp += ".tmp";
    {
        std::ofstream out (tmp, std::ios::binary | std::ios::trunc);
        if (! out) return false;
        out.write (text.data(), (std::streamsize) text.size());
        out.close();
        if (! out) return false;
    }
    std::error_code ec;
    fs::rename (tmp, file, ec);
    return ! ec;
}

static bool writeBinaryAtomically (const fs::path& file, const std::vector<uint8_t>& bytes)
{
    return writeTextAtomically (file, std::string (bytes.begin(), bytes.end()));
}

bool readManifest (const fs::path& folder, CassetteInfo& info, std::string& error)
{
    std::string text;
    if (! readText (folder / CassFiles::manifest, text)) { error = "no cassette.json"; return false; }
    auto j = Json::parse (text, &error);
    return j && fromJson (*j, info, error);
}

bool writeManifest (const fs::path& folder, const CassetteInfo& info, std::string& error)
{
    std::error_code ec;
    fs::create_directories (folder, ec);
    if (! writeTextAtomically (folder / CassFiles::manifest, toJson (info).dump()))
    {
        error = "could not write cassette.json";
        return false;
    }
    return true;
}

bool readState (const fs::path& folder, CassetteState& state, std::string& error)
{
    std::string text;
    if (! readText (folder / CassFiles::state, text)) { state = {}; return true; }   // optional: a fresh cassette
    auto j = Json::parse (text, &error);
    return j && fromJson (*j, state, error);
}

bool writeState (const fs::path& folder, const CassetteState& state, std::string& error)
{
    std::error_code ec;
    fs::create_directories (folder, ec);
    if (! writeTextAtomically (folder / CassFiles::state, toJson (state).dump()))
    {
        error = "could not write state.json";
        return false;
    }
    return true;
}

bool readMap (const fs::path& file, size_t blocks, TrackMap& map)
{
    map = {};
    map.resize (blocks);
    std::ifstream in (file, std::ios::binary);
    if (! in) return true;   // a track that was never recorded has no map
    std::vector<uint8_t> rec (4);
    for (size_t b = 0; b < blocks; ++b)
    {
        in.read (reinterpret_cast<char*> (rec.data()), 4);
        if (in.gcount() < 4) break;
        map.flags[b] = rec[0];
        map.recorder[b] = rec[1];
        map.speedMilli[b] = (uint16_t) (rec[2] | (rec[3] << 8));
    }
    return true;
}

bool writeMap (const fs::path& file, const TrackMap& map)
{
    std::vector<uint8_t> bytes (map.flags.size() * 4);
    for (size_t b = 0; b < map.flags.size(); ++b)
    {
        bytes[b * 4]     = map.flags[b];
        bytes[b * 4 + 1] = b < map.recorder.size() ? map.recorder[b] : 0;
        auto s = b < map.speedMilli.size() ? map.speedMilli[b] : (uint16_t) 0;
        bytes[b * 4 + 2] = (uint8_t) (s & 0xff);
        bytes[b * 4 + 3] = (uint8_t) (s >> 8);
    }
    return writeBinaryAtomically (file, bytes);
}

bool readWear (const fs::path& file, size_t seconds, std::vector<uint16_t>& passes)
{
    passes.assign (seconds, 0);
    std::ifstream in (file, std::ios::binary);
    if (! in) return true;   // never played
    for (size_t s = 0; s < seconds; ++s)
    {
        uint8_t v[2];
        in.read (reinterpret_cast<char*> (v), 2);
        if (in.gcount() < 2) break;
        passes[s] = (uint16_t) (v[0] | (v[1] << 8));
    }
    return true;
}

bool writeWear (const fs::path& file, const std::vector<uint16_t>& passes)
{
    std::vector<uint8_t> bytes (passes.size() * 2);
    for (size_t s = 0; s < passes.size(); ++s)
    {
        bytes[s * 2] = (uint8_t) (passes[s] & 0xff);
        bytes[s * 2 + 1] = (uint8_t) (passes[s] >> 8);
    }
    return writeBinaryAtomically (file, bytes);
}

//==============================================================================
std::vector<std::string> cassetteFiles (const fs::path& folder, const CassetteInfo& info)
{
    std::vector<std::string> names { CassFiles::manifest, CassFiles::state, CassFiles::wear };
    for (auto& t : info.tracks) { names.push_back (t.pcm); names.push_back (t.map); }

    std::error_code ec;
    if (fs::is_directory (folder / "art", ec))
    {
        std::vector<std::string> art;
        for (auto& e : fs::directory_iterator (folder / "art", ec))
            if (e.is_regular_file()) art.push_back (std::string ("art/") + utf8FromPath (e.path().filename()));
        std::sort (art.begin(), art.end());
        names.insert (names.end(), art.begin(), art.end());
    }

    std::vector<std::string> present;
    for (auto& n : names)
        if (fs::is_regular_file (folder / pathFromUtf8 (n), ec)) present.push_back (n);
    return present;
}

bool packCassette (const fs::path& folder, const fs::path& cassFile, std::string& error)
{
    CassetteInfo info;
    if (! readManifest (folder, info, error)) return false;

    auto tmp = cassFile;
    tmp += ".tmp";
    {
        ZipWriter zip (tmp);
        for (auto& name : cassetteFiles (folder, info))
            zip.addFile (name, folder / pathFromUtf8 (name));
        if (! zip.finish()) { error = zip.error(); std::error_code ec; fs::remove (tmp, ec); return false; }
    }
    std::error_code ec;
    fs::rename (tmp, cassFile, ec);
    if (ec) { error = "could not write " + utf8FromPath (cassFile); return false; }
    return true;
}

bool readPackedManifest (const fs::path& cassFile, CassetteInfo& info, std::string& error)
{
    ZipReader zip (cassFile);
    if (! zip.ok()) { error = zip.error(); return false; }
    std::string text;
    if (! zip.read (CassFiles::manifest, text)) { error = "not a cassette: " + zip.error(); return false; }
    auto j = Json::parse (text, &error);
    return j && fromJson (*j, info, error);
}

bool unpackCassette (const fs::path& cassFile, const fs::path& folder, std::string& error)
{
    ZipReader zip (cassFile);
    if (! zip.ok()) { error = zip.error(); return false; }

    std::string text;
    if (! zip.read (CassFiles::manifest, text)) { error = "not a cassette: " + zip.error(); return false; }
    auto j = Json::parse (text, &error);
    CassetteInfo info;
    if (! j || ! fromJson (*j, info, error)) return false;

    // Only the files the format defines; anything else in the archive stays in it.
    std::set<std::string> allowed { CassFiles::manifest, CassFiles::state, CassFiles::wear };
    for (auto& t : info.tracks) { allowed.insert (t.pcm); allowed.insert (t.map); }

    std::error_code ec;
    fs::create_directories (folder, ec);
    if (ec) { error = "could not create " + utf8FromPath (folder); return false; }
    for (auto& e : zip.getEntries())
    {
        const bool art = e.name.rfind ("art/", 0) == 0 && e.name.find ('/', 4) == std::string::npos;
        if (! allowed.count (e.name) && ! art) continue;
        auto target = folder / pathFromUtf8 (e.name);
        if (art) fs::create_directories (folder / "art", ec);
        if (! zip.extract (e.name, target)) { error = zip.error(); return false; }
    }
    return true;
}
} // namespace tfmt
