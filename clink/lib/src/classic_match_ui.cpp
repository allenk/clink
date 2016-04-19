// Copyright (c) 2016 Martin Ridgers
// License: http://opensource.org/licenses/MIT

#include "pch.h"
#include "classic_match_ui.h"

#include <core/base.h>
#include <core/settings.h>
#include <lib/binder.h>
#include <lib/line_buffer.h>
#include <lib/editor_backend.h>
#include <line_state.h>
#include <matches/matches.h>
#include <terminal/terminal.h>

//------------------------------------------------------------------------------
static setting_int g_query_threshold(
    "match.query_threshold",
    "Ask if matches > threshold",
    "", // MODE4
    100);

static setting_int g_max_width(
    "match.max_width",
    "Maximum display width",
    "", // MODE4
    106);

static setting_bool g_vertical(
    "match.vertical",
    "Display matches vertically",
    "", // MODE4
    true);



//------------------------------------------------------------------------------
void classic_match_ui::bind(binder& binder)
{
    binder.bind("\t", *this, state_none);
}

//------------------------------------------------------------------------------
void classic_match_ui::begin_line(const char* prompt, const context& context)
{
}

//------------------------------------------------------------------------------
void classic_match_ui::end_line()
{
}

//------------------------------------------------------------------------------
void classic_match_ui::on_matches_changed(const context& context)
{
    m_waiting = false;
}

//------------------------------------------------------------------------------
editor_backend::result classic_match_ui::on_input(
    const char* keys,
    int id,
    const context& context)
{
    auto& terminal = context.terminal;
    auto& matches = context.matches;

    if (matches.get_match_count() == 0)
        return result::next;

    if (m_waiting)
    {
        int next_state = state_none;
        switch (id)
        {
        case state_none:    next_state = begin_print(context); break;
        case state_query:   next_state = query_prompt(keys[0], context); break;
        case state_pager:   next_state = pager_prompt(keys[0], context); break;
        }

        if (next_state > state_print)
            next_state = print(context, next_state == state_print_one);

        switch (next_state)
        {
        case state_query:   return { result::more_input, state_query };
        case state_pager:   return { result::more_input, state_pager };
        }

        return result::redraw;
    }

    // One match? Accept it.
    if (matches.get_match_count() == 1)
        return { result::accept_match, 0 };

    // Valid LCD? Append it.
    str<> lcd;
    matches.get_match_lcd(lcd);
    if (int lcd_length = lcd.length())
    {
        const word end_word = *(context.line.get_words().back());

        line_buffer& buffer = context.buffer;
        unsigned int cursor = buffer.get_cursor();

        int word_end = end_word.offset + end_word.length;
        int dx = lcd_length - (cursor - word_end);

        if (dx < 0)
        {
            buffer.remove(cursor + dx, cursor);
            buffer.set_cursor(cursor + dx);
        }
        else if (dx > 0)
            buffer.insert(lcd.c_str() + lcd_length - dx);
        else if (!dx)
            m_waiting = true;

        return result::next;
    }

    m_waiting = true;
    return result::next;
}

//------------------------------------------------------------------------------
classic_match_ui::state classic_match_ui::begin_print(const context& context)
{
    const matches& matches = context.matches;
    int match_count = matches.get_match_count();

    m_longest = 0;
    m_row = 0;

    // Get the longest match length.
    for (int i = 0, n = matches.get_match_count(); i < n; ++i)
    {
        const char* match = matches.get_match(i);
        m_longest = max<int>(char_count(match), m_longest);
    }

    if (!m_longest)
        return state_none;

    context.terminal.write("\n", 1);

    int query_threshold = g_query_threshold.get();
    if (query_threshold > 0 && query_threshold <= match_count)
    {
        str<64> prompt;
        prompt.format("Show %d matches? [Yn]", match_count);
        context.terminal.write(prompt.c_str(), -1);
        context.terminal.flush();

        return state_query;
    }

    return state_print_page;
}

//------------------------------------------------------------------------------
classic_match_ui::state classic_match_ui::print(const context& context, bool single_row)
{
    terminal& term = context.terminal;
    const matches& matches = context.matches;

    auto_flush flusher(term);
    term.write("\r", 1);

    int match_count = matches.get_match_count();

    int columns = max(1, g_max_width.get() / m_longest);
    int total_rows = (match_count + columns - 1) / columns;

    bool vertical = g_vertical.get();
    int dx = vertical ? total_rows : 1;

    int max_rows = single_row ? 1 : (total_rows - m_row - 1);
    max_rows = min(term.get_rows() - 1 - !!m_row, max_rows);
    for (; max_rows >= 0; --max_rows, ++m_row)
    {
        int index = vertical ? m_row : (m_row * columns);
        for (int x = 0; x < columns; ++x)
        {
            if (index >= match_count)
                continue;

            const char* match = matches.get_match(index);
            term.write(match, int(strlen(match)));

            for (int i = m_longest - char_count(match) + 1; i >= 0;)
            {
                const char spaces[] = "                ";
                term.write(spaces, min<int>(sizeof_array(spaces) - 1, i));
                i -= sizeof_array(spaces) - 1;
            }

            index += dx;
        }

        term.write("\n", 1);
    }

    if (m_row == total_rows)
        return state_none;

    static const char prompt[] = { "--More--" };
    term.write(prompt, sizeof_array(prompt) - 1);
    return state_pager;
}

//------------------------------------------------------------------------------
classic_match_ui::state classic_match_ui::query_prompt(
    unsigned char key,
    const context& context)
{
    switch(key)
    {
    case 'y':
    case 'Y':
    case ' ':
    case '\t':
    case '\r':
        return state_print_page;

    case 'n':
    case 'N':
    case 0x03: // ctrl-c
    case 0x04: // ctrl-d
    case 0x1b: // esc
        context.terminal.write("\n", 1);
        return state_none;
    }

    context.terminal.write("\x07", 1);
    return state_query;
}

//------------------------------------------------------------------------------
classic_match_ui::state classic_match_ui::pager_prompt(
    unsigned char key,
    const context& context)
{
    switch (key)
    {
    case ' ':
    case '\t':
        return state_print_page;

    case '\r':
        return state_print_one;

    case 'q':
    case 'Q':
    case 0x03: // ctrl-c
    case 0x04: // ctrl-d
    case 0x1b: // esc
        context.terminal.write("\n", 1);
        return state_none;
    }

    context.terminal.write("\x07", 1);
    return state_pager;
}
