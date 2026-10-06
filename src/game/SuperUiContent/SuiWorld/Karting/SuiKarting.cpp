/*
 * WoW Karting - see SuiKarting.h and MSUIClient shared_docs/WOW_KARTING.md.
 */

#include "SuiKarting.h"

#include "SpellAuras.h"
#include "SpellMgr.h"
#include "Unit.h"

bool SuiKarting::IsPackSpell(uint32 spellId)
{
    return spellId >= PACK_SPELL_FIRST && spellId < PACK_SPELL_END;
}

bool SuiKarting::IsKartSpell(SpellEntry const* spell)
{
    if (!spell || !IsPackSpell(spell->Id))
        return false;

    bool mounts = false;
    bool marked = false;
    for (uint8 effect = 0; effect < MAX_EFFECT_INDEX; ++effect)
    {
        if (spell->Effect[effect] != SPELL_EFFECT_APPLY_AURA)
            continue;
        if (spell->EffectApplyAuraName[effect] == SPELL_AURA_MOUNTED)
            mounts = true;
        else if (spell->EffectApplyAuraName[effect] == SPELL_AURA_DUMMY &&
                 spell->EffectMiscValue[effect] == KART_MARKER)
            marked = true;
    }
    return mounts && marked;
}

bool SuiKarting::IsNeutralKartRider(Unit const* unit)
{
    // Hot path (every creature line-of-sight check): the mount test rejects almost everyone.
    if (!unit || !unit->IsMounted())
        return false;

    for (Aura const* aura : unit->GetAurasByType(SPELL_AURA_DUMMY))
    {
        if (aura->GetModifier()->m_miscvalue == KART_MARKER && IsPackSpell(aura->GetId()))
            return true;
    }
    return false;
}
