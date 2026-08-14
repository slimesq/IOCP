#ifndef WIN32_LEAN_AND_MEAN
#define WINWIN32_LEAN_AND_MEAN
#include <errhandlingapi.h>
#include <handleapi.h>
#include <minwindef.h>
#include <synchapi.h>
#include <winsock2.h>
#include <regex>
#include <stdexcept>
#endif

#ifndef NOMINMAX
#define NOMNOMINMAX
#endif

#include <WinSock2.h>
#include <WS2tcpip.h>
#include <Windows.h>

#include <iostream>

class Client
{
public:
    static Client& getInstance()
    {
        static Client clinet{};
        return clinet;
    }

    void connectToHost(std::string const& _ip, short _port)
    {
        struct sockaddr_in sockAddr{};
        sockAddr.sin_family = AF_INET;
        sockAddr.sin_port = ::htons(_port);
        ::InetPtonA(AF_INET, _ip.c_str(), &sockAddr.sin_addr);
        if (::connect(m_socket,
                      reinterpret_cast<struct sockaddr*>(&sockAddr),
                      static_cast<int>(sizeof(struct sockaddr_in))) == SOCKET_ERROR)
        {
            throw std::runtime_error{"connect fail\n"};
        }
    }

    void send(std::string const& _data)
    {
        size_t offset{0};
        while (offset < _data.size())
        {
            int ret{::send(m_socket, _data.c_str() + offset, _data.size() - offset, 0)};
            if (ret == SOCKET_ERROR)
            {
                throw std::runtime_error{"send error\n"};
            }
            offset += ret;
        }
    }
    void recv(std::string& _data)
    {
        _data.clear();
        _data.resize(4096);
        int len{};
        int ret{::recv(m_socket, _data.data(), _data.size(), 0)};
        // 
    }

private:
    Client()
    {
        WSADATA wsaData{};
        m_wsaStart = {WSAStartup(MAKEWORD(2, 2), &wsaData) == 0};

        m_socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (m_socket == INVALID_SOCKET)
        {
            throw std::runtime_error{"create client socket fail\n"};
        }
    }
    ~Client() noexcept
    {
        if (m_wsaStart == true)
        {
            WSACleanup();
        }
        m_wsaStart = false;

        if (m_socket != INVALID_SOCKET)
        {
            ::closesocket(m_socket);
        }
    }

private:
    int m_wsaStart{false};
    SOCKET m_socket{INVALID_SOCKET};
};

int main()
{
    auto& client{Client::getInstance()};
    client.connectToHost("127.0.0.1", 9527);

    while (true)
    {
    }

    return 0;
}
