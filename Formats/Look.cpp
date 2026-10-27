#include "Look.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

namespace tfmt
{
namespace
{
    Json point (const Look::Point& p)
    {
        auto j = Json::object();
        j.set ("x", p.x);
        j.set ("y", p.y);
        return j;
    }

    Look::Point point (const Json& j, Look::Point fallback)
    {
        return { j["x"].number (fallback.x), j["y"].number (fallback.y) };
    }

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

    bool safeImageName (const std::string& name)
    {
        return name.rfind ("art/", 0) == 0 && name.find ('/', 4) == std::string::npos
               && name.find ("..") == std::string::npos && name.find ('\\') == std::string::npos && name.size() > 4;
    }
}

Json toJson (const Look& l)
{
    auto j = Json::object();
    j.set ("format", "cass-look");
    j.set ("version", 1);
    j.set ("design", l.design);
    j.set ("labelText", l.labelText);
    auto& shell = j.set ("shell", Json::object());
    shell.set ("width", l.width);
    shell.set ("height", l.height);
    shell.set ("unit", "0.1mm");
    j.set ("scale", l.scale);

    auto& layers = j.set ("layers", Json::object());
    layers.set ("back", l.back);
    auto& front = layers.set ("front", Json::object());
    front.set ("A", l.frontA);
    front.set ("B", l.frontB);
    auto& hub = layers.set ("hub", Json::object());
    hub.set ("image", l.hub);
    hub.set ("size", l.hubSize);

    auto reels = Json::array();
    for (auto& r : l.reels) reels.push (point (r));
    j.set ("reels", reels);

    auto& pack = j.set ("pack", Json::object());
    pack.set ("minDiameter", l.pack.minDiameter);
    pack.set ("fullDiameter", l.pack.fullDiameter);
    pack.set ("inner", l.pack.inner);
    pack.set ("outer", l.pack.outer);
    auto& rings = pack.set ("rings", Json::object());
    rings.set ("colour", l.pack.ring);
    rings.set ("width", l.pack.ringWidth);
    rings.set ("spacing", l.pack.ringSpacing);
    rings.set ("inset", l.pack.ringInset);
    rings.set ("stop", l.pack.ringStop);

    auto& tape = j.set ("tape", Json::object());
    tape.set ("colour", l.tape.colour);
    tape.set ("width", l.tape.width);
    auto guides = Json::array();
    for (auto& g : l.tape.guides) guides.push (point (g));
    tape.set ("guides", guides);
    tape.set ("wrapRadius", l.tape.wrapRadius);

    auto& label = j.set ("label", Json::object());
    label.set ("x", l.label.x);
    label.set ("y", l.label.y);
    label.set ("w", l.label.w);
    label.set ("h", l.label.h);

    for (auto& [k, v] : l.unknown.members()) j.set (k) = v;
    return j;
}

bool fromJson (const Json& j, Look& l, std::string& error)
{
    if (! j.isObject() || j["format"].string() != "cass-look") { error = "not a cassette look"; return false; }
    // A newer version is read for what this one knows: its additions are kept in `unknown`.
    l = {};
    l.design = j["design"].string();
    l.labelText = j["labelText"].string();
    l.width = j["shell"]["width"].number (1004);
    l.height = j["shell"]["height"].number (638);
    l.scale = std::max (0.1, j["scale"].number (1));
    l.back = j["layers"]["back"].string();
    l.frontA = j["layers"]["front"]["A"].string();
    l.frontB = j["layers"]["front"]["B"].string();
    l.hub = j["layers"]["hub"]["image"].string();
    l.hubSize = j["layers"]["hub"]["size"].number (224);
    for (size_t i = 0; i < 2; ++i)
        l.reels[i] = point (j["reels"][i], l.reels[i]);
    auto& p = j["pack"];
    l.pack.minDiameter = p["minDiameter"].number (216);
    l.pack.fullDiameter = p["fullDiameter"].number (440);
    l.pack.inner = p["inner"].string();
    l.pack.outer = p["outer"].string();
    l.pack.ring = p["rings"]["colour"].string();
    l.pack.ringWidth = p["rings"]["width"].number (2);
    l.pack.ringSpacing = std::max (1.0, p["rings"]["spacing"].number (26));
    l.pack.ringInset = p["rings"]["inset"].number (10);
    l.pack.ringStop = p["rings"]["stop"].number (104);
    auto& t = j["tape"];
    l.tape.colour = t["colour"].string();
    l.tape.width = t["width"].number (4);
    for (size_t i = 0; i < 2; ++i)
        l.tape.guides[i] = point (t["guides"][i], l.tape.guides[i]);
    l.tape.wrapRadius = t["wrapRadius"].number (30);
    auto& lb = j["label"];
    l.label = { lb["x"].number (68), lb["y"].number (398), lb["w"].number (868), lb["h"].number (80) };
    l.unknown = leftovers (j, { "format", "version", "design", "labelText", "shell", "scale", "layers", "reels", "pack", "tape", "label" });

    for (auto* name : { &l.back, &l.frontA, &l.frontB, &l.hub })
        if (! name->empty() && ! safeImageName (*name)) { error = "a look image outside art/: " + *name; return false; }
    if (l.back.empty() || l.frontA.empty() || l.hub.empty()) { error = "a look without its images"; return false; }
    if (l.frontB.empty()) l.frontB = l.frontA;
    error.clear();
    return true;
}

bool readLook (const std::filesystem::path& folder, Look& look, std::string& error)
{
    std::ifstream in (folder / "art" / "look.json", std::ios::binary);
    if (! in) { error = "no look"; return false; }
    std::stringstream ss;
    ss << in.rdbuf();
    auto j = Json::parse (ss.str(), &error);
    return j && fromJson (*j, look, error);
}

double packDiameter (const Look& l, double f) noexcept
{
    // The tape's area is the same whichever hub it is on.
    const auto lo = l.pack.minDiameter, hi = std::max (l.pack.minDiameter, l.pack.fullDiameter);
    return std::sqrt (lo * lo + (hi * hi - lo * lo) * std::clamp (f, 0.0, 1.0));
}
} // namespace tfmt
