//
// Created by AmazingBuff on 2026/09/09.
//

#pragma once

PLUGIN_NAMESPACE_BEGIN

class MarkCorpse
{
public:
    MarkCorpse() = delete;
    ~MarkCorpse() = delete;
    MarkCorpse(MarkCorpse const&) = delete;
    MarkCorpse(MarkCorpse const&&) = delete;
    MarkCorpse operator=(MarkCorpse&) = delete;
    MarkCorpse operator=(MarkCorpse&&) = delete;

    // all input must be a corpse with container
    static void mark(RE::TESObjectREFR* a_ref);
    // all input must be a corpse with container
    [[nodiscard]] static bool contains(RE::TESObjectREFR* a_ref);
    static void install();
};

PLUGIN_NAMESPACE_END
