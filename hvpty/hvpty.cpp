/* 
 * This file is part of wslbridge2 project
 * Licensed under the GNU General Public License version 3
 * Copyright (C) 2019 Biswapriyo Nath
 */

#include <windows.h>
#include <winsock.h>
#include <assert.h>
#include <errno.h>
#include <getopt.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "../hvsocket/hvsocket.h"
#include "../wslbridge/Helpers.hpp"
#include "../wslbridge/Environment.hpp"
#include "../wslbridge/SocketIo.hpp"
#include "../wslbridge/TerminalState.hpp"

/* The range 49152–65535 contains dynamic ports */
#define DYNAMIC_PORT_LOW 49152
#define DYNAMIC_PORT_HIGH 65535
#define BIND_MAX_RETRIES 10

static int random_port(void)
{
    return rand() % (DYNAMIC_PORT_HIGH - DYNAMIC_PORT_LOW) + DYNAMIC_PORT_LOW;
}

/* Enable this to show debug information */
static const char IsDebugMode = 0;

void GetVmId(
    GUID *LxInstanceID,
    const std::wstring &DistroName,
    int *WslVersion);

/* return accepted client socket to receive */
static SOCKET Initialize(std::wstring &command, GUID *VmId)
{
    int ret;

    const SOCKET sServer = socket(AF_HYPERV, SOCK_STREAM, HV_PROTOCOL_RAW);
    assert(sServer > 0);

    const int optval = 1;
    ret = setsockopt(sServer, HV_PROTOCOL_RAW, HVSOCKET_CONNECTED_SUSPEND,
                    (const char *)&optval, sizeof optval);
    assert(ret == 0);

    struct SOCKADDR_HV addr;
    memset(&addr, 0, sizeof addr);

    addr.Family = AF_HYPERV;
    memcpy(&addr.VmId, VmId, sizeof addr.VmId);
    memcpy(&addr.ServiceId, &HV_GUID_VSOCK_TEMPLATE, sizeof addr.ServiceId);

    /* Try to bind to a dynamic port */
    int nretries = 0;
    int initPort;

    while (nretries < BIND_MAX_RETRIES)
    {
        initPort = random_port();
        addr.ServiceId.Data1 = initPort;
        ret = bind(sServer, (struct sockaddr *)&addr, sizeof addr);
        if (ret == 0)
            break;

        nretries ++;
    }

    /* Fill-in the placeholder of port number */
    {
        int portpos = command.find(L"$PORT");
        assert(portpos >= 0);
        std::array<wchar_t, 1024> buffer;
        ret = swprintf(buffer.data(), buffer.size(), L"%d", initPort);
        assert(ret > 0);
        command.replace(portpos, 5, buffer.data());
    }

    ret = listen(sServer, -1);
    assert(ret == 0);

    PROCESS_INFORMATION pi = {};
    STARTUPINFOW si = {};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESHOWWINDOW;

    if (IsDebugMode)
        si.wShowWindow = SW_SHOW;
    else
        si.wShowWindow = SW_HIDE;

    if (!command.empty())
    {
        ret = CreateProcessW(nullptr, &command[0], nullptr,
                             nullptr, false, CREATE_NEW_CONSOLE,
                             nullptr, nullptr, &si, &pi);
        if (!ret)
            fatal("backend start error: %d\n", GetLastError());
    }
    else
        fatal("error: backend command string is empty\n");

    SOCKET sClient = accept(sServer, nullptr, nullptr);
    assert(sClient > 0);

    closesocket(sServer);
    return sClient;
}

/* return socket and connect to random port number */
static SOCKET create_hvsock(unsigned int randomPort, GUID *VmId)
{
    int ret;

    const SOCKET sock = socket(AF_HYPERV, SOCK_STREAM, HV_PROTOCOL_RAW);
    assert(sock > 0);

    const int optval = 1;
    ret = setsockopt(sock, HV_PROTOCOL_RAW, HVSOCKET_CONNECTED_SUSPEND,
                    (const char *)&optval, sizeof optval);
    assert(ret == 0);

    const int timeout = 10 * 1000;
    ret = setsockopt(sock, HV_PROTOCOL_RAW, HVSOCKET_CONNECT_TIMEOUT,
                    (const char *)&timeout, sizeof timeout);
    assert(ret == 0);

    struct SOCKADDR_HV addr;
    memset(&addr, 0, sizeof addr);

    addr.Family = AF_HYPERV;
    memcpy(&addr.VmId, VmId, sizeof addr.VmId);
    memcpy(&addr.ServiceId, &HV_GUID_VSOCK_TEMPLATE, sizeof addr.ServiceId);
    addr.ServiceId.Data1 = randomPort;
    ret = connect(sock, (struct sockaddr *)&addr, sizeof addr);
    assert(ret == 0);

    return sock;
}

