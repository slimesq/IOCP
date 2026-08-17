#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#include <errhandlingapi.h>
#include <handleapi.h>
#include <minwindef.h>
#include <synchapi.h>
#include <winsock2.h>
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

class Socket
{
    friend class Client;
    Socket(Socket const& _socket) = delete;
    Socket operator=(Socket const _socket) = delete;

public:
    Socket()
    {
        m_socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (m_socket == INVALID_SOCKET)
        {
            std::cerr << "create client socket failed\n";
        }
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
        int offset{0};
        while (offset < static_cast<int>((_data.size())))
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

class RuntimeSock
{
public:
    RuntimeSock()
    {
        WSADATA wsaData{};
        m_wsaStart = {WSAStartup(MAKEWORD(2, 2), &wsaData) == 0};
        if (!m_wsaStart)
        {
            std::cerr << "client WSAStartup fialid";
        }
    }
    ~RuntimeSock() noexcept
    {
        if (m_wsaStart)
        {
            WSACleanup();
        }
        m_wsaStart = false;
    }

private:
    bool m_wsaStart{false};
};

class Client
{
public:
    static Client& getInstance()
    {
        static Client client{};
        return client;
    }

    void connectToHost(std::string const& _ip, short _port)
    {
        struct sockaddr_in sockAddr{};
        sockAddr.sin_family = AF_INET;
        sockAddr.sin_port = ::htons(_port);
        ::InetPtonA(AF_INET, _ip.c_str(), &sockAddr.sin_addr);
        if (::connect(m_socket.m_socket,
                      reinterpret_cast<struct sockaddr*>(&sockAddr),
                      static_cast<int>(sizeof(struct sockaddr_in))) == SOCKET_ERROR)
        {
            std::cerr << "connected server failed " << std::endl;
        }
    }

    Socket& getSocket() noexcept
    {
        return m_socket;
    }

private:
    Client() = default;
    ~Client() = default;

private:
    RuntimeSock m_runtimeSock;
    Socket m_socket{};
};

int main()
{
    auto& client{Client::getInstance()};
    client.connectToHost("127.0.0.1", 9527);

    std::string buf{};
    while (true)
    {
        std::cout << "请输入数据:";
        std::cin >> buf;
        client.getSocket().send(buf);
        buf.clear();
        if (!client.getSocket().recv(buf))
        {
            return 0;
        }

        std::cout << "收到的数据为:" << buf << std::endl;
    }

    return 0;
}
