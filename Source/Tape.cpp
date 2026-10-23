#include "Tape.h"

#include <cstdlib>

namespace tf
{
Tape::Tape (int typeIndex, int modelIndex)
    : type (juce::jlimit (0, numTapeTypes - 1, typeIndex)),
      model (juce::jlimit (0, numTapeModels - 1, modelIndex))
{
    lengthCells = (int64_t) tapeTypes[type].minutes * 60 * cellRate;
    numBlocks   = (int) ((lengthCells + blockCells - 1) / blockCells);
    numSeconds  = (int) ((lengthCells + cellRate - 1) / cellRate);

    for (int t = 0; t < numTracks; ++t)
    {
        // calloc'd zeroes are valid int16 atomics, and pages stay untouched until recorded on.
        data[t] = static_cast<Cell*> (std::calloc ((size_t) lengthCells, sizeof (Cell)));
        if (data[t] == nullptr)
            throw std::bad_alloc();

        flags[t].reset (new std::atomic<uint8_t>[(size_t) numBlocks]);
        speedMilli[t].reset (new std::atomic<uint16_t>[(size_t) numBlocks]);
        dirty[t].reset (new std::atomic<uint8_t>[(size_t) numSeconds]);

        for (int b = 0; b < numBlocks; ++b) { flags[t][b] = 0; speedMilli[t][b] = 0; }
        for (int s = 0; s < numSeconds; ++s) dirty[t][s] = 0;
    }

    id = juce::Uuid().toString();
    created = juce::Time::getCurrentTime().formatted ("%d %b %Y").toUpperCase();
    for (int i = 0; i < Inlay::numFields; ++i)
        trackSheet.add ({});
    trackSheet.set (Inlay::date, juce::Time::getCurrentTime().formatted ("%d.%m.%y"));
}

Tape::~Tape()
{
    for (auto* d : data)
        std::free (d);
}

float Tape::readInterpolated (int track, double pos) const noexcept
{
    auto i = (int64_t) std::floor (pos);
    auto f = (float) (pos - (double) i);
    auto y0 = readCell (track, i - 1), y1 = readCell (track, i), y2 = readCell (track, i + 1), y3 = readCell (track, i + 2);
    auto c1 = 0.5f * (y2 - y0);
    auto c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
    auto c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
    return ((c3 * f + c2) * f + c1) * f + y1;
}

void Tape::writeCell (int track, int64_t c, float v, bool nrEncoded, float speed, int pos) noexcept
{
    if (c < 0 || c >= lengthCells)
        return;

    data[track][c].store ((int16_t) juce::roundToInt (juce::jlimit (-1.0f, 1.0f, v * fluxScale) * 32767.0f), std::memory_order_relaxed);

    auto b = (int) (c / blockCells);
    flags[track][b].store ((uint8_t) (1 | (nrEncoded ? 2 : 0) | ((juce::jlimit (0, 2, pos) + 1) << 2)), std::memory_order_relaxed);
    speedMilli[track][b].store ((uint16_t) juce::jlimit (0, 65535, juce::roundToInt (speed * 1000.0f)), std::memory_order_relaxed);
    // Release: a saver that sees the flag also sees the cell.
    dirty[track][(size_t) (c / cellRate)].store (1, std::memory_order_release);
    dataDirty.store (true, std::memory_order_release);
}

double Tape::usedSeconds() const
{
    int last = -1;
    for (int t = 0; t < numTracks; ++t)
        for (int b = numBlocks - 1; b > last; --b)
            if (isRecorded (t, b)) { last = b; break; }
    return (last + 1) * (double) blockCells / cellRate;
}

int Tape::tracksUsed() const
{
    int n = 0;
    for (int t = 0; t < numTracks; ++t)
        for (int b = 0; b < numBlocks; ++b)
            if (isRecorded (t, b)) { ++n; break; }
    return n;
}

bool Tape::needsSaving() const noexcept
{
    return dataDirty.load() || metaDirty.load();
}

//==============================================================================
static juce::File pcmFile (const juce::File& folder, int t)  { return folder.getChildFile ("track" + juce::String (t + 1) + ".pcm"); }

bool Tape::writeAudio (const juce::File& folder)
{
    if (! folder.createDirectory())
        return false;
    if (! dataDirty.exchange (false, std::memory_order_acq_rel))
        return true;

    bool ok = true;
    std::vector<int16_t> chunk ((size_t) cellRate);
    const size_t bytesPerSecond = (size_t) cellRate * sizeof (int16_t);

    // Only the seconds that changed, plus anything beyond the current end of the file.
    for (int t = 0; t < numTracks; ++t)
    {
        int lastNeeded = -1;
        for (int s = numSeconds - 1; s >= 0; --s)
            if (dirty[t][s].load (std::memory_order_acquire) != 0) { lastNeeded = s; break; }

        if (lastNeeded < 0)
            continue;

        auto file = pcmFile (folder, t);
        auto existing = file.existsAsFile() ? file.getSize() : 0;
        juce::FileOutputStream out (file);

        if (! out.openedOk())
        {
            ok = false;
            continue;
        }

        for (int s = 0; s <= lastNeeded; ++s)
        {
            auto offset = (int64_t) s * (int64_t) bytesPerSecond;
            bool beyondEnd = offset >= existing;

            // Clear the flag before copying: if the audio thread writes this second again
            // while it's being copied, the flag goes back up and it is saved again next time.
            if (dirty[t][s].exchange (0, std::memory_order_acq_rel) == 0 && ! beyondEnd)
                continue;

            auto firstCell = (int64_t) s * cellRate;
            auto count = (size_t) std::min<int64_t> (cellRate, lengthCells - firstCell);
            for (size_t i = 0; i < count; ++i)
                chunk[i] = data[t][firstCell + (int64_t) i].load (std::memory_order_relaxed);

            out.setPosition (offset);
            if (! out.write (chunk.data(), count * sizeof (int16_t)))
            {
                ok = false;
                dirty[t][s].store (1, std::memory_order_release);
            }
        }
        out.flush();
        if (out.getStatus().failed()) ok = false;
    }

    if (! ok)
    {
        // Put back what didn't make it, so the next save tries again.
        for (int t = 0; t < numTracks; ++t)
            for (int s = 0; s < numSeconds; ++s)
                if (dirty[t][s].load()) { dataDirty = true; break; }
        dataDirty = true;
    }
    return ok;
}

std::unique_ptr<juce::XmlElement> Tape::createMetadata() const
{
    auto meta = std::make_unique<juce::XmlElement> ("TAPEFOUR_TAPE");
    meta->setAttribute ("version", 2);
    meta->setAttribute ("id", id);
    meta->setAttribute ("name", name);
    meta->setAttribute ("created", created);
    meta->setAttribute ("type", type);
    meta->setAttribute ("model", model);
    meta->setAttribute ("fluxScale", fluxScale);
    meta->setAttribute ("position", position.load());
    meta->setAttribute ("counterZero", counterZero.load());
    meta->setAttribute ("usedSeconds", usedSeconds());
    meta->setAttribute ("tracksUsed", tracksUsed());

    auto* sheet = meta->createNewChildElement ("TRACK_SHEET");
    sheet->setAttribute ("layout", "inlay1");
    for (auto& line : trackSheet)
        sheet->createNewChildElement ("LINE")->setAttribute ("text", line);

    for (int t = 0; t < numTracks; ++t)
    {
        juce::MemoryBlock fb ((size_t) numBlocks), sb ((size_t) numBlocks * 2);
        auto* fp = static_cast<uint8_t*> (fb.getData());
        auto* sp = static_cast<uint8_t*> (sb.getData());
        for (int b = 0; b < numBlocks; ++b)
        {
            fp[b] = flags[t][b].load();
            auto v = speedMilli[t][b].load();
            sp[b * 2] = (uint8_t) (v & 0xff);
            sp[b * 2 + 1] = (uint8_t) (v >> 8);
        }
        auto* tr = meta->createNewChildElement ("TRACK");
        tr->setAttribute ("index", t);
        tr->setAttribute ("flags", fb.toBase64Encoding());
        tr->setAttribute ("speeds", sb.toBase64Encoding());
    }
    return meta;
}

static bool replaceAtomically (const juce::XmlElement& xml, const juce::File& target)
{
    auto tmp = target.getSiblingFile (target.getFileName() + ".tmp");
    return xml.writeTo (tmp) && tmp.moveFileTo (target);
}

bool Tape::writeMetadata (const juce::XmlElement& meta, const juce::File& folder)
{
    return folder.createDirectory() && replaceAtomically (meta, folder.getChildFile ("tape.xml"));
}

bool Tape::writeDeck (const juce::XmlElement& deck, const juce::File& folder)
{
    return folder.createDirectory() && replaceAtomically (deck, folder.getChildFile ("deck.xml"));
}

bool Tape::save (const juce::File& folder)
{
    // Audio first: the metadata never claims a recording whose audio isn't on disk yet.
    bool ok = writeAudio (folder);
    if (deckState != nullptr && deckNeedsWriting)
    {
        if (writeDeck (*deckState, folder)) deckNeedsWriting = false;
        else ok = false;
    }
    if (auto meta = createMetadata(); meta != nullptr && writeMetadata (*meta, folder))
        metaDirty = false;
    else
        ok = false;
    return ok;
}

std::unique_ptr<Tape> Tape::load (const juce::File& folder)
{
    auto xml = juce::XmlDocument::parse (folder.getChildFile ("tape.xml"));
    if (xml == nullptr || ! xml->hasTagName ("TAPEFOUR_TAPE"))
        return {};

    std::unique_ptr<Tape> tape;
    try { tape = std::make_unique<Tape> (xml->getIntAttribute ("type", 1), xml->getIntAttribute ("model", defaultTapeModel)); }
    catch (...) { return {}; }
    tape->setFluxScale ((float) juce::jlimit (0.1, 1.0, xml->getDoubleAttribute ("fluxScale", 1.0)));

    tape->id          = xml->getStringAttribute ("id", tape->id);
    tape->name        = xml->getStringAttribute ("name");
    tape->created     = xml->getStringAttribute ("created", tape->created);
    tape->position    = juce::jlimit (0.0, (double) tape->lengthCells, xml->getDoubleAttribute ("position"));
    tape->counterZero = xml->getDoubleAttribute ("counterZero");

    if (auto* sheet = xml->getChildByName ("TRACK_SHEET"))
    {
        juce::StringArray lines;
        for (auto* line : sheet->getChildIterator())
            lines.add (line->getStringAttribute ("text"));

        if (sheet->getStringAttribute ("layout") == "inlay1")
        {
            for (int i = 0; i < std::min (lines.size(), (int) Inlay::numFields); ++i)
                tape->trackSheet.set (i, lines[i]);
        }
        else
        {
            // The old eight free lines: title, then the four tracks, then notes as songs.
            tape->trackSheet.set (Inlay::title, lines[0]);
            for (int t = 0; t < numTracks; ++t)
                tape->trackSheet.set (Inlay::trackNote (t), lines[1 + t]);
            for (int i = 0; i < 3; ++i)
                tape->trackSheet.set (Inlay::songName (i), lines[5 + i]);
        }
    }

    // The deck lives in its own file, so a big plug-in state isn't rewritten with every autosave.
    // Tapes from before kept it inside tape.xml.
    if (auto deckFile = juce::XmlDocument::parse (folder.getChildFile ("deck.xml")); deckFile != nullptr && deckFile->hasTagName ("DECK"))
        tape->deckState = std::move (deckFile);
    else if (auto* deck = xml->getChildByName ("DECK"))
    {
        tape->deckState = std::make_unique<juce::XmlElement> (*deck);
        tape->deckNeedsWriting = true;   // move it out to deck.xml at the next save
    }

    for (auto* tr : xml->getChildWithTagNameIterator ("TRACK"))
    {
        auto t = tr->getIntAttribute ("index", -1);
        if (t < 0 || t >= numTracks) continue;

        juce::MemoryBlock fb, sb;
        fb.fromBase64Encoding (tr->getStringAttribute ("flags"));
        sb.fromBase64Encoding (tr->getStringAttribute ("speeds"));
        auto nb = std::min ((int) fb.getSize(), tape->numBlocks);
        auto* fp = static_cast<const uint8_t*> (fb.getData());
        auto* sp = static_cast<const uint8_t*> (sb.getData());
        for (int b = 0; b < nb; ++b)
        {
            tape->flags[t][b] = fp[b];
            if ((size_t) b * 2 + 1 < sb.getSize())
                tape->speedMilli[t][b] = (uint16_t) (sp[b * 2] | (sp[b * 2 + 1] << 8));
        }

        auto file = pcmFile (folder, t);
        if (file.existsAsFile())
        {
            juce::FileInputStream in (file);
            if (in.openedOk())
            {
                auto cells = std::min<int64_t> (in.getTotalLength() / (int64_t) sizeof (int16_t), tape->lengthCells);
                // Nothing else can see this tape yet, so read straight into it.
                in.read (static_cast<void*> (tape->data[t]), (int) std::min<int64_t> (cells * (int64_t) sizeof (int16_t), (int64_t) std::numeric_limits<int>::max()));
            }
        }
    }

    tape->dataDirty = false;
    tape->metaDirty = tape->deckNeedsWriting;
    return tape;
}
} // namespace tf
