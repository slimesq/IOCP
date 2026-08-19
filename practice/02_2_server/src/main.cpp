#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#include <errhandlingapi.h>
#include <handleapi.h>
#include <minwindef.h>
#include <synchapi.h>
#include <memory>
#include <regex>
#include <stdexcept>
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <WinSock2.h>
#include <WS2tcpip.h>
#include <Windows.h>

#include <iostream>
#include <vector>

class Socket
{
    Socket(Socket const& _socket) = delete;
    Socket operator=(Socket const _socket) = delete;

public:
    Socket()
    {
        m_socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (m_socket == INVALID_SOCKET)
        {
            throw std::runtime_error{"create client socket fail\n"};
        }
    }
    Socket(SOCKET&& _socket) : m_socket{_socket}
    {
    }
    Socket(SOCKET& _socket) : m_socket{_socket}
    {
    }

    ~Socket() noexcept
    {
        if (m_socket != INVALID_SOCKET)
        {
            ::closesocket(m_socket);
        }
    }

    void send(std::string const& _data)
    {
        size_t offset{0};
        while (offset < _data.size())
        {
            int ret{::send(
                m_socket, _data.c_str() + offset, static_cast<int>(_data.size()) - offset, 0)};
            if (ret == SOCKET_ERROR)
            {
                std::cout << "send error\n";
                return;
            }
            offset += ret;
        }
    }
    bool recv(std::string& _data)
    {
        char buffer[4096]{};
        int ret{::recv(m_socket, buffer, sizeof(buffer), 0)};

        if (ret == 0)
        {
            std::cout << "recv end\n";
            return false;
        }
        else if (ret == SOCKET_ERROR)
        {
            std::cout << "recv error\n";
            return false;
        }
        // For the time being, consider recv as capable of receiving all the data.
        _data.assign(buffer, static_cast<size_t>(ret));
        return true;
    }

private:
    SOCKET m_socket{INVALID_SOCKET};
};

class Server
{
public:
    Server()
    {
        WSADATA wsaData{};
        m_wsaStart = {WSAStartup(MAKEWORD(2, 2), &wsaData) == 0};
        if (!m_wsaStart)
        {
            std::cerr << "The server fialid in WSAStartup\n";
        }
        m_listenSocket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (m_listenSocket == INVALID_SOCKET)
        {
            std::cerr << "The server failed to create a listening socket\n";
        }
    }

    void start(std::string const& _ip, short _port)
    {
        struct sockaddr_in sockaddr{};
        sockaddr.sin_family = AF_INET;
        sockaddr.sin_port = ::htons(_port);
        ::InetPtonA(AF_INET, _ip.c_str(), &sockaddr.sin_addr);
        // bind
        int ret{::bind(
            m_listenSocket, reinterpret_cast<struct sockaddr*>(&sockaddr), sizeof(sockaddr))};
        if (ret == SOCKET_ERROR)
        {
            std::cerr << "The server failed to bind the sockaddr\n";
        }
        // listen
        ret = ::listen(m_listenSocket, 5);
        if (ret == SOCKET_ERROR)
        {
            std::cerr << "The server failed to listen the sockaddr\n";
        }
    }

    std::shared_ptr<Socket> accept()
    {
        struct sockaddr_in sockaddr{};
        int len{sizeof(sockaddr)};
        auto client{::accept(m_listenSocket, reinterpret_cast<struct sockaddr*>(&sockaddr), &len)};
        if (client == INVALID_SOCKET)
        {
            std::cerr << "accept fialed\n";
        }
        auto autoPtr{std::make_shared<Socket>(client)};
        m_clientSockets.push_back(autoPtr);
        char buffer[1024];
        ::InetNtopA(AF_INET, &sockaddr.sin_addr, buffer, sizeof(buffer));
        std::cout << "client ip:" << buffer << ", port:" << ::ntohs(sockaddr.sin_port) << std::endl;
        return autoPtr;
    };

    ~Server() noexcept
    {
        m_clientSockets.clear();
        if (m_listenSocket != INVALID_SOCKET)
        {
            closesocket(m_listenSocket);
        }
        if (m_wsaStart)
        {
            WSACleanup();
        }
        m_wsaStart = false;
    }

private:
    bool m_wsaStart{false};
    SOCKET m_listenSocket{INVALID_SOCKET};
    std::vector<std::shared_ptr<Socket>> m_clientSockets{};
};

int main()
{
    Server server{};
    server.start("0.0.0.0", 9527);
    auto clientPtr{server.accept()};

    std::string buf{};
    while (clientPtr->recv(buf))
    {
        clientPtr->send(buf);
        buf.clear();
    }

    return 0;
}
