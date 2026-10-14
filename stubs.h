#pragma once
#include <cstdint>

// Symbols defined in stubs.asm
extern "C" {
    extern uint8_t  g_rarityLegendary;
    extern uint64_t g_dropItem;
    extern uint64_t g_lootNemGear;
    extern uint64_t ret_rarity;
    extern uint64_t ret_intel;
    extern uint64_t ret_drop;
    void SowRarityStub();
    void SowIntelStub();
    void SowDropStub();
}
