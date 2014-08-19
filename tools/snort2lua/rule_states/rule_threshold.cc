/*
** Copyright (C) 2014 Cisco and/or its affiliates. All rights reserved.
 * Copyright (C) 2002-2013 Sourcefire, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License Version 2 as
 * published by the Free Software Foundation.  You may not use, modify or
 * distribute this program under any other version of the GNU General
 * Public License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */
// rule_threshold.cc author Josh Rosenbaum <jrosenba@cisco.com>

#include <sstream>
#include <vector>

#include "conversion_state.h"
#include "utils/converter.h"
#include "rule_states/rule_api.h"
#include "utils/s2l_util.h"

namespace rules
{

namespace {


class Threshold : public ConversionState
{
public:
    Threshold(Converter* cv, LuaData* ld) : ConversionState(cv, ld) {};
    virtual ~Threshold() {};
    virtual bool convert(std::istringstream& data);
};

} // namespace

bool Threshold::convert(std::istringstream& data_stream)
{
    std::string args;
    std::string value;
    bool retval = true;

    args = util::get_rule_option_args(data_stream);
    std::istringstream arg_stream(args);


    ld->open_table("event_filter");
    ld->add_diff_option_comment("ips_option: threshold", "event_filter");
    ld->open_table();

    while (util::get_string(arg_stream, value, ","))
    {
        std::string keyword;
        std::string val;
        bool tmpval = true;
        std::istringstream subopt_stream(value);

        if (!(subopt_stream >> keyword) || !(subopt_stream >> val))
            tmpval = false;

        else if (!(keyword.compare("count")))
            tmpval = ld->add_option_to_table("count", std::stoi(val));

        else if (!(keyword.compare("seconds")))
            tmpval = ld->add_option_to_table("seconds", std::stoi(val));

        else if (!(keyword.compare("type")))
            tmpval = ld->add_option_to_table("type", val);

        else if (!(keyword.compare("track")))
            tmpval = ld->add_option_to_table("track", val);

        else
            retval = false;

        if (retval && !tmpval)
            retval = false;
    }

    // save the current position
    const std::streamoff curr_pos = data_stream.tellg();

    if (curr_pos == -1)
        data_stream.clear();

    bool found_gid = false, found_sid = false;
    std::string rule_keyword;

    data_stream.seekg(0);
    std::getline(data_stream, rule_keyword, '(');
    std::streamoff tmp_pos = data_stream.tellg();

    while(std::getline(data_stream, rule_keyword, ':'))
    {
        std::size_t semi_colon_pos = rule_keyword.find(';');
        if (semi_colon_pos != std::string::npos)
        {
            // found an option without a colon, so set stream
            // to semi-colon
            std::streamoff off = 1 + (std::streamoff)(tmp_pos) +
                                 (std::streamoff)(semi_colon_pos);
            data_stream.seekg(off);
            rule_keyword = rule_keyword.substr(0, semi_colon_pos);
        }

        // now, lets get the next option.
        util::trim(rule_keyword);

        if (!rule_keyword.compare("sid"))
        {
            std::string val = util::get_rule_option_args(data_stream);
            ld->add_option_to_table("sid", std::stoi(val));
            found_sid = true;
        }
        else if (!rule_keyword.compare("gid"))
        {
            std::string val = util::get_rule_option_args(data_stream);
            ld->add_option_to_table("gid", std::stoi(val));
            found_gid = true;
        }
        else  if (semi_colon_pos == std::string::npos)
            std::getline(data_stream, rule_keyword, ';');

        // short circuit in case we already found the gid/sid
        if (found_gid && found_sid)
            break;

        tmp_pos = data_stream.tellg();
    }


    ld->close_table();
    ld->close_table();
    if (curr_pos != -1)
        data_stream.clear();

    data_stream.seekg(curr_pos);
    return set_next_rule_state(data_stream) && retval;
}

/**************************
 *******  A P I ***********
 **************************/


static ConversionState* ctor(Converter* cv, LuaData* ld)
{
    return new Threshold(cv, ld);
}

static const ConvertMap rule_threshold =
{
    "threshold",
    ctor,
};

const ConvertMap* threshold_map = &rule_threshold;

} // namespace rules