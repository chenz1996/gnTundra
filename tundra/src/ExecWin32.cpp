/* exec-win32.c -- Windows subprocess handling
 *
 * This module is complicated due to how Win32 handles child I/O and because
 * its command processor cannot handle long command lines, requiring tools to
 * support so-called "response files" which are basically just the contents of
 * the command line, but written to a file. Tundra implements this via the
 * @RESPONSE handling.
 *
 * Also, rather than using the tty merging module that unix uses, this module
 * handles output merging via temporary files.  This removes the pipe
 * read/write deadlocks I've seen from time to time when using the TTY merging
 * code. Rather than losing my last sanity points on debugging win32 internals
 * I've opted for this much simpler approach.
 *
 * Instead of buffering stuff in memory via pipe babysitting, this new code
 * first passes the stdout handle straight down to the first process it spawns.
 * Subsequent child processes that are spawned when the TTY is busy instead get
 * to inherit a temporary file handle. Once the process completes, it will wait
 * to get the TTY and flush its temporary output file to the console. The
 * temporary is then deleted.
 */

#include "Exec.hpp"
#include "Common.hpp"
#include "Config.hpp"
#include "Mutex.hpp"
#include "BuildQueue.hpp"
#include "Atomic.hpp"
#include "SignalHandler.hpp"

#include <algorithm>

#if defined(TUNDRA_WIN32)

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <RestartManager.h>
#include <VersionHelpers.h>
#include <thread>

#include "Banned.hpp"


static char s_TemporaryDir[MAX_PATH];
static DWORD s_TundraPid;
static Mutex s_FdMutex;

//allocate one stdout and one stderr handle per job
static HANDLE s_TempFiles[kMaxBuildThreads];

static void ShowProgramsKeepingPathOpen(const char *path)
{
    const char *function;
    DWORD error;

    WCHAR pathWide[MAX_PATH];
    if (!MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, path, -1, pathWide, ARRAY_SIZE(pathWide)))
    {
        function = "MultiByteToWideChar";
        error = GetLastError();
        PrintErrno();
        goto exit;
    }

    DWORD dwSessionHandle;
    WCHAR sessionKey[CCH_RM_SESSION_KEY + 1];
    memset(sessionKey, 0, sizeof(sessionKey));
    function = "RmStartSession";
    error = RmStartSession(&dwSessionHandle, 0, sessionKey);
    if (error)
        goto exit;

    const WCHAR *rgsFileNames[] = {pathWide};
    function = "RmRegisterResources";
    error = RmRegisterResources(dwSessionHandle, ARRAY_SIZE(rgsFileNames), rgsFileNames, 0, NULL, 0, NULL);
    if (error)
        goto endSession;

    UINT nProcInfoNeeded;
    RM_PROCESS_INFO rgAffectedApps[16];
    UINT nProcInfo = ARRAY_SIZE(rgAffectedApps);
    DWORD dwRebootReasons;
    function = "RmGetList";
    error = RmGetList(dwSessionHandle, &nProcInfoNeeded, &nProcInfo, rgAffectedApps, &dwRebootReasons);
    if (error)
        goto endSession;

    fprintf(stderr, "tundra: found %u processes keeping the file \"%s\" open (showing %u).\n",
            (unsigned int)nProcInfoNeeded, path, (unsigned int)nProcInfo);

    for (UINT i = 0; i < nProcInfo; ++i)
    {
        fprintf(stderr, "- \"%ls\" (PID %u)\n",
                (wchar_t *)rgAffectedApps[i].strAppName,
                (unsigned int)rgAffectedApps[i].Process.dwProcessId);

        HANDLE hProcess;
        FILETIME creationTime, exitTime, kernelTime, userTime;
        WCHAR exeName[MAX_PATH];
        DWORD dwSize = ARRAY_SIZE(exeName);

        hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, rgAffectedApps[i].Process.dwProcessId);
        if (hProcess && GetProcessTimes(hProcess, &creationTime, &exitTime, &kernelTime, &userTime) && CompareFileTime(&rgAffectedApps[i].Process.ProcessStartTime, &creationTime) == 0 && QueryFullProcessImageNameW(hProcess, 0, exeName, &dwSize))
        {
            fprintf(stderr, "    %ls\n", exeName);
        }
        else
        {
            fputs("    could not determine process image path: ", stderr);
            PrintErrno();
        }

        if (hProcess)
            CloseHandle(hProcess);
    }

