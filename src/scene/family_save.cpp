#include "scene/family_save.hpp"

#include "content/neighborhood.hpp"
#include "content/package_rewrite.hpp"
#include "formats/binary_reader.hpp"
#include "formats/binary_writer.hpp"
#include "formats/dbpf.hpp"
#include "formats/dbpf_writer.hpp"
#include "formats/fami.hpp"
#include "formats/ngbh.hpp"
#include "formats/propset.hpp"
#include "formats/sdsc.hpp"
#include "formats/str.hpp"
#include "formats/swaf.hpp"
#include "scene/character_file.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <stdexcept>
#include <system_error>

namespace ts2::scene {

namespace {

namespace fs = std::filesystem;

constexpr std::uint32_t kLocal = 0xFFFFFFFF;
constexpr std::uint32_t kFamilyType = 0x46414D49, kDescription = 0xAACE2EFB, kDna = 0xEBFEE33F, kWants = 0xCD95548E,
                        kStringTable = 0x53545223, kNeighborhood = 0x4E474248, kNeighborList = 0x2C310F46, kNextNeighbor = 0xCC2A6A34;
// The townie and NPC pools' family records, past every family the player makes.
constexpr std::uint32_t kFirstPool = 0x7FFE;
// A new family's funds: 20,000 (Create-a-Sim's kDefaultFamilyFunds), and 5,000 more for each Sim past
// the first, as the game gave a family of two 25,000.
constexpr std::int32_t kFunds = 20000, kFundsPerSim = 5000;

// Person data a new Sim's description sets (see the stage notes: the game's own new Sims).
constexpr std::size_t kNice = 2, kActive = 3, kPlayful = 5, kOutgoing = 6, kNeat = 7, kAspiration = 0x2E, kBornNeat = 0x2F, kBornNice = 0x30,
                      kBornActive = 0x31, kBornOutgoing = 0x32, kBornPlayful = 0x33, kAge = 0x3A, kFamilyNumber = 0x3D, kGender = 0x41,
                      kZodiac = 0x46, kFatness = 0x52, kVoice = 0x58, kSelection = 0x9E, kAspirationScore = 0xA0, kAspirationRaw = 0xA2,
                      kZodiacPersonality = 0xC7, kHobby = 0xD7;
// The value the game keeps at 0xC7 for each sign, Aries first.
constexpr std::array<std::int16_t, 12> kSignValues = {97, 538, 324, 144, 360, 529, 298, 85, 390, 521, 162, 644};
// Wants' and inventories' versions when the neighborhood has none to copy.
constexpr std::uint32_t kInventoryVersion = 190;
// Tokens the game puts in a new Sim's inventory with no values.
constexpr std::array<std::uint32_t, 2> kNewSimTokens = {0xB424F345, 0x33F6E0FB};

[[noreturn]] void fail(const std::string& what) { throw std::runtime_error("saving the family: " + what); }

std::int16_t ageCode(std::uint32_t age) {
    switch (age) {
    case 0x20: return 1;   // baby
    case 0x01: return 2;   // toddler
    case 0x02: return 3;   // child
    case 0x04: return 16;  // teen
    case 0x10: return 51;  // elder
    default: return 19;    // adult
    }
}

std::int16_t aspirationFlag(int aspiration, std::uint32_t age) {
    if (age == 0x01 || age == 0x02) {
        return 64;  // grow up
    }
    return aspiration >= 1 && aspiration <= 8 ? static_cast<std::int16_t>(1 << (aspiration - 1)) : std::int16_t{0};
}

std::string property(const std::map<std::string, std::string>& p, const char* key) {
    const auto it = p.find(key);
    return it != p.end() ? it->second : std::string();
}

// A tone as the game writes it in a neighborhood: a GUID with its dashes.
std::string dashed(const std::string& tone) {
    std::string d;
    for (const char c : tone) {
        if (std::isxdigit(static_cast<unsigned char>(c))) {
            d += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
    }
    if (d.size() != 32) {
        return tone;
    }
    return d.substr(0, 8) + "-" + d.substr(8, 4) + "-" + d.substr(12, 4) + "-" + d.substr(16, 4) + "-" + d.substr(20);
}

// The Sim's DNA as the game writes a new Sim's: its skin, hair and eye colours and the face parts it
// passes on, each with its recessive copy (0x10000000 + the key), in the game's order.
std::vector<std::uint8_t> dna(const NewFamilySim& sim) {
    const auto& p = sim.made.data.properties;
    const std::string skin = dashed(property(p, "skincolor")), hair = dashed(property(p, "haircolor")), eyes = dashed(property(p, "eyecolor"));
    std::vector<propset::TextProperty> out;
    auto add = [&](std::uint32_t key, const std::string& value, const std::string& recessive) {
        out.push_back({"AnyString", std::to_string(key), "0x0b8bea18", value});
        out.push_back({"AnyString", std::to_string(0x10000000u + key), "0x0b8bea18", recessive});
    };
    add(6, hair, hair);
    add(2, hair, hair);
    add(1, skin, skin);
    add(3, eyes, eyes);
    add(5, "nose,brow,cheek,jaw,", "");
    return propset::encodeText(out);
}

std::vector<std::uint8_t> noWants() {
    swaf::WantsAndFears w;
    w.version = 6;
    w.maxWants = 4;
    w.maxFears = 3;
    w.historyVersion = 2;
    return swaf::encode(w);
}

} // namespace

SavedFamily saveNewFamily(Assets& assets, const OutfitCatalog& catalog, const fs::path& folder, const std::string& familyName,
                          const std::vector<NewFamilySim>& sims, std::vector<std::string>& problems) {
    if (sims.empty()) {
        fail("a family needs a Sim");
    }
    const fs::path hoodFile = content::neighborhoodPackage(folder);
    const std::string hood = folder.filename().string();
    dbpf::Package package{hoodFile};

    // What the neighborhood holds: its families, Sims (their versions and GUIDs), the neighborhood
    // record, the list of neighbor ids and the next free one.
    std::uint32_t lastFamily = 0, familyVersion = 0x4E;
    std::int32_t lastFamilyNumber = 0;
    std::set<std::uint32_t> guids;
    std::int32_t lastNeighbor = 0;
    sdsc::SimDescription newest;
    std::optional<std::vector<std::uint8_t>> neighborhood, neighborList, nextNeighbor;
    for (const auto& e : package.entries()) {
        const auto& k = e.key;
        if (k.type == kFamilyType && k.instance < kFirstPool) {
            const auto f = fami::parse(package.read(e));
            lastFamily = std::max(lastFamily, k.instance);
            familyVersion = std::max(familyVersion, f.version);
            lastFamilyNumber = std::max(lastFamilyNumber, f.value4);
        } else if (k.type == kDescription) {
            lastNeighbor = std::max<std::int32_t>(lastNeighbor, static_cast<std::int32_t>(k.instance));
            try {
                auto s = sdsc::parse(package.read(e));
                guids.insert(s.guid);
                if (s.version > newest.version) {
                    newest = std::move(s);
                }
            } catch (const std::exception&) {
                // Older descriptions the reader doesn't take don't set the version written.
            }
        } else if (k.type == kNeighborhood && k.instance == 1) {
            neighborhood = package.read(e);
        } else if (k.type == kNeighborList && k.instance == 0) {
            neighborList = package.read(e);
        } else if (k.type == kNextNeighbor && k.instance == 0) {
            nextNeighbor = package.read(e);
        }
    }
    if (!neighborhood || newest.version == 0) {
        fail("the neighborhood has no neighborhood record or Sims to go by");
    }

    // The family's number, the Sims' neighbor ids, GUIDs and character files.
    SavedFamily saved;
    saved.family = lastFamily + 1;
    if (saved.family >= kFirstPool) {
        fail("the neighborhood has no free family number");
    }
    std::int32_t next = lastNeighbor + 1;
    if (nextNeighbor && nextNeighbor->size() >= 14) {
        next = std::max<std::int32_t>(next, (*nextNeighbor)[12] | (*nextNeighbor)[13] << 8);
    }
    if (next + static_cast<std::int32_t>(sims.size()) > 0x7FFF) {
        fail("the neighborhood has no free neighbor ids");
    }
    std::uint32_t lastFile = 0;
    std::error_code ec;
    const fs::path characters = folder / "Characters";
    const std::string prefix = hood + "_User";
    for (const auto& entry : fs::directory_iterator(characters, ec)) {
        const std::string name = entry.path().filename().string();
        if (name.starts_with(prefix) && entry.path().extension() == ".package") {
            lastFile = std::max(lastFile, static_cast<std::uint32_t>(std::strtoul(name.c_str() + prefix.size(), nullptr, 10)));
        }
    }
    std::random_device device;
    std::mt19937 random(device());
    for (std::size_t i = 0; i < sims.size(); ++i) {
        saved.neighbors.push_back(static_cast<std::int16_t>(next + static_cast<std::int32_t>(i)));
        std::uint32_t guid = 0;
        do {
            guid = random() | 0x04000000u;
        } while (guids.contains(guid));
        guids.insert(guid);
        saved.guids.push_back(guid);
        char file[64];
        std::snprintf(file, sizeof file, "%s%05u.package", prefix.c_str(), lastFile + 1 + static_cast<std::uint32_t>(i));
        saved.characterFiles.push_back(characters / file);
    }

    // The records.
    std::vector<content::ReplacedResource> changes;
    const std::size_t values = sdsc::personDataCount(newest.personDataVersion);
    for (std::size_t i = 0; i < sims.size(); ++i) {
        const auto& sim = sims[i];
        sdsc::SimDescription d;
        d.version = newest.version;
        d.personDataVersion = newest.personDataVersion;
        d.personDataFlag = newest.personDataFlag;
        d.personData.assign(values, 0);
        auto set = [&](std::size_t index, int value) {
            if (index < d.personData.size()) {
                d.personData[index] = static_cast<std::int16_t>(value);
            }
        };
        const int neat = sim.traits[0] * 100, outgoing = sim.traits[1] * 100, active = sim.traits[2] * 100, playful = sim.traits[3] * 100,
                  nice = sim.traits[4] * 100;
        set(kNeat, neat);
        set(kOutgoing, outgoing);
        set(kActive, active);
        set(kPlayful, playful);
        set(kNice, nice);
        set(kBornNeat, neat);
        set(kBornOutgoing, outgoing);
        set(kBornActive, active);
        set(kBornPlayful, playful);
        set(kBornNice, nice);
        set(kAspiration, aspirationFlag(sim.aspiration, sim.age));
        set(kAge, ageCode(sim.age));
        set(kFamilyNumber, static_cast<int>(saved.family));
        set(kGender, sim.gender == 1 ? 1 : 0);
        set(kZodiac, sim.zodiac);
        set(kFatness, 500);
        set(kVoice, static_cast<std::int16_t>(random()));
        set(kSelection, 1);
        set(kAspirationScore, 20);
        set(kAspirationRaw, 216);
        if (sim.zodiac >= 1 && sim.zodiac <= 12) {
            set(kZodiacPersonality, kSignValues[static_cast<std::size_t>(sim.zodiac - 1)]);
        }
        set(kHobby, 204 + static_cast<int>(random() % 10));
        d.neighborId = saved.neighbors[i];
        d.guid = saved.guids[i];
        // The Sims it knows: the family's others.
        BinaryWriter rest;
        rest.u32(3);
        rest.u32(static_cast<std::uint32_t>(sims.size() - 1));
        for (std::size_t j = 0; j < sims.size(); ++j) {
            if (j != i) {
                rest.u32(static_cast<std::uint32_t>(saved.neighbors[j]));
            }
        }
        rest.bytes(std::vector<std::uint8_t>(9, 0));
        d.rest = rest.take();
        const auto id = static_cast<std::uint32_t>(saved.neighbors[i]);
        changes.push_back({{kDescription, kLocal, id, 0}, sdsc::encode(d)});
        changes.push_back({{kDna, kLocal, id, 0}, dna(sim)});
        changes.push_back({{kWants, kLocal, id, 0}, noWants()});
    }

    // The family's record and name.
    fami::Family family;
    family.magic = kFamilyType;
    family.version = familyVersion;
    family.value4 = lastFamilyNumber + 1;
    family.funds = kFunds + kFundsPerSim * static_cast<std::int32_t>(sims.size() - 1);
    family.funds2 = family.funds;
    family.value8 = 1;
    family.members = saved.guids;
    changes.push_back({{kFamilyType, kLocal, saved.family, 0}, fami::encode(family)});
    str::StringTable name;
    name.format = 0xFFFD;
    name.entries = {{1, familyName, ""}, {1, "", ""}};
    changes.push_back({{kStringTable, kLocal, saved.family, 0}, str::encode(name)});

    // The neighborhood record: the Sims' and the family's inventories.
    auto raw = ngbh::readRaw(*neighborhood);
    const std::uint32_t inventoryVersion = raw.neighbors.empty() ? kInventoryVersion : raw.neighbors.back().second.version;
    for (const auto id : saved.neighbors) {
        ngbh::Inventory inventory;
        inventory.version = inventoryVersion;
        for (const std::uint32_t token : kNewSimTokens) {
            xobj::Token t;
            t.guid = token;
            inventory.singular.push_back(t);
        }
        raw.neighbors.emplace_back(id, std::move(inventory));
    }
    ngbh::Inventory familyInventory;
    familyInventory.version = inventoryVersion;
    raw.families.emplace_back(static_cast<std::int32_t>(saved.family), std::move(familyInventory));
    changes.push_back({{kNeighborhood, kLocal, 1, 0}, ngbh::encode(raw)});

    // The list of neighbor ids and the next free one.
    if (neighborList && neighborList->size() >= 8) {
        BinaryReader r(*neighborList);
        const std::uint32_t version = r.u32();
        const std::uint32_t count = r.u32();
        std::vector<std::uint32_t> ids;
        for (std::uint32_t i = 0; i < count && r.remaining() >= 4; ++i) {
            ids.push_back(r.u32());
        }
        const auto rest = r.bytes(r.remaining());
        BinaryWriter w;
        w.u32(version);
        w.u32(static_cast<std::uint32_t>(ids.size() + sims.size()));
        for (const auto id : ids) {
            w.u32(id);
        }
        for (const auto id : saved.neighbors) {
            w.u32(static_cast<std::uint32_t>(id));
        }
        w.bytes(rest);
        changes.push_back({{kNeighborList, kLocal, 0, 0}, w.take()});
    } else {
        problems.push_back("the neighborhood has no list of neighbor ids to add the Sims to");
    }
    if (nextNeighbor && nextNeighbor->size() >= 14) {
        auto bytes = *nextNeighbor;
        const auto after = static_cast<std::uint16_t>(next + static_cast<std::int32_t>(sims.size()));
        bytes[12] = static_cast<std::uint8_t>(after);
        bytes[13] = static_cast<std::uint8_t>(after >> 8);
        changes.push_back({{kNextNeighbor, kLocal, 0, 0}, std::move(bytes)});
    } else {
        problems.push_back("the neighborhood has no next neighbor id to move on");
    }
    package.close();  // the file is let go before it is written again

    // The character files, then the neighborhood. A failure takes the files back out.
    std::vector<fs::path> written;
    try {
        for (std::size_t i = 0; i < sims.size(); ++i) {
            const auto& sim = sims[i];
            const CharacterFileSim file{saved.characterFiles[i], saved.guids[i], sim.firstName, familyName, sim.bio};
            const auto resources = characterResources(assets, catalog, sim.made, file, problems);
            if (fs::exists(file.file)) {
                fail("a character file is in the way: " + file.file.filename().string());
            }
            writeCharacterFile(file.file, resources);
            written.push_back(file.file);
        }
        saved.backup = content::rewritePackage(hoodFile, changes).backup;
    } catch (...) {
        for (const auto& f : written) {
            fs::remove(f, ec);
        }
        throw;
    }
    return saved;
}

void saveFamilyPicture(const fs::path& folder, std::uint32_t family, const std::vector<std::uint8_t>& jpeg) {
    constexpr std::uint32_t kFamilyPicture = 0x8C3CE95A;
    const fs::path file = folder / "Thumbnails" / (folder.filename().string() + "_FamilyThumbnails.package");
    const content::ReplacedResource picture{{kFamilyPicture, kLocal, family, 0}, jpeg};
    if (fs::exists(file)) {
        content::rewritePackage(file, std::span(&picture, 1));
        return;
    }
    fs::create_directories(file.parent_path());
    auto temporary = file;
    temporary += ".neots2-new";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        dbpf::PackageWriter writer(out);
        writer.add(picture.key, picture.bytes, false, static_cast<std::uint32_t>(picture.bytes.size()));
        writer.finish(0x00010000);
        if (!out) {
            fail("couldn't write the family's picture");
        }
    }
    fs::rename(temporary, file);
}

} // namespace ts2::scene
