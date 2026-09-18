#pragma once

PLUGIN_NAMESPACE_BEGIN

class Menu
{
public:
    Menu() = delete;
    ~Menu() = delete;
    Menu(Menu const&) = delete;
    Menu(Menu const&&) = delete;
    Menu operator=(Menu&) = delete;
    Menu operator=(Menu&&) = delete;

    static void register_menu();
    [[nodiscard]] static bool is_menu_open();
};

PLUGIN_NAMESPACE_END