endSession:
    RmEndSession(dwSessionHandle);

exit:
    if (error)
        fprintf(stderr, "tundra: failed to list processes keeping file open (%s returned error %d).\n", function, error);
}

static HANDLE GetOrCreateTempFileFor(int job_id, const char *command_that_just_finished)
{
    if (job_id >= kMaxBuildThreads)
        CroakErrnoAbort("Trying to create a job with id %i, which is higher than the maximum allowed number of build threads %i", job_id, kMaxBuildThreads);

    HANDLE result = s_TempFiles[job_id];

    if (!result)
    {
        char temp_dir[MAX_PATH + 1];
        DWORD access, sharemode, disp, flags;

        _snprintf(temp_dir, MAX_PATH, "%stundra.%u.%d", s_TemporaryDir, s_TundraPid, job_id);
        temp_dir[MAX_PATH] = '\0';

        access = GENERIC_WRITE | GENERIC_READ;
        sharemode = FILE_SHARE_WRITE;
        disp = CREATE_ALWAYS;
        flags = FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE;

        result = CreateFileW(ToWideString(temp_dir).c_str(), access, sharemode, NULL, disp, flags, NULL);

        if (INVALID_HANDLE_VALUE == result)
        {
            bool was_sharing_violation = GetLastError() == 0x00000020;
            fprintf(stderr, "tundra: error: failed to create temporary file: %s\n", temp_dir);
            PrintErrno();
            if (command_that_just_finished)
            {
                fprintf(stderr, "The just completed command was:\n  %s\n", command_that_just_finished);
                if (was_sharing_violation)
                    fputs("Most likely, the build action spawned a lingering subprocess that is "
                          "keeping stdout/stderr open. The build action should either wait for "
                          "such subprocesses to terminate before returning, or prevent them from "
                          "inheriting its stdout/stderr handles to begin with.\n",
                          stderr);
            }
            ShowProgramsKeepingPathOpen(temp_dir);

            if (!command_that_just_finished || !was_sharing_violation)
                Croak("(!command_that_just_finished || !was_sharing_violation)");

            fputs("tundra: waiting for subprocesses to exit, and for the file to be deleted...\n", stderr);
            fflush(stderr);
            do
            {
                Sleep(1000);
                result = CreateFileW(ToWideString(temp_dir).c_str(), access, sharemode, NULL, disp, flags, NULL);
            } while (result == INVALID_HANDLE_VALUE);
        }

        SetHandleInformation(result, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);

        s_TempFiles[job_id] = result;
    }

    return result;
}

static void CopyTempFileContentsIntoBufferAndPrepareFileForReuse(int job_id, const char *command_that_just_finished, OutputBufferData *outputBuffer, MemAllocHeap *heap)
{
    HANDLE tempFile = s_TempFiles[job_id];

    //get the filesize
    DWORD fsize = SetFilePointer(tempFile, 0, NULL, FILE_CURRENT);

    // Rewind file position of the temporary file.
    SetFilePointer(tempFile, 0, NULL, FILE_BEGIN);

    assert(outputBuffer->buffer == nullptr);
    assert(outputBuffer->heap == nullptr);
    outputBuffer->buffer = static_cast<char *>(HeapAllocate(heap, fsize + 1));
    outputBuffer->heap = heap;
    outputBuffer->cursor = 0;
    outputBuffer->buffer_size = fsize;

    DWORD processed = 0;
    while (processed < fsize)
    {
        DWORD spaceRemaining = (DWORD)outputBuffer->buffer_size - outputBuffer->cursor;
        DWORD amountRead = 0;
        if (!ReadFile(tempFile, outputBuffer->buffer + outputBuffer->cursor, spaceRemaining, &amountRead, NULL) || amountRead == 0)
            CroakErrnoAbort("ReadFile from temporary file failed before we read all of its data");
        processed += amountRead;
        outputBuffer->cursor += amountRead;
    }
    outputBuffer->buffer[outputBuffer->cursor] = 0;

    // Close file (which should trigger its deletion).
    if (!CloseHandle(tempFile))
        CroakErrnoAbort("CloseHandle failed");
    s_TempFiles[job_id] = 0;

    // Immediately recreate the file. If a lingering process is keeping the file open, that is a bug, and we want to find out immediately.
    GetOrCreateTempFileFor(job_id, command_that_just_finished);
}

