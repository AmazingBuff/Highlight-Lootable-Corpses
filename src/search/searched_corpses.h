//
// Created by AmazingBuff on 2026/09/09.
//

#pragma once

PLUGIN_NAMESPACE_BEGIN

class MarkCorpse
{
public:
    MarkCorpse(MarkCorpse const&) = delete;
    MarkCorpse(MarkCorpse const&&) = delete;
    MarkCorpse operator=(MarkCorpse&) = delete;
    MarkCorpse operator=(MarkCorpse&&) = delete;

    static MarkCorpse& instance();

    // all input must be a corpse with container
    void mark(RE::TESObjectREFR* a_ref);
    // all input must be a corpse with container
    [[nodiscard]] bool contains(RE::TESObjectREFR* a_ref);
    void install();
private:
    MarkCorpse();
    ~MarkCorpse();
private:
    std::unordered_set<RE::FormID> m_searched_corpses;
};

PLUGIN_NAMESPACE_END