union IoSockets
{
    SOCKET sock[4];
    struct
    {
        SOCKET inputSock;
        SOCKET outputSock;
        SOCKET errorSock;
        SOCKET controlSock;
    };
};

/* global variable */
static union IoSockets g_ioSockets = { 0 };

static void* resize_window(void *set)
{
    int ret, signum;
    struct winsize winp;

    while (1)
    {
        /* wait for the window resize signal aka. SIGWINCH */
        ret = sigwait((sigset_t *)set, &signum);
        if (ret != 0 || signum != SIGWINCH)
            break;

        /* send terminal window size to control socket */
        ioctl(STDIN_FILENO, TIOCGWINSZ, &winp);
        send(g_ioSockets.controlSock, (char *)&winp, sizeof winp, 0);

        if (IsDebugMode)
            printf("cols: %d row: %d\n", winp.ws_col,winp.ws_row);
    }

    return nullptr;
}

static void* send_buffer(void *param)
{
    int ret;
    char data[1024];

    struct pollfd fds = { STDIN_FILENO, POLLIN, 0 };

    while (1)
    {
        ret = poll(&fds, 1, -1);

        if (fds.revents & POLLIN)
        {
            ret = read(STDIN_FILENO, data, sizeof data);
            if (ret > 0)
                ret = send(g_ioSockets.inputSock, data, ret, 0);
            else
                break;
        }
    }

    return nullptr;
}

static void usage(const char *prog)
{
    printf(
    "Run a program in WSL using AF_HYPERV sockets\n"
    "Usage: %s [--] [options] [arguments]\n"
    "\n"
    "Options:\n"
    "  -b, --backend BACKEND\n"
    "                    Overrides the default path of wslbridge2-backend to BACKEND\n"
    "  -d, --distribution Distribution Name\n"
    "                    Run the specified distribution\n"
    "  -e VAR            Copies VAR into the WSL environment\n"
    "  -e VAR=VAL        Sets VAR to VAL in the WSL environment\n"
    "  -h, --help        Show this usage information\n"
    "  -l, --login       Start a login shell\n"
    "  -L, --no-login    Do not start a login shell\n"
    "  -u, --user WSL User Name\n"
    "                    Run as the specified user\n"
    "  -w, --windir Folder\n"
    "                    Changes the working directory to Windows style path\n"
    "  -W, --wsldir Folder\n"
    "                    Changes the working directory to Unix style path\n",
    prog);
    exit(0);
}

static void invalid_arg(const char *arg)
{
    fatal("error: the %s option requires a non-empty string argument\n", arg);
}