static struct Win32EnvBinding
{
    const char *m_Name;
    const char *m_Value;
    size_t m_NameLength;
} g_Win32Env[1024];

static char UTF8_WindowsEnvironment[128 * 1024];

static size_t g_Win32EnvCount;

void ExecInit(void)
{
    s_TundraPid = GetCurrentProcessId();

    if (0 == GetTempPathA(sizeof(s_TemporaryDir), s_TemporaryDir))
        CroakErrno("couldn't get temporary directory path");

    MutexInit(&s_FdMutex);

    // Initialize win32 env block. We're going to let it leak.
    // This block contains a series of nul-terminated strings, with a double nul
    // terminator to indicated the end of the data.

    // Since all our operations are in UTF8 space, we're going to convert the windows environment once on startup into utf8 as well,
    // so that all follow up operations are fast.
    WCHAR *widecharenv = GetEnvironmentStringsW();
    int len = 0;
    while ((*(widecharenv + len)) != 0 || (*(widecharenv + len + 1)) != 0)
        len++;
    len += 2;
    WideCharToMultiByte(CP_UTF8, 0, widecharenv, len, UTF8_WindowsEnvironment, sizeof UTF8_WindowsEnvironment, NULL, NULL);

    const char *env = UTF8_WindowsEnvironment;
    size_t env_count = 0;

    while (*env && env_count < ARRAY_SIZE(g_Win32Env))
    {
        size_t len = strlen(env);

        if (const char *eq = strchr(env, '='))
        {
            // Discard empty variables that Windows sometimes has internally
            if (eq != env)
            {
                g_Win32Env[env_count].m_Name = env;
                g_Win32Env[env_count].m_Value = eq + 1;
                g_Win32Env[env_count].m_NameLength = size_t(eq - env);
                ++env_count;
            }
        }

        env += len + 1;
    }

    g_Win32EnvCount = env_count;
}

static bool
AppendEnvVar(char *block, size_t block_size, size_t *cursor, const char *name, size_t name_len, const char *value)
{
    size_t value_len = strlen(value);
    size_t pos = *cursor;

    if (pos + name_len + value_len + 2 > block_size)
        return false;

    memcpy(block + pos, name, name_len);
    pos += name_len;

    block[pos++] = '=';
    memcpy(block + pos, value, value_len);
    pos += value_len;

    block[pos++] = '\0';

    *cursor = pos;
    return true;
}

extern char *s_Win32EnvBlock;

