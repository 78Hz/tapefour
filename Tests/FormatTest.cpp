// Tests for the cassette and deck format library (Formats/): no JUCE, as other products will use it.

#include "Cassette.h"
#include "Deck.h"
#include "Zip.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>

using namespace tfmt;
namespace fs = std::filesystem;

namespace
{
    int failures = 0;

    void check (bool ok, const char* what)
    {
        std::printf ("%s  %s\n", ok ? "PASS" : "FAIL", what);
        if (! ok) ++failures;
    }

    std::string slurp (const fs::path& p)
    {
        std::ifstream in (p, std::ios::binary);
        return std::string ((std::istreambuf_iterator<char> (in)), std::istreambuf_iterator<char>());
    }

    void spit (const fs::path& p, const std::string& s)
    {
        std::ofstream out (p, std::ios::binary | std::ios::trunc);
        out.write (s.data(), (std::streamsize) s.size());
    }

    Formulation sd()
    {
        Formulation f;
        f.name = "SD"; f.line = "STUDIO DYNAMIC"; f.position = "TYPE I \xc2\xb7 NORMAL"; f.basedOn = "TDK AD"; f.accent = "#e8702a";
        f.iecType = 1; f.eqMicroseconds = 120;
        f.mol100 = 3.8; f.mol400 = 6.2; f.sol10k = -8.2; f.snrDbA = 60.3; f.modNoiseDb = -44.3; f.sensitivityDb = -0.3;
        return f;
    }
}

