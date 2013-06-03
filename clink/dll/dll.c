/* Copyright (c) 2012 Martin Ridgers
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "pch.h"
#include "inject_args.h"
#include "shell.h"
#include "shared/util.h"
#include "shared/shared_mem.h"

//------------------------------------------------------------------------------
typedef struct
{
    wchar_t*            buffer;
    int                 size;
} write_cache_t;

//------------------------------------------------------------------------------
void                    save_history();
void                    shutdown_lua();
void                    clear_to_eol();
void                    emulate_doskey(wchar_t*, unsigned);
int                     call_readline_w(const wchar_t*, wchar_t*, unsigned);
void                    shutdown_clink_settings();
int                     get_clink_setting_int(const char*);
void                    prepare_env_for_inputrc();
int                     check_auto_answer(const wchar_t*);

inject_args_t           g_inject_args;
static const shell_t*   g_shell                 = NULL;
static int              g_write_cache_index     = 0;
static const int        g_write_cache_size      = 0xffff;      // 0x10000 - 1 !!
static write_cache_t    g_write_cache[2]        = { {NULL, 0},
                                                    {NULL, 0} };
extern shell_t          shell_cmd;
extern shell_t          shell_generic;

//------------------------------------------------------------------------------
static LONG WINAPI exception_filter(EXCEPTION_POINTERS* info)
{
#if defined(_MSC_VER) && defined(CLINK_USE_SEH)
    MINIDUMP_EXCEPTION_INFORMATION mdei = { GetCurrentThreadId(), info, FALSE };
    DWORD pid;
    HANDLE process;
    HANDLE file;
    char file_name[1024];

    get_config_dir(file_name, sizeof(file_name));
    str_cat(file_name, "/mini_dump.dmp", sizeof(file_name));

    fputs("\n!!! CLINK'S CRASHED!", stderr);
    fputs("\n!!! Something went wrong.", stderr);
    fputs("\n!!! Writing mini dump file to: ", stderr);
    fputs(file_name, stderr);
    fputs("\n", stderr);

    file = CreateFile(file_name, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (file != INVALID_HANDLE_VALUE)
    {
        pid = GetCurrentProcessId();
        process = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
        if (process != NULL)
        {
            MiniDumpWriteDump(process, pid, file, MiniDumpNormal, &mdei, NULL, NULL);
        }
        CloseHandle(process);
    }
    CloseHandle(file);
#endif // _MSC_VER

    // Would be awesome if we could unhook ourself, unload, and allow cmd.exe
    // to continue!

    return EXCEPTION_EXECUTE_HANDLER;
}

//------------------------------------------------------------------------------
static void invalidate_cached_write(int index)
{
    write_cache_t* cache;

    // Check bounds.
    if ((unsigned)index >= sizeof_array(g_write_cache))
    {
        return;
    }

    cache = g_write_cache + index;

    cache->size = 0;
    if (cache->buffer != NULL)
    {
        cache->buffer[0] = L'\0';
    }
}

//------------------------------------------------------------------------------
static void dispatch_cached_write(HANDLE output, int index)
{
    write_cache_t* cache;

    // Check bounds.
    if ((unsigned)index >= sizeof_array(g_write_cache))
    {
        return;
    }

    cache = g_write_cache + index;

    // Write the line to the console.
    if (cache->buffer != NULL)
    {
        DWORD j;
        WriteConsoleW(output, cache->buffer, cache->size, &j, NULL);
    }

    invalidate_cached_write(index);
}

//------------------------------------------------------------------------------
BOOL WINAPI hooked_read_console_input(
    HANDLE input,
    INPUT_RECORD* buffer,
    DWORD buffer_size,
    LPDWORD events_read
)
{
    int i;

    i = (g_write_cache_index + 1) & 1;
    dispatch_cached_write(GetStdHandle(STD_OUTPUT_HANDLE), i);

    return ReadConsoleInputA(input, buffer, buffer_size, events_read);
}

//------------------------------------------------------------------------------
static BOOL WINAPI handle_single_byte_read(
    HANDLE input,
    wchar_t* buffer,
    DWORD buffer_size,
    LPDWORD read_in,
    PCONSOLE_READCONSOLE_CONTROL control
)
{
    int i;
    int reply;
    write_cache_t* write_cache;

    i = (g_write_cache_index + 1) & 1;
    write_cache = g_write_cache + i;

    if (reply = check_auto_answer(write_cache->buffer))
    {
        // cmd.exe's PromptUser() method reads a character at a time until
        // it encounters a \n. The way Clink handle's this is a bit 'hacky'.
        static int visit_count = 0;

        ++visit_count;
        if (visit_count >= 2)
        {
            invalidate_cached_write(i);

            reply = '\n';
            visit_count = 0;
        }

        *buffer = reply;
        *read_in = 1;
        return TRUE;
    }

    // Default behaviour.
    dispatch_cached_write(GetStdHandle(STD_OUTPUT_HANDLE), i);
    return ReadConsoleW(input, buffer, buffer_size, read_in, control);
}

//------------------------------------------------------------------------------
static void append_crlf(wchar_t* buffer, DWORD max_size)
{
    // Cmd.exe expects a CRLF combo at the end of the string, otherwise it
    // thinks the line is part of a multi-line command.

    size_t len;

    len = max_size - wcslen(buffer);
    wcsncat(buffer, L"\x0d\x0a", len);
    buffer[max_size - 1] = L'\0';
}

//------------------------------------------------------------------------------
BOOL WINAPI hooked_read_console(
    HANDLE input,
    wchar_t* buffer,
    DWORD buffer_size,
    LPDWORD read_in,
    PCONSOLE_READCONSOLE_CONTROL control
)
{
    const wchar_t* prompt;
    int is_eof;
    LPTOP_LEVEL_EXCEPTION_FILTER old_seh;
    int i;
    write_cache_t* write_cache;

    // If cmd.exe is asking for one character at a time, use the original path
    // It does this to handle y/n/all prompts which isn't an compatible use-
    // case for readline.
    if (buffer_size == 1)
    {
        return handle_single_byte_read(
            input,
            buffer,
            buffer_size,
            read_in,
            control
        );
    }

    // Get index to last cached write. This is our prompt.
    i = (g_write_cache_index + 1) & 1;
    write_cache = g_write_cache + i;

    old_seh = SetUnhandledExceptionFilter(exception_filter);

    // Call readline.
    is_eof = call_readline_w(write_cache->buffer, buffer, buffer_size);
    if (is_eof && get_clink_setting_int("ctrld_exits"))
    {
        wcsncpy(buffer, L"exit", buffer_size);
    }

    invalidate_cached_write(i);

    emulate_doskey(buffer, buffer_size);
    append_crlf(buffer, buffer_size);

    SetUnhandledExceptionFilter(old_seh);

    *read_in = (unsigned)wcslen(buffer);
    return TRUE;
}

//------------------------------------------------------------------------------
BOOL WINAPI hooked_write_console(
    HANDLE output,
    const wchar_t* buffer,
    DWORD buffer_size,
    LPDWORD written,
    void* unused
)
{
    // Writes to the console are double buffered. This stops custom prompts
    // from flickering.

    static int once = 0;
    int copy_size;
    int i;
    write_cache_t* cache;
    CONSOLE_SCREEN_BUFFER_INFO csbi;

    // First establish the next buffer to use and allocate it if need be.
    i = g_write_cache_index;
    g_write_cache_index = (i + 1) & 1;

    cache = g_write_cache + i;

    if (cache->buffer == NULL)
    {
        cache->buffer = VirtualAlloc(
            NULL,
            g_write_cache_size + 1,
            MEM_COMMIT,
            PAGE_READWRITE
        );
    }

    // Copy the write request into the buffer.
    copy_size = (g_write_cache_size < buffer_size)
        ? g_write_cache_size
        : buffer_size;

    cache->size = copy_size;
    memcpy(cache->buffer, buffer, copy_size * sizeof(wchar_t));
    cache->buffer[copy_size] = L'\0';

    // Dispatch previous write call.
    dispatch_cached_write(output, g_write_cache_index);

    if (written != NULL)
    {
        *written = buffer_size;
    }

    return TRUE;
}

//------------------------------------------------------------------------------
static void set_rl_readline_name()
{
    char buffer[MAX_PATH];

    if (GetModuleFileName(NULL, buffer, sizeof_array(buffer)))
    {
        static char exe_name[64];
        const char* slash;
        
        slash = strrchr(buffer, '\\');
        slash = slash ? slash + 1 : buffer;

        str_cpy(exe_name, slash, sizeof(exe_name));
        rl_readline_name = exe_name;

        LOG_INFO("Setting rl_readline_name to '%s'", exe_name);
    }
}

//------------------------------------------------------------------------------
static void get_inject_args(DWORD pid)
{
    shared_mem_t* shared_mem;
    shared_mem = open_shared_mem(1, "clink", pid);
    memcpy(&g_inject_args, shared_mem->ptr, sizeof(g_inject_args));
    close_shared_mem(shared_mem);
}

//------------------------------------------------------------------------------
static void success()
{
    extern const char* g_clink_header;
    extern const char* g_clink_footer;

    if (!g_inject_args.quiet)
    {
        puts(g_clink_header);
        puts("  ** Press Alt-H to show key bindings. **\n");
        puts(g_clink_footer);
    }
}

//------------------------------------------------------------------------------
static void failed()
{
    char buffer[1024];

    buffer[0] = '\0';
    get_config_dir(buffer, sizeof_array(buffer));

    fprintf(stderr, "Failed to load clink.\nSee log for details (%s).\n", buffer);
}

//------------------------------------------------------------------------------
static BOOL on_dll_attach()
{
    void* base;

    // Get the inject arguments.
    get_inject_args(GetCurrentProcessId());
    if (g_inject_args.profile_path[0] != '\0')
    {
        set_config_dir_override(g_inject_args.profile_path);
    }

    // Prepare the process and environment for Readline.
    set_rl_readline_name();
    prepare_env_for_inputrc();

    // Search for a supported shell.
    {
        int i;
        struct {
            const char*     name;
            const shell_t*  shell;
        } shells[] = {
            { "cmd.exe", &shell_cmd },
        };

        for (i = 0; i < sizeof_array(shells); ++i)
        {
            if (stricmp(rl_readline_name, shells[i].name) == 0)
            {
                g_shell = shells[i].shell;
                break;
            }
        }
    }

    // Not a supported shell?
    if (g_shell == NULL)
    {
        if (!g_inject_args.no_host_check)
        {
            LOG_INFO("Unsupported shell '%s'", rl_readline_name);
            return FALSE;
        }

        g_shell = &shell_generic;
    }

    if (!g_shell->validate())
    {
        LOG_INFO("Shell validation failed.");
        return FALSE;
    }

    if (!g_shell->initialise())
    {
        failed();
        return FALSE;
    }

    success();
    return TRUE;
}

//------------------------------------------------------------------------------
static BOOL on_dll_detach()
{
    if (g_shell != NULL)
    {
        g_shell->shutdown();

        save_history();
        shutdown_lua();
        shutdown_clink_settings();
    }

    return TRUE;
}

//------------------------------------------------------------------------------
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID unused)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:    return on_dll_attach();
    case DLL_PROCESS_DETACH:    return on_dll_detach();
    }

    return TRUE;
}

// vim: expandtab
