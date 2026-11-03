// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Process.h"

#if JUCE_WINDOWS
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#if JUCE_MAC
#include <crt_externs.h>
#else
extern char** environ;
#endif
#endif

namespace sedef::update
{
namespace
{
/// Text from a program's output: UTF-8, or with anything that is not replaced by '?'.
juce::String text (const std::vector<char>& bytes)
{
    if (bytes.empty())
        return {};
    std::vector<char> copy (bytes);
    copy.push_back (0);
    if (juce::CharPointer_UTF8::isValidString (copy.data(), static_cast<int> (bytes.size())))
        return juce::String::fromUTF8 (copy.data(), static_cast<int> (bytes.size()));
    for (auto& c : copy)
        if (static_cast<unsigned char> (c) >= 0x80)
            c = '?';
    return juce::String (copy.data());
}

#if !JUCE_WINDOWS
char** environment()
{
#if JUCE_MAC
    return *_NSGetEnviron(); // a plug-in can't link to `environ` on macOS
#else
    return environ;
#endif
}

/// argv as the C strings posix_spawn wants.
struct CArgv
{
    explicit CArgv (const juce::StringArray& args)
    {
        for (const auto& a : args)
            storage.push_back (a.toStdString());
        for (auto& s : storage)
            pointers.push_back (s.data());
        pointers.push_back (nullptr);
    }
    std::vector<std::string> storage;
    std::vector<char*> pointers;
};

/// The child starts with default signal handling and no blocked signals, whatever the host set,
/// and in its own process group, so a kill reaches what it started too.
class Attributes
{
public:
    explicit Attributes (bool detached)
    {
        ok_ = posix_spawnattr_init (&attr_) == 0;
        if (!ok_)
            return;
        sigset_t none, defaults;
        sigemptyset (&none);
        sigemptyset (&defaults);
        for (const int s :
             { SIGPIPE, SIGINT, SIGQUIT, SIGTERM, SIGHUP, SIGCHLD, SIGALRM, SIGUSR1, SIGUSR2 })
            sigaddset (&defaults, s);
        int flags = POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
#if JUCE_MAC
        flags |= POSIX_SPAWN_CLOEXEC_DEFAULT; // no descriptor of the host's leaks into it
#endif
        if (detached)
        {
#ifdef POSIX_SPAWN_SETSID
            flags |= POSIX_SPAWN_SETSID;
#else
            flags |= POSIX_SPAWN_SETPGROUP;
#endif
        }
        else
        {
            flags |= POSIX_SPAWN_SETPGROUP;
        }
        posix_spawnattr_setpgroup (&attr_, 0);
        posix_spawnattr_setsigmask (&attr_, &none);
        posix_spawnattr_setsigdefault (&attr_, &defaults);
        posix_spawnattr_setflags (&attr_, static_cast<short> (flags));
    }
    ~Attributes()
    {
        if (ok_)
            posix_spawnattr_destroy (&attr_);
    }
    Attributes (const Attributes&) = delete;
    Attributes& operator= (const Attributes&) = delete;

    bool ok() const noexcept { return ok_; }
    const posix_spawnattr_t* get() const noexcept { return &attr_; }

private:
    posix_spawnattr_t attr_ {};
    bool ok_ = false;
};

class FileActions
{
public:
    FileActions() { ok_ = posix_spawn_file_actions_init (&actions_) == 0; }
    ~FileActions()
    {
        if (ok_)
            posix_spawn_file_actions_destroy (&actions_);
    }
    FileActions (const FileActions&) = delete;
    FileActions& operator= (const FileActions&) = delete;

