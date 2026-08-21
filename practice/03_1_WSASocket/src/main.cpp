#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#include <errhandlingapi.h>
#include <handleapi.h>
#include <ioapiset.h>
#include <minwinbase.h>
#include <minwindef.h>
#include <synchapi.h>
#include <winuser.h>
#include <filesystem>
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
#include <array>

class ReceiveOperation final
{
public:
    ReceiveOperation() : m_event{WSACreateEvent()}
    {
        this->m_overlapped.hEvent = m_event;
        this->m_nativeBuffer.buf = this->m_storage.data();
        this->m_nativeBuffer.len = static_cast<ULONG>(this->m_storage.size());
    }
    ~ReceiveOperation()
    {
        if (this->m_event != WSA_INVALID_EVENT)
        {
            WSACloseEvent(this->m_event);
        }
    }
    ReceiveOperation(ReceiveOperation const&) = delete;
    ReceiveOperation(ReceiveOperation&&) = delete;
    ReceiveOperation& operator=(ReceiveOperation const&) = delete;
    ReceiveOperation& operator=(ReceiveOperation&&) = delete;

    [[nodiscard]] bool isValid() const noexcept
    {
        return this->m_event != WSA_INVALID_EVENT;
    }

    [[nodiscard]] OVERLAPPED* overlapped() noexcept
    {
        return &(this->m_overlapped);
    }

    [[nodiscard]] WSABUF* nativeBuffer() noexcept
    {
        return &(this->m_nativeBuffer);
    }

    [[nodiscard]] WSAEVENT event() const noexcept
    {
        return this->m_event;
    }

    [[nodiscard]] char const* data() const noexcept
    {
        return this->m_storage.data();
    }

private:
    OVERLAPPED m_overlapped{};
    std::array<char, 8192> m_storage{};
    WSABUF m_nativeBuffer{};
    WSAEVENT m_event{WSA_INVALID_EVENT};
};

enum class ReceiveResultKind
{
    Data,
    PeerClosed,
    Failed
};

struct ReceiveResult
{
    ReceiveResultKind kind{ReceiveResultKind::Failed};
    DWORD transferredBytes{0};
    int error{0};
};

[[nodiscard]] ReceiveResult makeSuccessfulReceive(DWORD _transferredBytes) noexcept
{
    return {_transferredBytes == 0 ? ReceiveResultKind::PeerClosed : ReceiveResultKind::Data,
            _transferredBytes,
            0};
}

[[nodiscard]] ReceiveResult receiveOnceWhthEvent(SOCKET _socket, ReceiveOperation& _operation)
{
    //1.WSARecv
    DWORD immediateBytes{0};
    DWORD flags{0};
    int const result{WSARecv(_socket,
                             _operation.nativeBuffer(),
                             1,
                             &immediateBytes,
                             &flags,
                             _operation.overlapped(),
                             nullptr)};
    if (result == 0)
    {
        return makeSuccessfulReceive(immediateBytes);
    }
    int const submitError{WSAGetLastError()};
    if (submitError != WSA_IO_PENDING)
    {
        return {ReceiveResultKind::Failed, 0, submitError};
    }
    //2.WSAWaitForMultipleEvents
    WSAEVENT eventHandle{_operation.event()};
    const DWORD waitResult{WSAWaitForMultipleEvents(1, &eventHandle, TRUE, 10'000, FALSE)};
    if (waitResult != WSA_WAIT_EVENT_0)
    {
        CancelIoEx(reinterpret_cast<HANDLE>(_socket), nullptr);
    }
    //3.WSAGetOverlappedResult
    DWORD transferredBytes{0};
    const BOOL completed{
        WSAGetOverlappedResult(_socket, _operation.overlapped(), &transferredBytes, TRUE, &flags)};
    if (completed == FALSE)
    {
        return {ReceiveResultKind::Failed, 0, WSAGetLastError()};
    }
    return makeSuccessfulReceive(transferredBytes);
}

int main()
{
    SOCKET socketHandle{WSASocketW(AF_INET,
                                   SOCK_STREAM,
                                   IPPROTO_TCP,
                                   nullptr /*自动选择协议提供者*/,
                                   0 /*不使用 socket group*/,
                                   WSA_FLAG_OVERLAPPED)};
    if (socketHandle == INVALID_SOCKET)
    {
        int const error{WSAGetLastError()};
        std::cerr << "WSASocketW failed:" << error << std::endl;
    }

    DWORD flags{0};
    DWORD immediateBytes{0};
    OVERLAPPED overLapped{};
    char buffer[4096];
    WSABUF wsaBuf{};
    wsaBuf.buf = buffer;
    wsaBuf.len = 4096;
    int const result{
        WSARecv(socketHandle, &wsaBuf, 1, &immediateBytes, &flags, &overLapped, nullptr)};
    if (result == 0)
    {
        std::cerr << "immediately returned\n";
    }
    else if (result == SOCKET_ERROR)
    {
        int const error{WSAGetLastError()};
        if (error == WSA_IO_PENDING)
        {
            std::cerr << "WSARecv WSA_IO_PENDING\n";
        }
        else
        {
            std::cerr << "WSARecv error\n";
        }
    }
    return 0;
}