int main(int argc, char *argv[])
{
    srand(time(nullptr));
    int ret;

    struct WSAData wdata;
    ret = WSAStartup(MAKEWORD(2,2), &wdata);
    assert(ret == 0);

    const char shortopts[] = "+b:d:e:hlLu:w:W:";
    const struct option longopts[] = {
        { "backend",       required_argument, 0, 'b' },
        { "distribution",  required_argument, 0, 'd' },
        { "help",          no_argument,       0, 'h' },
        { "login",         no_argument,       0, 'l' },
        { "no-login",      no_argument,       0, 'L' },
        { "user",          required_argument, 0, 'u' },
        { "windir",        required_argument, 0, 'w' },
        { "wsldir",        required_argument, 0, 'W' },
        { 0,               no_argument,       0,  0  },
    };

    Environment env;
    std::string distroName;
    std::string customBackendPath;
    std::string userName;
    std::string winDir;
    std::string wslDir;

    enum class LoginMode { Auto, Yes, No } loginMode = LoginMode::Auto;
    if (argv[0][0] == '-')
        loginMode = LoginMode::Yes;

    int c = 0;
    while ((c = getopt_long(argc, argv, shortopts, longopts, nullptr)) != -1)
    {
        switch (c)
        {
            case 0:
                /* Ignore long option. */
                break;

            case 'b':
                customBackendPath = optarg;
                if (customBackendPath.empty())
                    invalid_arg("backend");
                break;

            case 'd':
                distroName = optarg;
                if (distroName.empty())
                    invalid_arg("distribution");
                break;

            case 'e':
            {
                const char *eq = strchr(optarg, '=');
                const std::string varname = eq ?
                                            std::string(optarg, eq - optarg)
                                            : std::string(optarg);
                if (varname.empty())
                    invalid_arg("environment");

                if (eq)
                    env.set(varname, eq + 1);
                else
                    env.set(varname);
                break;
            }

            case 'h':
                usage(argv[0]);
                break;

            case 'l':
                loginMode = LoginMode::Yes;
                break;

            case 'L':
                loginMode = LoginMode::No;
                break;

            case 'u':
                userName = optarg;
                if (userName.empty())
                    invalid_arg("user");
                break;

            case 'w':
                winDir = optarg;
                if (winDir.empty())
                    invalid_arg("windir");
                break;

            case 'W':
                wslDir = optarg;
                if (wslDir.empty())
                    invalid_arg("wsldir");
                break;

            default:
                fatal("Try '%s --help' for more information.\n", argv[0]);
        }
    }

    const std::wstring wslPath = findSystemProgram(L"wsl.exe");
    const auto backendPathInfo = normalizePath(
                    findBackendProgram(customBackendPath, L"hvpty-backend"));
    const std::wstring backendPathWin = backendPathInfo.first;
    const std::wstring fsname = backendPathInfo.second;
    const struct TermSize initialSize = terminalSize();

    /* Prepare the backend command line */
    std::wstring wslCmdLine;
    wslCmdLine.append(L"\"$(wslpath -u");
    appendWslArg(wslCmdLine, backendPathWin);
    wslCmdLine.append(L")\"");

    /* If no extra command use login mode by default
    const bool hasCommand = optind < argc;
    if (loginMode == LoginMode::Auto)
        loginMode = hasCommand ? LoginMode::No : LoginMode::Yes;
    */

    if (loginMode == LoginMode::Yes)
        appendWslArg(wslCmdLine, L"--login");

    for (const auto &envPair : env.pairs())
        appendWslArg(wslCmdLine, L"-e" + envPair.first + L"=" + envPair.second);

    {
        std::array<wchar_t, 1024> buffer;
        ret = swprintf(buffer.data(), buffer.size(),
                       L" --cols %d --rows %d --port $PORT",
                       initialSize.cols,
                       initialSize.rows);

        assert(ret > 0);
        wslCmdLine.append(buffer.data());
    }

    if (!wslDir.empty())
    {
        wslCmdLine.append(L" --path \"");
        wslCmdLine.append(mbsToWcs(wslDir));
        wslCmdLine.append(L"\"");
    }

    /* Append wsl.exe options and its arguments */
    std::wstring cmdLine;
    cmdLine.append(L"\"");
    cmdLine.append(wslPath);
    cmdLine.append(L"\"");

    if (!distroName.empty())
    {
        cmdLine.append(L" -d ");
        cmdLine.append(mbsToWcs(distroName));
    }

   /* Taken from HKCU\Directory\Background\shell\WSL\command */
    if (!winDir.empty())
    {
        cmdLine.append(L" --cd \"");
        cmdLine.append(mbsToWcs(winDir));
        cmdLine.append(L"\"");
    }

    if (!userName.empty())
    {
        cmdLine.append(L" --user ");
        cmdLine.append(mbsToWcs(userName));
    }

    cmdLine.append(L" /bin/sh -c ");
    appendWslArg(cmdLine, wslCmdLine);

    if (IsDebugMode)
        wprintf(L"%ls\n", cmdLine.c_str());

    /* Create server to receive random port number */
    GUID VmId;
    int WslVersion;
    GetVmId(&VmId, mbsToWcs(distroName), &WslVersion);
    if (WslVersion != 2)
        fatal("This is for WSL2 distributions only\n");

    const SOCKET sClient = Initialize(cmdLine, &VmId);

    unsigned int randomPort = 0;
    ret = recv(sClient, (char *)&randomPort, sizeof randomPort, 0);
    assert(ret > 0);
    closesocket(sClient);

    /* Create four I/O sockets and connect with WSL server */
    for (int i = 0; i < 4; i++)
    {
        g_ioSockets.sock[i] = create_hvsock(randomPort, &VmId);
        assert(g_ioSockets.sock[i] > 0);
    }

    /* Create thread to send window size through control socket */
    pthread_t tidResize;
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGWINCH);
    pthread_sigmask(SIG_BLOCK, &set, nullptr);
    ret = pthread_create(&tidResize, nullptr, resize_window, (void *)&set);
    assert(ret == 0);

    /* Create thread to send input buffer to input socket */
    pthread_t tidInput;
    ret = pthread_create(&tidInput, nullptr, send_buffer, nullptr);
    assert(ret == 0);

    TerminalState termState;
    termState.enterRawMode();

    char data[1024];

    while (1)
    {
        ret = recv(g_ioSockets.outputSock, data, sizeof data, 0);
        if (ret > 0)
            ret = write(STDOUT_FILENO, data, ret);
        else
            break;
    }

    /* cleanup */
    for (int i = 0; i < 4; i++)
        closesocket(g_ioSockets.sock[i]);
    WSACleanup();
    termState.exitCleanly(0);
}
