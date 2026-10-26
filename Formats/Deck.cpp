#include "Deck.h"
#include "Cassette.h"

#include <fstream>

namespace tfmt
{
const std::vector<std::string>& DeckDescription::conditions()
{
    static const std::vector<std::string> c { "new", "serviced", "worn", "neglected" };
    return c;
}

void DeckDescription::applyCondition (const std::string& grade)
{
    // serviced is a machine in good everyday order; the others either side of it.
    struct Values { double wow, flutter, drift, cutoff, crosstalk; };
    Values v { 0.15, 0.075, 0.2, 15157.0, -65.0 };
    if (grade == "new")            v = { 0.08, 0.045, 0.1, 18000.0, -70.0 };
    else if (grade == "worn")      v = { 0.30, 0.15, 0.4, 9330.0, -55.0 };
    else if (grade == "neglected") v = { 0.45, 0.24, 0.7, 6598.0, -45.0 };
    condition = (grade == "new" || grade == "worn" || grade == "neglected") ? grade : "serviced";
    transport.wowPct = v.wow;
    transport.flutterPct = v.flutter;
    transport.driftPct = v.drift;
    heads.cutoffHz = v.cutoff;
    heads.crosstalkDb = v.crosstalk;
}

Json toJson (const DeckDescription& d)
{
    auto j = Json::object();
    j.set ("format", "deck");
    j.set ("version", d.version);
    j.set ("id", d.id);
    j.set ("name", d.name);
    j.set ("maker", d.maker);
    j.set ("kind", d.kind);

    auto& can = j.set ("capabilities");
    auto& layouts = can.set ("layouts", Json::array());
    for (auto& l : d.layouts) layouts.push (l);
    can.set ("records", d.records);
    can.set ("heads", d.headCount);
    can.set ("autoReverse", d.autoReverse);
    auto& speeds = can.set ("speedsIps", Json::array());
    for (auto s : d.speedsIps) speeds.push (s);
    auto& nr = can.set ("nrSystems", Json::array());
    for (auto& s : d.nrSystems) nr.push (s);
    can.set ("tapeSwitch", d.tapeSwitch);

    auto& tr = j.set ("transport");
    tr.set ("wowPct", d.transport.wowPct);
    tr.set ("flutterPct", d.transport.flutterPct);
    tr.set ("driftPct", d.transport.driftPct);
    tr.set ("speedErrorPct", d.transport.speedErrorPct);
    auto& hd = j.set ("heads");
    hd.set ("cutoffHz", d.heads.cutoffHz);
    hd.set ("crosstalkDb", d.heads.crosstalkDb);
    hd.set ("azimuthErrorMinutes", d.heads.azimuthErrorMinutes);
    j.set ("condition", d.condition);
    j.set ("honoursTapeCondition", d.honoursTapeCondition);

    for (auto& [k, v] : d.unknown.members())
        if (! j.has (k)) j.set (k) = v;
    return j;
}

bool fromJson (const Json& j, DeckDescription& d, std::string& error)
{
    if (! j.isObject() || j["format"].string() != "deck") { error = "not a deck description"; return false; }
    d = {};
    d.version = (int) j["version"].integer (0);
    if (d.version < 1) { error = "no format version"; return false; }
    d.id = j["id"].string();
    d.name = j["name"].string();
    d.maker = j["maker"].string();
    d.kind = j["kind"].string();

    const auto& can = j["capabilities"];
    if (can.has ("layouts"))
    {
        d.layouts.clear();
        for (auto& l : can["layouts"].elements()) d.layouts.push_back (l.string());
    }
    d.records = can["records"].boolean (true);
    d.headCount = (int) can["heads"].integer (2);
    d.autoReverse = can["autoReverse"].boolean (false);
    if (can.has ("speedsIps"))
    {
        d.speedsIps.clear();
        for (auto& s : can["speedsIps"].elements()) d.speedsIps.push_back (s.number (1.875));
    }
    for (auto& s : can["nrSystems"].elements()) d.nrSystems.push_back (s.string());
    d.tapeSwitch = can["tapeSwitch"].string ("manual");

    const auto& tr = j["transport"];
    d.transport.wowPct = tr["wowPct"].number (d.transport.wowPct);
    d.transport.flutterPct = tr["flutterPct"].number (d.transport.flutterPct);
    d.transport.driftPct = tr["driftPct"].number (d.transport.driftPct);
    d.transport.speedErrorPct = tr["speedErrorPct"].number (0.0);
    const auto& hd = j["heads"];
    d.heads.cutoffHz = hd["cutoffHz"].number (d.heads.cutoffHz);
    d.heads.crosstalkDb = hd["crosstalkDb"].number (d.heads.crosstalkDb);
    d.heads.azimuthErrorMinutes = hd["azimuthErrorMinutes"].number (0.0);
    d.condition = j["condition"].string ("serviced");
    d.honoursTapeCondition = j["honoursTapeCondition"].boolean (true);

    auto leftovers = Json::object();
    for (auto& [k, v] : j.members())
        if (k != "format" && k != "version" && k != "id" && k != "name" && k != "maker" && k != "kind"
            && k != "capabilities" && k != "transport" && k != "heads" && k != "condition" && k != "honoursTapeCondition")
            leftovers.set (k) = v;
    d.unknown = leftovers;

    // Keep a hostile or broken file within what a machine can be.
    auto clampTo = [] (double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); };
    d.transport.wowPct = clampTo (d.transport.wowPct, 0.0, 5.0);
    d.transport.flutterPct = clampTo (d.transport.flutterPct, 0.0, 5.0);
    d.transport.driftPct = clampTo (d.transport.driftPct, 0.0, 5.0);
    d.transport.speedErrorPct = clampTo (d.transport.speedErrorPct, -10.0, 10.0);
    d.heads.cutoffHz = clampTo (d.heads.cutoffHz, 1000.0, 30000.0);
    d.heads.crosstalkDb = clampTo (d.heads.crosstalkDb, -120.0, -20.0);
    d.heads.azimuthErrorMinutes = clampTo (d.heads.azimuthErrorMinutes, -120.0, 120.0);
    return true;
}

bool readDeck (const std::filesystem::path& file, DeckDescription& d, std::string& error)
{
    std::ifstream in (file, std::ios::binary);
    if (! in) { error = "no deck file"; return false; }
    std::string text ((std::istreambuf_iterator<char> (in)), std::istreambuf_iterator<char>());
    if (text.size() > (1u << 20)) { error = "deck file too large"; return false; }
    auto j = Json::parse (text, &error);
    return j && fromJson (*j, d, error);
}

bool writeDeck (const std::filesystem::path& file, const DeckDescription& d, std::string& error)
{
    if (! writeTextAtomically (file, toJson (d).dump()))
    {
        error = "could not write the deck file";
        return false;
    }
    return true;
}
} // namespace tfmt