int main()
{
    auto root = fs::temp_directory_path() / ("tfmt-test-" + newUuid());
    fs::create_directories (root);

    // ---- JSON ----
    {
        auto j = Json::object();
        j.set ("name", "tape \xc3\xa9 \"one\"\n");
        j.set ("n", 0.1);
        j.set ("big", (int64_t) 86400000);
        j.set ("neg", -1.5e-7);
        j.set ("ok", true);
        auto& a = j.set ("list", Json::array());
        a.push (1); a.push ("two"); a.push (Json());
        auto text = j.dump();
        auto back = Json::parse (text);
        check (back && *back == j, "json: a value survives writing and reading");
        check (back && (*back)["big"].integer() == 86400000 && sameNumber ((*back)["n"].number(), 0.1), "json: numbers come back exactly");
        check (back && back->dump() == text, "json: writing again gives the same text");
        check (! Json::parse ("{\"a\":1,}"), "json: a trailing comma is refused");
        check (! Json::parse (std::string (200, '[')), "json: hostile nesting is refused");
        auto u = Json::parse ("\"\\u00e9\\ud83d\\ude00\"");
        check (u && u->string() == "\xc3\xa9\xf0\x9f\x98\x80", "json: \\u escapes and surrogate pairs decode to UTF-8");
        check (Json::parse ("{\"x\":1}")->operator[] ("missing")["deeper"].number (7) > 6.9, "json: missing members read as their defaults");
    }

    // ---- CRC and zip ----
    {
        check (crc32 ("123456789", 9) == 0xcbf43926u, "zip: CRC-32 check value");
        auto file = root / "t.zip";
        {
            ZipWriter z (file);
            z.addData ("a.txt", "hello", 5);
            z.addData ("dir/b.bin", "\0\1\2", 3);
            check (z.finish(), "zip: an archive is written");
        }
        ZipReader r (file);
        std::string s;
        check (r.ok() && r.getEntries().size() == 2, "zip: its directory reads back");
        check (r.read ("a.txt", s) && s == "hello", "zip: an entry reads back with its checksum");
        check (! ZipReader::isSafeName ("../x") && ! ZipReader::isSafeName ("/x") && ! ZipReader::isSafeName ("a\\b")
                   && ! ZipReader::isSafeName ("c:x") && ZipReader::isSafeName ("art/cover.png"),
               "zip: unsafe entry names are refused");

        // Damage a byte of data: the checksum catches it.
        auto bytes = slurp (file);
        auto at = bytes.find ("hello");
        bytes[at] = 'j';
        spit (file, bytes);
        ZipReader damaged (file);
        check (! damaged.read ("a.txt", s), "zip: damaged data is detected");
    }

    // ---- a cassette: folder, pack, unpack ----
    {
        auto folder = root / "cassette";
        auto info = CassetteInfo::make (sd(), 1, Layout::fourTrackOneWay, Condition::forGrade ("worn"), "FormatTest");
        info.label = "demo songs \xe2\x80\x93 oct";
        info.inlayLayout = "tf4-inlay-1";
        info.inlay = { "DEMO SONGS", "000", "Harbour" };
        info.recorders = { "tapefour.tf-4" };
        info.unknown.set ("fromTheFuture", Json::object()).set ("x", 1);
        std::string err;
        check (writeManifest (folder, info, err), "cassette: the description is written");

        CassetteInfo read;
        check (readManifest (folder, read, err) && read.id == info.id && read.formulation == info.formulation
                   && read.condition == info.condition && read.tracks == info.tracks && read.inlay == info.inlay
                   && read.label == info.label && read.cellsPerTrack == 60 * 32000,
               "cassette: the description reads back");
        check (read.unknown["fromTheFuture"]["x"].integer() == 1, "cassette: members from a newer version are kept");

        CassetteState st;
        st.protectA = true;
        st.position = 1234.5;
        st.lastUsed = isoNow();
        check (writeState (folder, st, err), "cassette: the state is written");
        CassetteState st2;
        check (readState (folder, st2, err) && st2.protectA && ! st2.protectB && sameNumber (st2.position, 1234.5), "cassette: the state reads back");

        // Two tracks of recording, one map, and some wear.
        std::mt19937 gen (7);
        std::string pcm (32000 * 2 * 10, '\0');
        for (auto& c : pcm) c = (char) gen();
        spit (folder / CassFiles::pcm (0), pcm);
        spit (folder / CassFiles::pcm (2), pcm.substr (0, 64000));
        TrackMap map;
        map.resize ((size_t) info.numBlocks());
        map.flags[3] = TrackMap::recorded | TrackMap::nrEncoded | (1 << 2);
        map.recorder[3] = 1;
        map.speedMilli[3] = 2000;
        check (writeMap (folder / CassFiles::map (0), map), "cassette: a track map is written");
        TrackMap map2;
        check (readMap (folder / CassFiles::map (0), (size_t) info.numBlocks(), map2) && map2.flags == map.flags
                   && map2.speedMilli == map.speedMilli && map2.recorder == map.recorder,
               "cassette: a track map reads back");
        std::vector<uint16_t> wear ((size_t) info.numSeconds(), 0);
        wear[5] = 300;
        check (writeWear (folder / CassFiles::wear, wear), "cassette: wear is written");
        fs::create_directories (folder / "art");
        spit (folder / "art" / "cover.png", "not really a png");
        spit (folder / "session.xml", "<DECK/>");   // a session is never packed

        auto cass = root / "demo.cass";
        check (packCassette (folder, cass, err), "cassette: packed into a .cass file");
        ZipReader z (cass);
        check (z.ok() && z.find ("session.xml") == nullptr && z.find ("art/cover.png") != nullptr && z.find (CassFiles::pcm (1)) == nullptr,
               "cassette: the .cass holds the cassette's files and not the session");

        CassetteInfo packed;
        check (readPackedManifest (cass, packed, err) && packed.id == info.id, "cassette: its description reads without unpacking");

        auto out = root / "unpacked";
        check (unpackCassette (cass, out, err), "cassette: unpacked again");
        bool same = true;
        for (auto& name : cassetteFiles (folder, info))
            same = same && slurp (folder / pathFromUtf8 (name)) == slurp (out / pathFromUtf8 (name));
        check (same && ! fs::exists (out / "session.xml"), "cassette: every file comes back byte for byte");

        // A cassette whose description is damaged is refused before anything is unpacked.
        auto bad = root / "bad";
        fs::create_directories (bad);
        spit (bad / CassFiles::manifest, "{\"format\":\"cass\",\"version\":1,\"id\":\"x\",\"layout\":\"8-track\",\"tracks\":[]}");
        CassetteInfo nope;
        check (! readManifest (bad, nope, err), "cassette: an unknown layout is refused");
        spit (bad / CassFiles::manifest, "{\"format\":\"cass\",\"version\":1,\"id\":\"x\",\"layout\":\"4-track-one-way\","
                                         "\"tracks\":[{\"pcm\":\"../../evil\",\"map\":\"m\"}]}");
        check (! readManifest (bad, nope, err), "cassette: a track file outside the cassette is refused");
    }

    // ---- a deck ----
    {
        DeckDescription d;
        d.id = "tapefour.tf-4";
        d.name = "TAPE\xc2\xb7" "FOUR TF-4";
        d.kind = "multitracker";
        d.nrSystems = { "compander-2to1", "sliding-b", "sliding-c" };
        d.speedsIps = { 1.875, 3.75 };
        d.applyCondition ("worn");
        d.honoursTapeCondition = false;
        std::string err;
        auto file = root / "tf4.deck";
        check (writeDeck (file, d, err), "deck: written");
        DeckDescription back;
        check (readDeck (file, back, err) && back.id == d.id && sameNumber (back.transport.wowPct, d.transport.wowPct)
                   && sameNumber (back.heads.cutoffHz, d.heads.cutoffHz) && back.condition == "worn" && ! back.honoursTapeCondition
                   && back.speedsIps == d.speedsIps && back.nrSystems == d.nrSystems,
               "deck: reads back");
        spit (file, "{\"format\":\"deck\",\"version\":1,\"transport\":{\"wowPct\":900}}");
        check (readDeck (file, back, err) && back.transport.wowPct <= 5.0, "deck: impossible values are kept to what a machine can be");
    }

    std::error_code ec;
    fs::remove_all (root, ec);
    std::printf (failures == 0 ? "ALL PASSED (0 failures)\n" : "%d FAILURES\n", failures);
    return failures == 0 ? 0 : 1;
}
