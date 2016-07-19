// Copyright (c) 2015 Martin Ridgers
// License: http://opensource.org/licenses/MIT

#pragma once

#include "host.h"

#include <core/singleton.h>

#include <Windows.h>

//------------------------------------------------------------------------------
class host_ps
    : public host
    , public singleton<host_ps>
{
public:
                        host_ps();
                        ~host_ps();
    bool                validate() override;
    bool                initialise() override;
    void                shutdown() override;

private:
    static BOOL WINAPI  read_console(HANDLE input, wchar_t* buffer, DWORD buffer_count, LPDWORD read_in, void* control);
    void                edit_line(const wchar_t* prompt, wchar_t* buffer, int buffer_count);
    void                initialise_lua(lua_state& lua);
    virtual void        initialise_editor_desc(line_editor::desc& desc) override;
};