static bool
MakeEnvBlock(char *env_block, size_t block_size, const EnvVariable *env_vars, int env_count, size_t *out_env_block_length)
{
    size_t cursor = 0;
    size_t env_var_len[ARRAY_SIZE(g_Win32Env)];
    unsigned char used_env[ARRAY_SIZE(g_Win32Env)];

    if (env_count > int(sizeof used_env))
        return false;

    for (int i = 0; i < env_count; ++i)
    {
        env_var_len[i] = strlen(env_vars[i].m_Name);
    }

    memset(used_env, 0, sizeof(used_env));

    // Loop through windows environment block and emit anything we're not going to override.
    for (size_t i = 0, count = g_Win32EnvCount; i < count; ++i)
    {
        bool replaced = false;

        for (int x = 0; !replaced && x < env_count; ++x)
        {
            if (used_env[x])
                continue;

            size_t len = env_var_len[x];
            if (len == g_Win32Env[i].m_NameLength && 0 == _memicmp(g_Win32Env[i].m_Name, env_vars[x].m_Name, len))
            {
                if (!AppendEnvVar(env_block, block_size, &cursor, env_vars[x].m_Name, len, env_vars[x].m_Value))
                    return false;
                replaced = true;
                used_env[x] = 1;
            }
        }

        if (!replaced)
        {
            if (!AppendEnvVar(env_block, block_size, &cursor, g_Win32Env[i].m_Name, g_Win32Env[i].m_NameLength, g_Win32Env[i].m_Value))
                return false;
        }
    }

    // Loop through our env vars and emit those we didn't already push out.
    for (int i = 0; i < env_count; ++i)
    {
        if (used_env[i])
            continue;
        if (!AppendEnvVar(env_block, block_size, &cursor, env_vars[i].m_Name, env_var_len[i], env_vars[i].m_Value))
            return false;
    }

    env_block[cursor++] = '\0';
    env_block[cursor++] = '\0';
    *out_env_block_length = cursor;
    return true;
}

