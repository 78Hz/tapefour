#pragma once

/*
    The deck format (.deck), version 1: a description of a cassette machine. The
    specification is docs/formats/deck-v1.md. Plain C++17.

    A .deck is one JSON file. It holds what the machine is (what it can do) and the state of
    its mechanism and heads (how well it does it). It never holds a cassette or a session.
*/

#include "Json.h"

#include <filesystem>
#include <string>
#include <vector>

namespace tfmt
{
inline constexpr int deckFormatVersion = 1;

struct DeckDescription
{
    int version = deckFormatVersion;
    std::string id;        // stable, reverse-domain style: "tapefour.tf-4"
    std::string name;      // as printed on the machine
    std::string maker;
    std::string kind;      // multitracker, hifi, portable, boombox, car

    // What it can do
    std::vector<std::string> layouts { "4-track-one-way" };   // cassette layouts it plays (see Cassette.h)
    bool records = true;
    int headCount = 2;                 // 2: one head records and plays; 3: separate, so it can play while recording
    bool autoReverse = false;
    std::vector<double> speedsIps { 1.875 };
    std::vector<std::string> nrSystems;   // compander-2to1, sliding-b, sliding-c
    std::string tapeSwitch = "manual";    // manual or auto (reads the cassette's sensing holes)

    // How well it does it
    struct Transport { double wowPct = 0.15, flutterPct = 0.075, driftPct = 0.2, speedErrorPct = 0.0; } transport;
    struct Heads { double cutoffHz = 15157.0, crosstalkDb = -65.0, azimuthErrorMinutes = 0.0; } heads;
    std::string condition = "serviced";   // new, serviced, worn, neglected
    /** Off: every cassette plays as if new (its formulation still counts, its wear and age don't). */
    bool honoursTapeCondition = true;

    Json unknown;

    /** The mechanism and heads of a machine in this condition (the rest unchanged). */
    void applyCondition (const std::string& grade);
    static const std::vector<std::string>& conditions();
};

Json toJson (const DeckDescription&);
bool fromJson (const Json&, DeckDescription&, std::string& error);
bool readDeck (const std::filesystem::path& file, DeckDescription&, std::string& error);
bool writeDeck (const std::filesystem::path& file, const DeckDescription&, std::string& error);
} // namespace tfmt
