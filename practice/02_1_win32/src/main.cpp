#ifndef WIN32_LEAN_AND_MEAN
#define WINWIN32_LEAN_AND_MEAN
#include <errhandlingapi.h>
#include <handleapi.h>
#include <minwindef.h>
#include <synchapi.h>
#include <regex>
#endif

#ifndef NOMINMAX
#define NOMNOMINMAX
#endif

#include <WinSock2.h>
#include <WS2tcpip.h>
#include <Windows.h>

#include <iostream>

void demonstrateWin32Handle()
{
    HANDLE eventHandle{CreateEventW(nullptr, FALSE, FALSE, L"RemoteControlEvent")};
    if (eventHandle == nullptr)
    {
        DWORD const error{GetLastError()};
        std::cerr << "CreateEventW failed: " << error << "\n";
        return;
    }
    BOOL const closeResult{CloseHandle(eventHandle)};
    if (closeResult == FALSE)
    {
        DWORD const error{GetLastError()};
        std::cerr << "CloseHandle failed:" << error << "\n";
    }
    std::cerr << "handle success" << "\n";
}

void demonstrateWin32Socket()
{
    /*
    WSADATA wsaData{};
    int res{WSAStartup(MAKEWORD(2, 2), &wsaData)};
    if (res != 0)
    {
        std::cerr << "wsaStartUp failed" << res;
    }
    */

    SOCKET socket{::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)};
    if (socket == INVALID_SOCKET)
    {
        int const error{WSAGetLastError()};
        std::cerr << "socket fialed:" << error << "\n";
    }

    int const closeResult{closesocket(socket)};
    if (closeResult == SOCKET_ERROR)
    {
        int const error{WSAGetLastError()};
        std::cerr << "closeSocket failed:" << error << "\n";
    }

    std::cerr << "socket success" << "\n";
}

int main()
{
    demonstrateWin32Handle();
    demonstrateWin32Socket();

    return 0;
}