static bool SetupResponseFile(const char *cmd_line, char *out_new_cmd_line, int new_cmdline_max_length, char *out_responsefile, int response_file_max_length)
{
    static const char response_prefix[] = "@RESPONSE|";
    static const char response_suffix_char = '|';
    static const char always_response_prefix[] = "@RESPONSE!";
    static const char always_response_suffix_char = '!';
    static_assert(sizeof response_prefix == sizeof always_response_prefix, "Response prefix lengths differ");
    static const size_t response_prefix_len = sizeof(response_prefix) - 1;
    char command_buf[512];
    char option_buf[32];
    char response_suffix = response_suffix_char;
    const char *response;

    if (NULL == (response = strstr(cmd_line, response_prefix)))
    {
        if (NULL != (response = strstr(cmd_line, always_response_prefix)))
        {
            response_suffix = always_response_suffix_char;
        }
    }

    /* scan for a @RESPONSE|<option>|.... section at the end of the command line */
    if (NULL != response)
    {
        const size_t cmd_len = strlen(cmd_line);
        const char *option, *option_end;

        option = response + response_prefix_len;

        if (NULL == (option_end = strchr(option, response_suffix)))
        {
            fprintf(stderr, "badly formatted @RESPONSE section; missing %c after option: %s\n", response_suffix, cmd_line);
            return false;
        }

        /* Limit on XP and later is 8191 chars; but play it safe */
        if (response_suffix == always_response_suffix_char || cmd_len > 8000)
        {
            char tmp_dir[MAX_PATH];
            DWORD rc;

            rc = GetTempPath(sizeof(tmp_dir), tmp_dir);
            if (rc >= sizeof(tmp_dir) || 0 == rc)
            {
                fprintf(stderr, "couldn't get temporary directory for response file; win32 error=%d", (int)GetLastError());
                return false;
            }

            if ('\\' == tmp_dir[rc - 1])
                tmp_dir[rc - 1] = '\0';

            static uint32_t foo = 0;
            uint32_t sequence = AtomicIncrement(&foo);

            _snprintf(out_responsefile, response_file_max_length, "%s\\tundra.resp.%u.%u", tmp_dir, GetCurrentProcessId(), sequence);
            out_responsefile[response_file_max_length] = '\0';

            {
                HANDLE hf = CreateFileW(ToWideString(out_responsefile).c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
                if (INVALID_HANDLE_VALUE == hf)
                {
                    fprintf(stderr, "couldn't create response file %s; @err=%u", out_responsefile, (unsigned int)GetLastError());
                    return false;
                }

                DWORD written;
                WriteFile(hf, option_end + 1, (DWORD)strlen(option_end + 1), &written, NULL);

                if (!CloseHandle(hf))
                {
                    fprintf(stderr, "couldn't close response file %s: errno=%d", out_responsefile, errno);
                    return false;
                }
                hf = NULL;
            }

            {
                const int pre_suffix_len = (int)(response - cmd_line);
                int copy_len = std::min(pre_suffix_len, (int)(sizeof(command_buf) - 1));
                if (copy_len != pre_suffix_len)
                {
                    char truncated_cmd[sizeof(command_buf)];
                    _snprintf(truncated_cmd, sizeof(truncated_cmd) - 1, "%s", cmd_line);
                    truncated_cmd[sizeof(truncated_cmd) - 1] = '\0';

                    fprintf(stderr, "Couldn't copy command (%s...) before response file suffix. "
                                    "Move the response file suffix closer to the command starting position.\n",
                            truncated_cmd);
                    return false;
                }
                strncpy(command_buf, cmd_line, copy_len);
                command_buf[copy_len] = '\0';
                copy_len = std::min((int)(option_end - option), (int)(sizeof(option_buf) - 1));
                strncpy(option_buf, option, copy_len);
                option_buf[copy_len] = '\0';
            }

            _snprintf(out_new_cmd_line, new_cmdline_max_length, "%s %s%s", command_buf, option_buf, out_responsefile);
            out_new_cmd_line[new_cmdline_max_length - 1] = '\0';
        }
        else
        {
            size_t i, len;
            int copy_len = std::min((int)(response - cmd_line), (int)(sizeof(command_buf) - 1));
            strncpy(command_buf, cmd_line, copy_len);
            command_buf[copy_len] = '\0';
            _snprintf(out_new_cmd_line, new_cmdline_max_length, "%s%s", command_buf, option_end + 1);
            out_new_cmd_line[new_cmdline_max_length - 1] = '\0';

            /* Drop any newlines in the command line. They are needed for response
      * files only to make sure the max length doesn't exceed 128 kb */
            for (i = 0, len = strlen(out_new_cmd_line); i < len; ++i)
            {
                if (out_new_cmd_line[i] == '\n')
                {
                    out_new_cmd_line[i] = ' ';
                }
            }
        }
    }
    return true;
}

static void CleanupResponseFile(const char *responseFile)
{
    if (*responseFile != 0)
        RemoveFileOrDir(responseFile);
}

static int WaitForFinish(HANDLE processHandle, int (*callback_on_slow)(void *user_data), void *callback_on_slow_userdata, int time_until_first_callback)
{
    DWORD timeUntilNextSlowCallbackInvoke = callback_on_slow != nullptr ? time_until_first_callback : INFINITE;
    while (true)
    {
        DWORD waitResult = WaitForSingleObject(processHandle, timeUntilNextSlowCallbackInvoke * 1000);
        DWORD result_code = 0;
        switch (waitResult)
        {
        case WAIT_OBJECT_0:
            // OK - command ran to completion.
            GetExitCodeProcess(processHandle, &result_code);
            return result_code;

        case WAIT_TIMEOUT:
            timeUntilNextSlowCallbackInvoke = (*callback_on_slow)(callback_on_slow_userdata);
        }
    }
}

ExecResult ExecuteProcess(
    const char *cmd_line,
    int env_count,
    const EnvVariable *env_vars,
    MemAllocHeap *heap,
    int job_id,
    int (*callback_on_slow)(void *user_data),
    void *callback_on_slow_userdata,
    int time_until_first_callback)
{
    STARTUPINFOEXW sinfo;
    ZeroMemory(&sinfo, sizeof(STARTUPINFOEXW));

    sinfo.StartupInfo.cb = sizeof(STARTUPINFOEXW);
    DWORD creationFlags = CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT;

    
    void *attributeListAllocation = nullptr;
    
    sinfo.StartupInfo.hStdOutput = sinfo.StartupInfo.hStdError = GetOrCreateTempFileFor(job_id, NULL);
    sinfo.StartupInfo.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    sinfo.StartupInfo.dwFlags |= STARTF_USESTDHANDLES;
    creationFlags |= EXTENDED_STARTUPINFO_PRESENT;

    HANDLE handles_to_inherit[2];
    size_t num_handles_to_inherit = 1;
    handles_to_inherit[0] = sinfo.StartupInfo.hStdOutput;

    if (IsWindows8OrGreater()) // Inheriting stdin fails on Windows 7 with Win32 error 1450
    {
        handles_to_inherit[1] = sinfo.StartupInfo.hStdInput;
        ++num_handles_to_inherit;
    }

    SIZE_T attributeListSize = 0;

    //this is pretty crazy, but this call is _supposed_ to fail, and give us the correct attributeListSize, so we verify the returncode !=0
    if (InitializeProcThreadAttributeList(NULL, 1, 0, &attributeListSize))
        CroakErrnoAbort("InitializeProcThreadAttributeList failed");

    attributeListAllocation = HeapAllocate(heap, attributeListSize);
    sinfo.lpAttributeList = static_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeListAllocation);

    //but this call is supposed to succeed, so here we check it for returning ==0
    if (!InitializeProcThreadAttributeList(sinfo.lpAttributeList, 1, 0, &attributeListSize))
        CroakErrno("InitializeProcThreadAttributeList failed (2)");
    if (!UpdateProcThreadAttribute(sinfo.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles_to_inherit, sizeof(HANDLE) * num_handles_to_inherit, NULL, NULL))
        CroakErrno("UpdateProcThreadAttribute failed");


    char buffer[8192];
    char env_block[128 * 1024];
    WCHAR env_block_wide[128 * 1024];
    size_t env_block_length = 0;
    if (!MakeEnvBlock(env_block, sizeof(env_block) - 2, env_vars, env_count, &env_block_length))
        CroakAbort("env block error; too big?\n");

    if (!MultiByteToWideChar(CP_UTF8, 0, env_block, (int)env_block_length, env_block_wide, sizeof(env_block_wide) / sizeof(WCHAR)))
        CroakErrnoAbort("Failed converting environment block to wide char");

    ExecResult result;
    char new_cmd[8192];
    char responseFile[MAX_PATH];
    ZeroMemory(&result, sizeof(ExecResult));
    ZeroMemory(&responseFile, sizeof(responseFile));
    ZeroMemory(&new_cmd, sizeof(new_cmd));

    if (!SetupResponseFile(cmd_line, new_cmd, sizeof new_cmd, responseFile, sizeof responseFile))
        return result;

    const char *cmd_to_use = new_cmd[0] == 0 ? cmd_line : new_cmd;
    _snprintf(buffer, sizeof(buffer), "cmd.exe /c \"%s\"", cmd_to_use);
    buffer[sizeof(buffer) - 1] = '\0';

    HANDLE job_object = CreateJobObject(NULL, NULL);
    if (!job_object)
        CroakErrno("Couldn't create job object.");

    WCHAR buffer_wide[sizeof(buffer) * 2];
    if (!MultiByteToWideChar(CP_UTF8, 0, buffer, (int)sizeof(buffer), buffer_wide, sizeof(buffer_wide) / sizeof(WCHAR)))
        CroakErrnoAbort("Failed converting buffer block to wide char");

    PROCESS_INFORMATION pinfo;

    if (!CreateProcessW(NULL, buffer_wide, NULL, NULL, TRUE, creationFlags, env_block_wide, NULL, &sinfo.StartupInfo, &pinfo))
        CroakErrnoAbort("Couldn't launch process with command line:\n%s", buffer);

    DeleteProcThreadAttributeList(sinfo.lpAttributeList);
    HeapFree(heap, attributeListAllocation);

    AssignProcessToJobObject(job_object, pinfo.hProcess);
    ResumeThread(pinfo.hThread);
    CloseHandle(pinfo.hThread);

    result.m_ReturnCode = WaitForFinish(pinfo.hProcess, callback_on_slow, callback_on_slow_userdata, time_until_first_callback);

    CleanupResponseFile(responseFile);

    CopyTempFileContentsIntoBufferAndPrepareFileForReuse(job_id, buffer, &result.m_OutputBuffer, heap);

    CloseHandle(pinfo.hProcess);
    CloseHandle(job_object);

    return result;
}



#endif /* TUNDRA_WIN32 */
