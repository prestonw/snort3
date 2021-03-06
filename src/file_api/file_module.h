//--------------------------------------------------------------------------
// Copyright (C) 2014-2016 Cisco and/or its affiliates. All rights reserved.
// Copyright (C) 2012-2013 Sourcefire, Inc.
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of the GNU General Public License Version 2 as published
// by the Free Software Foundation.  You may not use, modify or distribute
// this program under any other version of the GNU General Public License.
//
// This program is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
//--------------------------------------------------------------------------

// file_module.h author Hui Cao <huica@cisco.com>

#ifndef FILE_MODULE_H
#define FILE_MODULE_H

#include "framework/module.h"

#include "file_config.h"
#include "file_identifier.h"
#include "file_policy.h"

//-------------------------------------------------------------------------
// file_id module
//-------------------------------------------------------------------------

class FileIdModule : public Module
{
public:
    FileIdModule();

    bool set(const char*, Value&, SnortConfig*) override;
    bool begin(const char*, int, SnortConfig*) override;
    bool end(const char*, int, SnortConfig*) override;

    const PegInfo* get_pegs() const override;
    PegCount* get_counts() const override;

    void sum_stats() override;

    FileConfig fc;

private:
    FileMagicRule rule;
    FileMagicData magic;
    FileRule file_rule;
};

#endif

