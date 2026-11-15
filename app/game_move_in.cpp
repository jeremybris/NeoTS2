// Moving a family of the Families bin into a house, as the game's Move In does: its picture clicked
// picks it up, a lot clicked asks, and Yes gives it the lot (scene::moveFamilyIn) and opens the lot,
// where its Sims arrive (sim::LotSimulation). NeoTS2 doesn't work out a house's price yet, so the
// family's funds are kept.
#include "game.hpp"

#include "diag/log.hpp"
#include "scene/family_save.hpp"

#include <algorithm>
#include <format>

namespace app {

namespace {

constexpr std::uint32_t kUiText = 0x7FA7DC7C, kYesText = 0x008E0003, kNoText = 0x008E0004, kOkText = 0x008E0001;  // string ids, leakcheck: ok
constexpr std::uint32_t kMoveInYes = 1, kMoveInNo = 2, kMoveInOk = 3;

} // namespace

void Game::askMoveIn(const NeighborhoodUi::BinFamily& family, std::uint32_t lot) {
    const NeighborhoodUi::LotCard* card = lot != 0 ? hoodUi_.lotCard(lot) : nullptr;
    const auto& lived = hoodUi_.occupiedLots();
    std::string why;
    if (card == nullptr || card->community) {
        why = "Choose a house in the neighborhood to move the " + family.name + " family into.";
    } else if (std::find(lived.begin(), lived.end(), lot) != lived.end()) {
        why = "Another household lives there. Choose a house nobody lives in.";
    }
    if (!why.empty()) {
        moveInBox_.open(screen_, uiWidth_, uiHeight_, "Move In", why, {{kMoveInOk, data_.text(kUiText, kOkText)}});
        return;
    }
    moveInFamily_ = family.id;
    moveInLot_ = lot;
    const std::string text = std::format("Move the {} family into {}?\n\nNeoTS2 doesn't work out a house's price yet, so the family keeps its funds.",
                                         family.name, card->name);
    moveInBox_.open(screen_, uiWidth_, uiHeight_, "Move In", text,
                    {{kMoveInYes, data_.text(kUiText, kYesText)}, {kMoveInNo, data_.text(kUiText, kNoText)}});
}

bool Game::moveInClicked(int window) {
    const auto answer = moveInBox_.clicked(screen_, window);
    if (!answer) {
        return false;
    }
    moveInBox_.close(screen_);
    if (*answer != kMoveInYes) {
        return true;
    }
    hoodUi_.dropFamily();
    hoodUi_.hideCard();
    try {
        ts2::scene::moveFamilyIn(openFolder_, moveInFamily_, moveInLot_, 0);
    } catch (const std::exception& ex) {
        ts2::diag::warning(std::string("moving in: ") + ex.what());
        moveInBox_.open(screen_, uiWidth_, uiHeight_, "Move In", std::string("The family couldn't move in: ") + ex.what(),
                        {{kMoveInOk, data_.text(kUiText, kOkText)}});
        return true;
    }
    ts2::diag::step(std::format("moving in: family {} moved into lot {}", moveInFamily_, moveInLot_));
    hoodUi_.refreshFamilies(openFolder_);
    enterLot(moveInLot_);
    return true;
}

} // namespace app
