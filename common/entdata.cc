/*  Copyright (C) 1996-1997  Id Software, Inc.

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA

    See file, 'COPYING', for details.
*/

#include <common/entdata.h>

#include <sstream>

#include <common/bsputils.hh>
#include <common/parser.hh>

#include <fmt/ostream.h>

entdict_t::entdict_t(std::initializer_list<keyvalue_t> l) : keyvalues(l) { }

entdict_t::entdict_t() = default;

entdict_t::entdict_t(parser_base_t &parser) { parse(parser); }

std::string entdict_t::get(const std::string_view &key) const
{
    if (auto it = find(key); it != keyvalues.end()) {
        return it->second;
    }

    static std::string empty;
    return empty;
}

vec_t entdict_t::get_float(const std::string_view &key) const
{
    auto s = get(key);

    if (s.empty()) {
        return 0;
    }

    try {
        return std::stod(s);
    } catch (std::exception &) {
        return 0.0;
    }
}

int32_t entdict_t::get_int(const std::string_view &key) const
{
    auto s = get(key);

    if (s.empty()) {
        return 0;
    }

    try {
        return std::stoi(s);
    } catch (std::exception &) {
        return 0;
    }
}

int32_t entdict_t::get_vector(const std::string_view &key, qvec3d &vec) const
{
    std::string value = get(key);

    // FIXME: this fixes ASan triggering on some entities...
    if (*(value.data() + value.size()) != 0) {
        *(value.data() + value.size()) = 0;
    }

    vec = {};
    return sscanf(value.data(), "%lf %lf %lf", &vec[0], &vec[1], &vec[2]);
}

void entdict_t::set(const std::string_view &key, const std::string_view &value)
{
    // search for existing key to update
    if (auto it = find(key); it != keyvalues.end()) {
        // found existing key
        it->second = value;
        return;
    }

    // no existing key; add new
    keyvalues.emplace_back(key, value);
}

void entdict_t::remove(const std::string_view &key)
{
    if (auto it = find(key); it != keyvalues.end()) {
        keyvalues.erase(it);
    }
}

void entdict_t::rename(const std::string_view &from, const std::string_view &to)
{
    const auto it = find(from);
    if (it != end()) {
        auto oldValue = std::move(it->second);
        keyvalues.erase(it);
        keyvalues.emplace_back(to, std::move(oldValue));
    }
}

keyvalues_t::iterator entdict_t::find(const std::string_view &key)
{
    auto existingIt = keyvalues.end();
    for (auto it = keyvalues.begin(); it != keyvalues.end(); ++it) {
        if (key == it->first) {
            existingIt = it;
            break;
        }
    }
    return existingIt;
}

keyvalues_t::const_iterator entdict_t::find(const std::string_view &key) const
{
    auto existingIt = keyvalues.end();
    for (auto it = keyvalues.begin(); it != keyvalues.end(); ++it) {
        if (key == it->first) {
            existingIt = it;
            break;
        }
    }
    return existingIt;
}

bool entdict_t::has(const std::string_view &key) const
{
    return find(key) != end();
}

void entdict_t::parse(parser_base_t &parser)
{
    /* parse the opening brace */
    if (!parser.parse_token())
        return;
    if (parser.token != "{")
        FError("found {} when expecting {", parser.token);

    /* go through all the keys in this entity */
    while (1) {
        /* parse key */
        if (!parser.parse_token())
            FError("EOF without closing brace");

        if (parser.token == "}")
            break;

        std::string keystr = parser.token;

        /* parse value */
        if (!parser.parse_token())
            FError("EOF without closing brace");

        if (parser.token == "}")
            FError("closing brace without data");

        // trim whitespace from start/end
        while (std::isspace(keystr.front())) {
            keystr.erase(keystr.begin());
        }
        while (std::isspace(keystr.back())) {
            keystr.erase(keystr.cbegin());
        }

        set(keystr, parser.token);
    }
}

void EntData_ParseInto(parser_t &parser, std::vector<entdict_t> &vector)
{
    /* go through all the entities */
    while (1) {
        /* parse the opening brace */
        if (parser.at_end() || !parser.parse_token(PARSE_PEEK))
            break;

        // emplace a new entdict_t out of the parser
        vector.emplace_back(parser);
    }
}

std::vector<entdict_t> EntData_Parse(parser_t &parser)
{
    std::vector<entdict_t> result;

    EntData_ParseInto(parser, result);

    return result;
}

/*
 * ================
 * EntData_Write
 * ================
 */
std::string EntData_Write(const std::vector<entdict_t> &ents)
{
    std::string out;
    for (const auto &ent : ents) {
        out += "{\n";
        for (const auto &epair : ent) {
            fmt::format_to(std::back_inserter(out), "\"{}\" \"{}\"\n", epair.first, epair.second);
        }
        out += "}\n";
    }
    return out;
}
