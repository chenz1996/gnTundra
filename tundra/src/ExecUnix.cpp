// ExecUnix.c -- subprocess spawning and output handling for unix-like systems

#include "Exec.hpp"
#include "Common.hpp"
#include "MemAllocHeap.hpp"

#if defined(TUNDRA_UNIX)

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <limits.h>

#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <stdlib.h>
#include <libgen.h>
#include <errno.h>

#include "Banned.hpp"



static void SetFdNonBlocking(int fd)
{
    int flags;

    flags = fcntl(fd, F_GETFL);
    flags |= O_NONBLOCK;
    if (-1 == fcntl(fd, F_SETFL, flags))
        CroakErrno("couldn't unblock fd %d", fd);
}

void ExecInit()
{
}

static int
EmitData(ExecResult *execResult, int fd)
{
    char text[8192];
    ssize_t count;

    count = read(fd, text, sizeof(text) - 1);

    if (count <= 0)
    {
        if (EAGAIN == errno)
            return 0;
        else
            return -1;
    }

    text[count] = '\0';

    EmitOutputBytesToDestination(execResult, text, count);

    return 0;
}

ExecResult
ExecuteProcess(
    const char *cmd_line,
    int env_count,
    const EnvVariable *env_vars,
    MemAllocHeap *heap,
    int job_id,
    int (*callback_on_slow)(void *user_data),
    void *callback_on_slow_userdata,
    int time_to_first_slow_callback)
{
    ExecResult result;

    result.m_ReturnCode = 1;
    result.m_OutputBuffer.buffer = nullptr;

    if (heap == nullptr)
        CroakAbort("Either pass in a heap so we can allocate buffers to store stdout");

    InitOutputBuffer(&result.m_OutputBuffer, heap);

    pid_t child;
    const int pipe_read = 0;
    const int pipe_write = 1;

    /* Create a pair of pipes to read back stdout, stderr */
    int stdout_pipe[2], stderr_pipe[2];

    if (-1 == pipe(stdout_pipe))
    {
        perror("pipe failed");
        return result;
    }

    if (-1 == pipe(stderr_pipe))
    {
        perror("pipe failed");
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        return result;
    }

    if (0 == (child = fork()))
    {
        const char *args[] = {"/bin/sh", "-c", cmd_line, NULL};

        close(stdout_pipe[pipe_read]);
        close(stderr_pipe[pipe_read]);

        if (-1 == dup2(stdout_pipe[pipe_write], STDOUT_FILENO))
            perror("dup2 failed");
        if (-1 == dup2(stderr_pipe[pipe_write], STDERR_FILENO))
            perror("dup2 failed");

        close(stdout_pipe[pipe_write]);
        close(stderr_pipe[pipe_write]);

        sigset_t sigs;
        sigfillset(&sigs);
        if (0 != sigprocmask(SIG_UNBLOCK, &sigs, 0))
            perror("sigprocmask failed");

        for (int i = 0; i < env_count; ++i)
        {
            setenv(env_vars[i].m_Name, env_vars[i].m_Value, /*overwrite:*/ 1);
        }

        if (-1 == execv("/bin/sh", (char **)args))
            Croak("Failed executing /bin/sh");
        /* we never get here */
        abort();
    }
    else if (-1 == child)
    {
        perror("fork failed");
        close(stdout_pipe[pipe_read]);
        close(stderr_pipe[pipe_read]);
        close(stdout_pipe[pipe_write]);
        close(stderr_pipe[pipe_write]);
        return result;
    }
    else
    {
        pid_t p;
        int return_code = 0;
        int rfd_count = 2;
        int rfds[2];
        fd_set read_fds;

        rfds[0] = stdout_pipe[pipe_read];
        rfds[1] = stderr_pipe[pipe_read];

        SetFdNonBlocking(rfds[0]);
        SetFdNonBlocking(rfds[1]);

        /* Close write end of the pipe, we're just going to be reading */
        close(stdout_pipe[pipe_write]);
        close(stderr_pipe[pipe_write]);

        /* Sit in a select loop over the two fds */

        //		int time_until_next_slow_callback = time_to_first_slow_callback;

        uint64_t now = TimerGet();
        uint64_t next_callback_at = now + TimerFromSeconds(time_to_first_slow_callback);

        for (;;)
        {
            int fd;
            int count;
            int max_fd = 0;
            struct timeval timeout;

            /* don't select if we know both pipes are closed */
            if (rfd_count > 0)
            {
                FD_ZERO(&read_fds);

                for (fd = 0; fd < 2; ++fd)
                {
                    if (rfds[fd])
                    {
                        if (rfds[fd] > max_fd)
                            max_fd = rfds[fd];
                        FD_SET(rfds[fd], &read_fds);
                    }
                }

                ++max_fd;

                now = TimerGet();
                timeout.tv_sec = (int)TimerDiffSeconds(now, next_callback_at);
                if (timeout.tv_sec < 1)
                    timeout.tv_sec = 1;
                timeout.tv_usec = 0;

                count = select(max_fd, &read_fds, NULL, NULL, &timeout);

                if (callback_on_slow != nullptr)
                {
                    if (TimerGet() > next_callback_at)
                        next_callback_at = TimerGet() + (*callback_on_slow)(callback_on_slow_userdata);
                }
                if (-1 == count) // happens in gdb due to syscall interruption
                    continue;

                for (fd = 0; fd < 2; ++fd)
                {
                    if (0 != rfds[fd] && FD_ISSET(rfds[fd], &read_fds))
                    {
                        if (0 != EmitData(&result, rfds[fd]))
                        {
                            /* Done with this FD. */
                            rfds[fd] = 0;
                            --rfd_count;
                        }
                    }
                }
            }

            return_code = 0;
            p = waitpid(child, &return_code, rfd_count > 0 ? WNOHANG : 0);

            if (0 == p)
            {
                /* child still running */
                continue;
            }
            else if (p != child)
            {
                return_code = 1;
                perror("waitpid failed");
                break;
            }
            else
            {
                /* fall out of the loop here - process has exited. */
                /* FIXME - is there a race between getting the last data out of
				 * the pipes vs quitting here? Probably there is. But it seems
				 * to work well in practice. If I put a blocking waitpid()
				 * after the loop I got deadlocks on Mac OS X in select. */
                break;
            }
        }

        close(stdout_pipe[pipe_read]);
        close(stderr_pipe[pipe_read]);

        if (WIFEXITED(return_code))
            result.m_ReturnCode = WEXITSTATUS(return_code);
        else
            result.m_ReturnCode = 128 + WTERMSIG(return_code);
            
        return result;
    }
}


#endif /* TUNDRA_UNIX */
