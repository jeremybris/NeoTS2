#pragma once

#include "scene/age_data.hpp"
#include "scene/assets.hpp"
#include "scene/outfit_catalog.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ts2::scene {

// A Sim of a new family, as Create-a-Sim made it: its look, names, age and gender flags (adult 0x08,
// female 1, male 2), personality points (0 to 10: neat, outgoing, active, playful, nice), zodiac
// sign (1 Aries to 12 Pisces) and aspiration (Create-a-Sim's: 1 romance, 2 family, 3 fortune,
// 5 popularity, 6 knowledge, 7 grow up, 8 pleasure; 0 none).
struct NewFamilySim {
    MadeLook made;
    std::string firstName, bio;
    std::uint32_t age = 0x08, gender = 1;
    std::array<int, 5> traits{};
    int zodiac = 1;
    int aspiration = 0;
};

struct SavedFamily {
    std::uint32_t family = 0;                          // the family's number (its record's instance)
    std::vector<std::int16_t> neighbors;               // each Sim's neighbor id
    std::vector<std::uint32_t> guids;                  // each Sim's GUID
    std::vector<std::filesystem::path> characterFiles; // each Sim's character file
    std::filesystem::path backup;                      // the neighborhood as the game last wrote it
};

// Saves a family made in Create-a-Sim into the neighborhood in `folder` (Neighborhoods/<hood>), as
// the game saves one accepted from Create-a-Sim, waiting in the Families bin (no lot):
// - each Sim's character file (Characters/<hood>_User<NNNNN>.package, the next free numbers);
// - in the neighborhood package: each Sim's description (its person data as Create-a-Sim sets it,
//   the other members it knows), DNA, and wants and fears (none yet: the game draws them); the
//   family's record (its members, funds) and name; the neighborhood record's inventories of the
//   Sims and the family; the list of neighbor ids and the next free one.
// The members are roommates: no family ties or relationships. The family's picture is saved apart
// (saveFamilyPicture). Not yet: its story album, relationships chosen in Family Relationships.
// The neighborhood package is written through content::rewritePackage (checked, kept as
// ".neots2bak" before the first rewrite). Character files are written first; if the neighborhood
// can't be written, they are removed again. Throws on failure.
SavedFamily saveNewFamily(Assets& assets, const OutfitCatalog& catalog, const std::filesystem::path& folder, const std::string& familyName,
                          const std::vector<NewFamilySim>& sims, std::vector<std::string>& problems);

// Puts a family's picture (a JPEG, see jpeg::encodeWithAlpha) in the neighborhood's family
// pictures (Thumbnails/<hood>_FamilyThumbnails.package), as the Families bin shows it: rewritten as
// content::rewritePackage does, or made when the neighborhood has none yet.
void saveFamilyPicture(const std::filesystem::path& folder, std::uint32_t family, const std::vector<std::uint8_t>& jpeg);

} // namespace ts2::scene