    bool ok() const noexcept { return ok_; }
    posix_spawn_file_actions_t* get() noexcept { return &actions_; }

private:
    posix_spawn_file_actions_t actions_ {};
    bool ok_ = false;
};

/// A pipe whose ends close on exec (the child gets copies on 1 and 2).
struct Pipe
{
    int read = -1, write = -1;
    bool open()
    {
        int fds[2];
        if (pipe (fds) != 0)
            return false;
        read = fds[0];
        write = fds[1];
        fcntl (read, F_SETFD, FD_CLOEXEC);
        fcntl (write, F_SETFD, FD_CLOEXEC);
        fcntl (read, F_SETFL, fcntl (read, F_GETFL) | O_NONBLOCK);
        return true;
    }
    void closeWrite()
    {
        if (write >= 0)
            close (write);
        write = -1;
    }
    ~Pipe()
    {
        closeWrite();
        if (read >= 0)
            close (read);
    }
};

/// Reads what is there without blocking; false once the pipe has closed.
bool drain (int fd, std::vector<char>& into)
{
    char buffer[4096];
    for (;;)
    {
        const auto n = ::read (fd, buffer, sizeof (buffer));
        if (n > 0)
        {
            const auto room = static_cast<size_t> (kMaxProcessOutput) - into.size();
            into.insert (into.end(), buffer, buffer + juce::jmin (room, static_cast<size_t> (n)));
            continue;
        }
        if (n == 0)
            return false;
        if (errno == EINTR)
            continue;
        return errno == EAGAIN || errno == EWOULDBLOCK;
    }
}

int exitCodeOf (int status)
{
    if (WIFEXITED (status))
        return WEXITSTATUS (status);
    if (WIFSIGNALED (status))
        return -WTERMSIG (status);
    return -1;
}
#endif
} // namespace

juce::String ProcessResult::summary() const
{
    auto all = (errors.trim() + "\n" + output.trim()).trim();
    if (all.length() > 800)
        all = "..." + all.getLastCharacters (800);
    return all;
}

int currentProcessId() noexcept
{
#if JUCE_WINDOWS
    return static_cast<int> (GetCurrentProcessId());
#else
    return static_cast<int> (getpid());
#endif
}

#if JUCE_WINDOWS

ProcessResult runProcess (const juce::StringArray&, int, const std::function<bool()>&)
{
    return {};
}

bool spawnDetached (const juce::StringArray&)
{
    return false;
}

#else

ProcessResult runProcess (const juce::StringArray& argv, int timeoutMs,
                          const std::function<bool()>& cancelled)
{
    ProcessResult r;
    if (argv.isEmpty() || !juce::File::isAbsolutePath (argv[0]))
        return r;
    Pipe out, err;
    FileActions actions;
    const Attributes attributes (false);
    if (!out.open() || !err.open() || !actions.ok() || !attributes.ok())
        return r;
    posix_spawn_file_actions_addopen (actions.get(), 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2 (actions.get(), out.write, 1);
    posix_spawn_file_actions_adddup2 (actions.get(), err.write, 2);

    const CArgv args (argv);
    pid_t pid = -1;
    if (posix_spawn (&pid, args.pointers[0], actions.get(), attributes.get(), args.pointers.data(),
                     environment()) != 0)
        return r;
    r.started = true;
    out.closeWrite();
    err.closeWrite();

    std::vector<char> stdoutBytes, stderrBytes;
    bool outOpen = true, errOpen = true;
    const auto started = juce::Time::getMillisecondCounterHiRes();
    int status = 0;
    bool reaped = false;
    for (;;)
    {
        pollfd fds[2] {};
        nfds_t n = 0;
        if (outOpen)
            fds[n++] = { out.read, POLLIN, 0 };
        if (errOpen)
            fds[n++] = { err.read, POLLIN, 0 };
        if (n > 0)
            poll (fds, n, 50);
        else
            usleep (20000);
        if (outOpen)
            outOpen = drain (out.read, stdoutBytes);
        if (errOpen)
            errOpen = drain (err.read, stderrBytes);

        const auto w = waitpid (pid, &status, WNOHANG);
        if (w == pid)
        {
            reaped = true;
            break;
        }
        if (w < 0 && errno != EINTR)
            break; // someone else reaped it (a host's SIGCHLD handler): the exit code is lost
        const bool late =
            timeoutMs > 0 && juce::Time::getMillisecondCounterHiRes() - started > timeoutMs;
        if (late || (cancelled && cancelled()))
        {
            kill (-pid, SIGKILL); // its process group: what it started goes too
            kill (pid, SIGKILL);
            while (waitpid (pid, &status, 0) < 0 && errno == EINTR)
            {
            }
            r.killed = true;
            break;
        }
    }
    if (outOpen)
        drain (out.read, stdoutBytes);
    if (errOpen)
        drain (err.read, stderrBytes);
    if (reaped)
        r.exitCode = exitCodeOf (status);
    r.output = text (stdoutBytes);
    r.errors = text (stderrBytes);
    return r;
}

bool spawnDetached (const juce::StringArray& argv)
{
    if (argv.isEmpty() || !juce::File::isAbsolutePath (argv[0]))
        return false;
    FileActions actions;
    const Attributes attributes (true);
    if (!actions.ok() || !attributes.ok())
        return false;
    posix_spawn_file_actions_addopen (actions.get(), 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen (actions.get(), 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen (actions.get(), 2, "/dev/null", O_WRONLY, 0);
    const CArgv args (argv);
    pid_t pid = -1;
    return posix_spawn (&pid, args.pointers[0], actions.get(), attributes.get(),
                        args.pointers.data(), environment()) == 0;
}

#endif

} // namespace sedef::update
