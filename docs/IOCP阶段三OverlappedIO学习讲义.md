# IOCP 阶段三：Overlapped I/O 学习讲义

> 贯穿项目：`D:\CodeRepository\claude\remote_control`  
> 适用环境：Windows、MSVC、C++17  
> 本讲义只讲阶段三需要掌握的 Overlapped I/O。代码均为关键示例片段，不要求在当前项目中创建或运行额外示例工程。

## 1. 阶段目标

完成本阶段后，应能够独立回答：

1. Overlapped I/O 与同步 I/O 的控制流有什么不同？
2. `OVERLAPPED`、`WSABUF` 和真实 buffer 分别由谁拥有？
3. `WSARecv()` 返回 `0`、`WSA_IO_PENDING` 和其他错误分别表示什么？
4. 为什么异步 API 返回后通常不能释放操作对象？
5. 为什么一个在途操作必须独占一个 `OVERLAPPED`？
6. 如何使用事件对象取得一次 receive 的最终结果？
7. `CancelIoEx()` 返回后为什么仍要等待最终完成？
8. 项目的 `IoOperation` 如何保证 operation、buffer 和 connection 存活？
9. pending 计数为什么必须在调用异步 API 前增加？

建议投入 6～10 小时。阶段四学习 IOCP 前，必须先理解本讲义中的生命周期。

## 2. 本阶段的边界

### 2.1 现在学习

- `WSA_FLAG_OVERLAPPED`。
- `OVERLAPPED`。
- `WSABUF`。
- `WSARecv()`、`WSASend()`。
- 立即完成、pending、同步投递失败。
- 事件通知与 `WSAGetOverlappedResult()`。
- 取消后的最终完成。
- operation 与 buffer 生命周期。

### 2.2 暂时不学习

- `CreateIoCompletionPort()`。
- `GetQueuedCompletionStatus()`。
- completion worker 调度。
- `PostQueuedCompletionStatus()`。
- `AcceptEx()`。

本阶段使用事件对象理解完成过程。事件不是高并发服务端的最终方案，但它能把一次 Overlapped I/O 的生命周期完整展示出来。

---

## 3. 从同步 I/O 切换到 Overlapped I/O

### 3.1 同步 receive

```text
创建 buffer
  → 调用 recv()
  → 当前线程阻塞
  → recv() 返回
  → 使用 buffer
  → 离开作用域
```

调用、等待和结果处理都发生在同一个栈帧中，所以 buffer 生命周期通常不容易出错。

### 3.2 Overlapped receive

```text
创建 operation 和 buffer
  → 调用 WSARecv() 提交操作
  → WSARecv() 返回
  → Windows 可能继续使用 operation 和 buffer
  → 未来产生最终完成结果
  → 应用取得结果
  → 最后释放 operation 和 buffer
```

核心变化是：

> “提交函数返回”与“操作真正结束”不再是同一个时刻。

### 3.3 Windows 借用的是地址

调用 `WSARecv()` 时，应用向 Windows 提供：

```text
OVERLAPPED*  ──→ 标识一次具体操作
WSABUF*      ──→ 描述 buffer 地址和长度
WSABUF.buf   ──→ 指向真实存储空间
```

Windows 不理解 `std::vector`、`QByteArray`、`std::unique_ptr` 或 C++ 作用域。它只会在操作期间使用这些地址。

因此，在最终完成前必须保证：

1. `OVERLAPPED` 仍然存在且地址不变。
2. `WSABUF` 指向的内存仍然存在且地址不变。
3. buffer 没有被扩容、移动或销毁。
4. 应用仍能在完成时找到 operation 并正确释放。

---

## 4. 三个核心对象

### 4.1 `OVERLAPPED`

`OVERLAPPED` 标识一次异步操作。

```cpp
OVERLAPPED overlapped{};
```

必须遵守：

- 创建时零初始化。
- 一个在途操作独占一个实例。
- 在操作最终完成前不能销毁。
- 在操作最终完成前不能重新清零或复用。
- `Internal` 和 `InternalHigh` 由系统使用，不要自行修改。
- socket receive/send 通常不使用 `Offset` 和 `OffsetHigh`。
- 事件模式下将 `hEvent` 设置为有效事件。

### 4.2 `WSABUF`

可以把 `WSABUF` 理解为：

```cpp
struct WSABUF
{
    ULONG len;
    CHAR* buf;
};
```

它只是视图，不拥有内存：

```cpp
std::array<char, 8192> storage{};

WSABUF nativeBuffer{};
nativeBuffer.buf = storage.data();
nativeBuffer.len = static_cast<ULONG>(storage.size());
```

所有权关系：

```text
std::array owns bytes
WSABUF observes bytes
```

销毁 `WSABUF` 不会释放 buffer；保留 `WSABUF` 也不会让 buffer 自动存活。

### 4.3 真实 buffer

buffer 可以由以下对象拥有：

- `std::array<char, N>`。
- `std::vector<char>`。
- `QByteArray`。
- operation 中的固定内存块。

如果使用可扩容容器，在操作在途期间不能执行可能改变地址的操作：

```cpp
std::vector<char> storage;
storage.resize(8192);

WSABUF nativeBuffer{};
nativeBuffer.buf = storage.data();

// WSARecv 已进入 pending 后：
storage.resize(16384);  // 错误：可能重分配，nativeBuffer.buf 仍指向旧地址。
```

---

## 5. 操作生命周期状态机

```text
Created
  │
  ├─ 同步投递失败 ─────────────────────→ Finished
  │
  ├─ 立即完成 ─────────────────────────→ Completed → Finished
  │
  └─ WSA_IO_PENDING ─→ InFlight
                         │
                         ├─ 成功完成 ───→ Completed → Finished
                         ├─ 对端关闭 ───→ Completed → Finished
                         ├─ I/O 失败 ───→ Completed → Finished
                         └─ 请求取消 ───→ Cancelling
                                            │
                                            └─ 取消完成 → Finished
```

`CancelIoEx()` 只把操作推进到“请求取消”，不会瞬间推进到 `Finished`。

### 5.1 一个操作一个 `OVERLAPPED`

错误关系：

```text
OVERLAPPED X ─→ Receive A 尚未完成
       同时 └→ Receive B 又使用 X
```

正确关系：

```text
OVERLAPPED A ─→ Receive A
OVERLAPPED B ─→ Receive B
OVERLAPPED C ─→ Send C
```

项目采用更严格的不变量：每条连接最多一个 receive 和一个 send 在途。这是项目设计，不是 Winsock 的强制限制。

---

## 6. 创建支持 Overlapped I/O 的 socket

关键参数是 `WSA_FLAG_OVERLAPPED`：

```cpp
SOCKET socketHandle{WSASocketW(
    AF_INET,
    SOCK_STREAM,
    IPPROTO_TCP,
    nullptr,
    0,
    WSA_FLAG_OVERLAPPED)};

if (socketHandle == INVALID_SOCKET)
{
    int const error{WSAGetLastError()};
    reportWinsockError(error);
}
```

### 6.1 `WSASocketW()` 参数说明

函数原型可以简化理解为：

```cpp
SOCKET WSASocketW(
    int af,
    int type,
    int protocol,
    LPWSAPROTOCOL_INFOW lpProtocolInfo,
    GROUP g,
    DWORD dwFlags);
```

| 参数 | 示例值 | 作用 |
| --- | --- | --- |
| `af` | `AF_INET` | 地址族。`AF_INET` 表示 IPv4，`AF_INET6` 表示 IPv6。 |
| `type` | `SOCK_STREAM` | socket 类型。`SOCK_STREAM` 表示有序的字节流，也就是 TCP 使用的类型。 |
| `protocol` | `IPPROTO_TCP` | 明确指定 TCP 协议。与 `AF_INET + SOCK_STREAM` 配合使用。 |
| `lpProtocolInfo` | `nullptr` | 可选的协议提供者信息。传空表示由 Winsock 根据前三个参数选择提供者。 |
| `g` | `0` | socket group。普通场景不使用分组时传 `0`。 |
| `dwFlags` | `WSA_FLAG_OVERLAPPED` | 创建标志。该标志表示 socket 支持 Overlapped I/O。 |

返回值：

- 成功：返回有效 `SOCKET`。
- 失败：返回 `INVALID_SOCKET`，随后立即调用 `WSAGetLastError()` 取得当前线程的 Winsock 错误码。

### 6.2 错误与关闭函数

| 函数 | 参数 | 返回值与作用 |
| --- | --- | --- |
| `WSAGetLastError()` | 无 | 返回当前调用线程最近一次 Winsock 错误码。应在失败后立即读取，避免被后续 Winsock 调用覆盖。 |
| `GetLastError()` | 无 | 返回当前调用线程最近一次 Win32 错误码。`CancelIoEx()` 失败时使用它，而不是 `WSAGetLastError()`。 |
| `closesocket(s)` | `s` 是要关闭的 `SOCKET` | 成功返回 `0`，失败返回 `SOCKET_ERROR`。关闭会终止 socket 使用，但不能代替在途 operation 的最终回收。 |

错误来源要区分：

- Winsock API 通常使用 `WSAGetLastError()`。
- `CancelIoEx()` 等普通 Win32 API 使用 `GetLastError()`。
- `SOCKET` 使用 `closesocket()`，不要使用 `CloseHandle()`。

---

## 7. `WSARecv()` 的三种提交结果

关键调用：

```cpp
DWORD flags{0};
DWORD immediateBytes{0};

int const result{WSARecv(socketHandle,
                         &nativeBuffer,
                         1,
                         &immediateBytes,
                         &flags,
                         &overlapped,
                         nullptr)};
```

### 7.1 `WSARecv()` 参数说明

函数原型可以简化理解为：

```cpp
int WSARecv(
    SOCKET s,
    LPWSABUF lpBuffers,
    DWORD dwBufferCount,
    LPDWORD lpNumberOfBytesRecvd,
    LPDWORD lpFlags,
    LPWSAOVERLAPPED lpOverlapped,
    LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine);
```

| 参数 | 示例值 | 作用与生命周期要求 |
| --- | --- | --- |
| `s` | `socketHandle` | 已连接且支持 Overlapped I/O 的 socket。必须在提交时有效。 |
| `lpBuffers` | `&nativeBuffer` | `WSABUF` 数组首地址。数组描述接收内存；它指向的真实 buffer 必须存活到最终完成。 |
| `dwBufferCount` | `1` | `lpBuffers` 中的元素数量。示例只有一个 `WSABUF`，所以传 `1`。 |
| `lpNumberOfBytesRecvd` | `&immediateBytes` | 立即完成时接收字节数的输出位置。若返回 `WSA_IO_PENDING`，不能把这里的值当作最终字节数，应从完成结果取得。 |
| `lpFlags` | `&flags` | 输入/输出接收标志。提交时可传入 `MSG_PEEK` 等标志；普通接收初始化为 `0`。最终 flags 可由完成 API 返回。 |
| `lpOverlapped` | `&overlapped` | 标识本次操作的 `OVERLAPPED`。在最终完成前必须保持对象存活、地址稳定且不得复用。 |
| `lpCompletionRoutine` | `nullptr` | 可选 completion routine。事件模式和 IOCP 模式不使用回调时传空。 |

返回值只表示“提交时发生了什么”，最终业务结果仍需结合完成模型判断。

### 7.2 返回 `0`：立即完成

在纯事件模式中：

```text
WSARecv 返回 0
  → 操作已经完成
  → immediateBytes 是完成字节数
  → 当前路径可以处理最终结果
```

进入 IOCP 阶段后要改变处理方式：socket 关联 completion port 后，即使操作立即成功，默认仍会产生 completion packet。项目没有开启“成功时跳过 completion”的模式，所以项目必须让 completion worker 统一收尾，不能在提交线程提前释放 operation。

### 7.3 返回 `SOCKET_ERROR`，错误为 `WSA_IO_PENDING`

```cpp
if (result == SOCKET_ERROR)
{
    int const error{WSAGetLastError()};
    if (error == WSA_IO_PENDING)
    {
        // 正常异步路径，操作已经成功进入在途状态。
    }
}
```

此时不能：

- 销毁或复用 `OVERLAPPED`。
- 销毁、移动或扩容 buffer。
- 把 `WSA_IO_PENDING` 当作连接失败。

### 7.4 返回 `SOCKET_ERROR`，错误不是 `WSA_IO_PENDING`

表示同步投递失败：

```text
操作没有进入在途状态
  → 当前提交路径负责回收 operation
  → 回滚 pending 计数
  → 根据错误决定是否关闭连接
```

### 7.5 判断表

| 返回结果 | 是否成功提交 | 是否需要未来完成路径 | 回收者 |
| --- | --- | --- | --- |
| `0` | 是，且立即完成 | 事件模式不需要；IOCP 默认需要 | 取决于完成模型 |
| `SOCKET_ERROR + WSA_IO_PENDING` | 是 | 需要 | 完成路径 |
| `SOCKET_ERROR + 其他错误` | 否 | 不需要普通完成路径 | 当前提交路径 |

---

## 8. 关键示例：事件版一次 receive

下面只展示 operation、提交、等待和最终结果。省略 `WSAStartup()`、bind、listen、accept 等阶段二已经学习过的内容。

### 8.1 operation 自己拥有事件和 buffer

本节首次使用两个事件 API：

| 函数 | 参数 | 返回值与作用 |
| --- | --- | --- |
| `WSACreateEvent()` | 无 | 创建一个初始为 nonsignaled 的 Winsock event。成功返回 `WSAEVENT`，失败返回 `WSA_INVALID_EVENT`。 |
| `WSACloseEvent(hEvent)` | `hEvent` 是 `WSACreateEvent()` 返回的事件 | 成功返回 `TRUE`，失败返回 `FALSE`。只有确认没有在途操作再使用该事件后才能关闭。 |

`OVERLAPPED::hEvent` 保存该事件。Windows 完成操作后设置事件，等待线程据此进入最终结果查询。

```cpp
class ReceiveOperation final
{
public:
    ReceiveOperation()
        : m_event{WSACreateEvent()}
    {
        this->m_overlapped.hEvent = this->m_event;
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
        return &this->m_overlapped;
    }

    [[nodiscard]] WSABUF* nativeBuffer() noexcept
    {
        return &this->m_nativeBuffer;
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
```

设计要点：

1. `OVERLAPPED`、buffer、`WSABUF` 和 event 具有同一生命周期。
2. 类型禁止复制和移动，避免内部指针指向旧对象。
3. `WSABUF` 指向 operation 自己拥有的 `m_storage`。
4. 每次 receive 创建一个新的 `ReceiveOperation`。

### 8.2 提交并取得最终结果

本节使用三个完成与取消 API。

#### `WSAWaitForMultipleEvents()`

```cpp
DWORD WSAWaitForMultipleEvents(
    DWORD cEvents,
    const WSAEVENT* lphEvents,
    BOOL fWaitAll,
    DWORD dwTimeout,
    BOOL fAlertable);
```

| 参数 | 示例值 | 作用 |
| --- | --- | --- |
| `cEvents` | `1` | `lphEvents` 数组中的事件数量。 |
| `lphEvents` | `&eventHandle` | 要等待的事件数组首地址。数组在调用期间必须有效。 |
| `fWaitAll` | `TRUE` | `TRUE` 表示等待全部事件；`FALSE` 表示任一事件。这里只有一个事件，两者结果等价。 |
| `dwTimeout` | `10'000` | 等待毫秒数。也可使用 `WSA_INFINITE` 表示无限等待。 |
| `fAlertable` | `FALSE` | 是否进行 alertable wait。示例不使用 completion routine，因此传 `FALSE`。 |

主要返回值：

- `WSA_WAIT_EVENT_0 + index`：对应事件满足等待条件。
- `WSA_WAIT_TIMEOUT`：在超时时间内没有满足条件。
- `WSA_WAIT_FAILED`：等待失败，调用 `WSAGetLastError()` 取得错误码。

#### `CancelIoEx()`

```cpp
BOOL CancelIoEx(
    HANDLE hFile,
    LPOVERLAPPED lpOverlapped);
```

| 参数 | 示例值 | 作用 |
| --- | --- | --- |
| `hFile` | `reinterpret_cast<HANDLE>(_socket)` | 发起 I/O 的 handle。socket 在这里转换为 Win32 `HANDLE` 视图，仅用于该 API。 |
| `lpOverlapped` | `_operation.overlapped()` | 指定要取消的那一次操作。传空表示请求取消该 handle 上由当前进程发起的全部匹配操作，本讲义不使用该方式。 |

返回非零表示取消请求已发出；返回 `FALSE` 时调用 `GetLastError()`。无论返回值如何，都不能据此直接释放 operation，仍需取得最终完成结果。

#### `WSAGetOverlappedResult()`

```cpp
BOOL WSAGetOverlappedResult(
    SOCKET s,
    LPWSAOVERLAPPED lpOverlapped,
    LPDWORD lpcbTransfer,
    BOOL fWait,
    LPDWORD lpdwFlags);
```

| 参数 | 示例值 | 作用 |
| --- | --- | --- |
| `s` | `_socket` | 提交该 operation 的 socket。 |
| `lpOverlapped` | `_operation.overlapped()` | 要查询的具体操作，必须与提交时使用同一地址。 |
| `lpcbTransfer` | `&transferredBytes` | 输出最终传输字节数。只有函数成功时才能按成功结果使用。 |
| `fWait` | `TRUE` | `TRUE` 表示操作未完成时继续等待；`FALSE` 表示立即查询。教学示例使用 `TRUE` 保证离开前操作已结束。 |
| `lpdwFlags` | `&flags` | 输出本次操作的最终 flags。 |

成功返回 `TRUE`；失败返回 `FALSE`，调用 `WSAGetLastError()` 取得该 operation 的最终错误。

#### 自定义示例函数参数

| 函数 | 参数 | 作用 |
| --- | --- | --- |
| `makeSuccessfulReceive(_transferredBytes)` | `_transferredBytes` 是最终接收字节数 | 将 `0` 分类为 `PeerClosed`，将大于 `0` 分类为 `Data`。 |
| `receiveOnceWithEvent(_socket, _operation)` | `_socket` 是已连接 socket；`_operation` 是尚未提交的新 operation | 提交一次 receive，必要时等待或取消，并返回最终分类、字节数和错误码。调用期间 `_operation` 必须持续存活。 |

```cpp
enum class ReceiveResultKind
{
    Data,
    PeerClosed,
    Failed,
};

struct ReceiveResult final
{
    ReceiveResultKind kind{ReceiveResultKind::Failed};
    DWORD transferredBytes{0};
    int error{0};
};

[[nodiscard]] ReceiveResult makeSuccessfulReceive(DWORD _transferredBytes) noexcept
{
    return {_transferredBytes == 0 ? ReceiveResultKind::PeerClosed
                                   : ReceiveResultKind::Data,
            _transferredBytes,
            0};
}

[[nodiscard]] ReceiveResult receiveOnceWithEvent(SOCKET _socket,
                                                  ReceiveOperation& _operation)
{
    DWORD flags{0};
    DWORD immediateBytes{0};

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

    WSAEVENT eventHandle{_operation.event()};
    DWORD const waitResult{
        WSAWaitForMultipleEvents(1, &eventHandle, TRUE, 10'000, FALSE)};

    if (waitResult != WSA_WAIT_EVENT_0)
    {
        static_cast<void>(CancelIoEx(
            reinterpret_cast<HANDLE>(_socket),
            _operation.overlapped()));
    }

    DWORD transferredBytes{0};
    BOOL const completed{WSAGetOverlappedResult(_socket,
                                                _operation.overlapped(),
                                                &transferredBytes,
                                                TRUE,
                                                &flags)};
    if (completed == FALSE)
    {
        return {ReceiveResultKind::Failed, 0, WSAGetLastError()};
    }

    return makeSuccessfulReceive(transferredBytes);
}
```

这段代码最重要的不是“等待十秒”，而是以下顺序：

```text
提交成功进入 pending
  → 等待 event
  → 超时或等待异常时请求取消
  → 无论取消请求结果如何，都调用 WSAGetOverlappedResult(..., TRUE, ...)
  → 取得最终结果后才允许 ReceiveOperation 析构
```

`WSAGetOverlappedResult()` 的 `fWait` 使用 `TRUE`，确保函数返回时操作已经进入最终完成状态。教学代码宁可继续等待，也不能在系统可能仍引用 operation 时提前离开作用域。

### 8.3 使用完成字节数

```cpp
ReceiveOperation operation;
ReceiveResult const result{receiveOnceWithEvent(socketHandle, operation)};

if (result.kind == ReceiveResultKind::Data)
{
    processBytes(operation.data(), result.transferredBytes);
}
else if (result.kind == ReceiveResultKind::PeerClosed)
{
    beginConnectionClose();
}
else
{
    reportWinsockError(result.error);
    beginConnectionClose();
}
```

上述业务辅助函数只是示意：

| 函数 | 参数 | 作用 |
| --- | --- | --- |
| `processBytes(_data, _size)` | `_data` 是有效字节首地址；`_size` 是 `transferredBytes` | 把本次收到的有效范围追加到协议接收缓冲区。 |
| `reportWinsockError(_error)` | `_error` 是 Winsock 错误码 | 记录错误；不要在函数内部再次读取可能已被覆盖的 last error。 |
| `beginConnectionClose()` | 无 | 进入幂等连接关闭流程。 |

只能处理 `[0, transferredBytes)`：

```cpp
// 错误：把整个容量都当作有效数据。
processBytes(operation.data(), 8192);

// 正确：只使用本次完成范围。
processBytes(operation.data(), result.transferredBytes);
```

一次 receive completion 只表示“一批 TCP 字节”，仍然可能是半包、一个完整 Packet 或多个 Packet。

---

## 9. 为什么普通局部变量经常出错

错误示例：

```cpp
void postReceiveAndReturn(SOCKET _socket)
{
    std::array<char, 8192> storage{};
    WSABUF nativeBuffer{
        static_cast<ULONG>(storage.size()), storage.data()};
    OVERLAPPED overlapped{};

    DWORD flags{0};
    int const result{WSARecv(_socket,
                             &nativeBuffer,
                             1,
                             nullptr,
                             &flags,
                             &overlapped,
                             nullptr)};

    static_cast<void>(result);
}  // 函数返回后，三个对象全部失效。
```

如果 `WSARecv()` 返回 `WSA_IO_PENDING`，Windows 仍可能访问：

- `&overlapped`。
- `nativeBuffer.buf`。
- `storage.data()`。

函数返回后这些地址都不再有效，属于 use-after-free。未立即崩溃不代表代码正确。

栈对象并非绝对不能用于 Overlapped I/O。只有当作用域明确等待最终完成后才离开时，才是安全的：

```text
创建栈对象
  → 提交
  → 在同一作用域等待最终完成
  → 处理结果
  → 离开作用域
```

真正的异步架构不会阻塞在原作用域，因此通常使用地址稳定的堆 operation。

---

## 10. 取消不是立即结束

错误理解：

```text
CancelIoEx()
  → Windows 已经忘记 operation
  → 立即 delete operation
```

正确理解：

```text
CancelIoEx()
  → 发出取消请求
  → 原操作仍需进入最终完成状态
  → 事件、completion routine 或 IOCP 返回最终结果
  → 应用消费最终结果
  → 最后释放 operation
```

### 10.1 `ERROR_NOT_FOUND` 竞态

`CancelIoEx()` 返回 `FALSE` 且 `GetLastError()==ERROR_NOT_FOUND`，可能表示目标操作刚好已经完成，取消请求没有找到仍在途的操作。

这不表示可以跳过完成协议。应用仍要取得该操作的最终结果。

### 10.2 常见取消结果

取消 operation 的最终错误通常表示操作被中止。取消 completion 仍是一次需要处理的完成结果，不能因为它“不包含业务数据”就忽略 operation 回收。

---

## 11. receive 完成结果分类

| 完成状态 | 字节数 | 含义 | 处理 |
| --- | ---: | --- | --- |
| 成功 | `> 0` | 收到 TCP 字节 | 追加到连接接收缓冲区 |
| 成功 | `0` | 对端正常关闭发送方向 | 进入连接关闭流程 |
| 失败 | 不使用 | 网络或 socket 错误 | 记录错误并幂等关闭 |
| 取消 | 不使用 | 应用请求结束操作 | 回收 operation，按关闭状态处理 |

不要把成功且零字节当作“本次暂时没数据”。对于 byte-stream socket，它表示对端正常关闭。

---

## 12. Overlapped send 的关键点

`WSASend()` 的典型调用：

```cpp
DWORD immediateBytes{0};

int const result{WSASend(socketHandle,
                         &operation.nativeBuffer,
                         1,
                         &immediateBytes,
                         0,
                         &operation,
                         nullptr)};
```

函数原型可以简化理解为：

```cpp
int WSASend(
    SOCKET s,
    LPWSABUF lpBuffers,
    DWORD dwBufferCount,
    LPDWORD lpNumberOfBytesSent,
    DWORD dwFlags,
    LPWSAOVERLAPPED lpOverlapped,
    LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine);
```

| 参数 | 示例值 | 作用与生命周期要求 |
| --- | --- | --- |
| `s` | `socketHandle` | 已连接且支持 Overlapped I/O 的 socket。 |
| `lpBuffers` | `&operation.nativeBuffer` | 待发送 `WSABUF` 数组首地址。它指向的数据在最终完成前不能修改、移动或销毁。 |
| `dwBufferCount` | `1` | `WSABUF` 元素数量。 |
| `lpNumberOfBytesSent` | `&immediateBytes` | 立即完成时输出发送字节数；pending 路径应从最终完成结果取得字节数。 |
| `dwFlags` | `0` | 发送标志。普通 TCP 发送使用 `0`。 |
| `lpOverlapped` | `&operation` | 标识本次 send 的 `OVERLAPPED`。示例中 `SendOperation` 继承 `OVERLAPPED`。 |
| `lpCompletionRoutine` | `nullptr` | 可选 completion routine。事件或 IOCP 模式不使用回调时传空。 |

返回 `0`、`SOCKET_ERROR + WSA_IO_PENDING` 和其他错误的含义与 `WSARecv()` 相同。

`WSASend()` 使用同一套提交规则：

```text
立即完成
WSA_IO_PENDING
同步投递失败
```

### 12.1 send operation 必须拥有发送数据

错误示例：

```cpp
std::string response{"response"};
postSend(socketHandle, response.data(), response.size());
response.clear();  // operation 可能仍在使用原地址。
```

这里的 `postSend(_socket, _data, _size)` 是错误示意中的辅助函数：

| 参数 | 作用 |
| --- | --- |
| `_socket` | 接收发送操作的连接 socket。 |
| `_data` | 待发送数据首地址；错误示例没有保证该地址活到 completion。 |
| `_size` | 待发送字节数。 |

推荐结构：

```cpp
struct SendOperation final : OVERLAPPED
{
    explicit SendOperation(std::vector<char> _bytes)
        : OVERLAPPED{}, sendBytes{std::move(_bytes)}
    {
        this->refreshBuffer();
    }

    void refreshBuffer() noexcept
    {
        this->nativeBuffer.buf = this->sendBytes.data() + this->sendOffset;
        this->nativeBuffer.len = static_cast<ULONG>(
            this->sendBytes.size() - this->sendOffset);
    }

    std::vector<char> sendBytes;
    std::size_t sendOffset{0};
    WSABUF nativeBuffer{};
};
```

`SendOperation(_bytes)` 的 `_bytes` 按值接收完整发送数据，再移动到 `sendBytes`，从而把数据所有权交给 operation。`refreshBuffer()` 没有形参，它根据成员 `sendOffset` 重新计算剩余数据的地址和长度。

`nativeBuffer` 只观察 `sendBytes`，真正的数据由 operation 拥有。

### 12.2 部分发送

一次 send completion 不保证覆盖全部数据：

```cpp
operation->sendOffset += transferredBytes;

if (operation->sendOffset < operation->sendBytes.size())
{
    operation->refreshBuffer();
    postSendAgain(std::move(operation));
    return;
}

finishCurrentSend();
```

示意辅助函数：

| 函数 | 参数 | 作用 |
| --- | --- | --- |
| `postSendAgain(_operation)` | `_operation` 是当前 send operation 的唯一所有权 | 把尚未发送完的 operation 再次提交；调用后当前作用域不再拥有它。 |
| `finishCurrentSend()` | 无 | 标记当前发送项完成，并根据发送队列决定是否继续下一项。 |

推演：

```text
sendBytes.size = 1000
sendOffset = 0

第一次完成 400
  → sendOffset = 400
  → 重新投递 [400, 1000)

第二次完成 600
  → sendOffset = 1000
  → 当前发送项完成
```

只有前一次 completion 已经被消费后，才能重新使用该 operation 投递剩余部分。

---

## 13. 映射到项目 `IoOperation`

重点阅读：

```text
server_transport/internal/RemoteControlTransportImpl.h
server_transport/src/RemoteControlTransportRuntime.cpp
server_transport/src/RemoteControlTransport.cpp
```

### 13.1 项目结构

```cpp
struct IoOperation final : OVERLAPPED
{
    IoOperationType type{IoOperationType::Receive};
    std::shared_ptr<ConnectionContext> connection;
    SOCKET acceptSocket{INVALID_SOCKET};
    QByteArray storage;
    QByteArray sendBytes;
    int sendOffset{0};
    WSABUF nativeBuffer{};
};
```

成员职责：

| 成员 | 作用 | 所有权 |
| --- | --- | --- |
| `OVERLAPPED` 基类 | 原生操作状态 | `IoOperation` 自身 |
| `type` | completion 分发类型 | 值成员 |
| `connection` | 保证连接上下文存活 | 共享所有权 |
| `acceptSocket` | `AcceptEx` 预创建 socket | accept operation 暂时持有 |
| `storage` | receive/accept buffer | operation 拥有 |
| `sendBytes` | 完整发送数据 | operation 拥有 |
| `sendOffset` | 部分发送进度 | 值成员 |
| `nativeBuffer` | 指向 storage 或 sendBytes | 非 owning 视图 |

### 13.2 receive operation 构造

项目关键代码：

```cpp
IoOperation::IoOperation(IoOperationType _type,
                         std::shared_ptr<ConnectionContext> _connection,
                         int _bufferSize)
    : OVERLAPPED{},
      type{_type},
      connection{std::move(_connection)},
      storage{_bufferSize, Qt::Uninitialized}
{
    this->nativeBuffer.buf = this->storage.data();
    this->nativeBuffer.len = static_cast<ULONG>(this->storage.size());
}
```

构造函数参数：

| 参数 | 作用 | 约束 |
| --- | --- | --- |
| `_type` | 指定操作类型，例如 `Receive` 或 `Accept`，completion 时据此分发。 | 必须与后续调用的原生 API 相匹配。 |
| `_connection` | 与该 operation 关联的连接上下文。移动到成员后，保证 connection 活到最终 completion。 | accept 尚未建立连接时可以为空；receive 必须提供对应连接。 |
| `_bufferSize` | 为 `storage` 分配的字节数。 | 必须大于零，并且能够安全转换为 `ULONG` 供 `WSABUF::len` 使用。 |

这是构造函数，没有返回值。构造完成后，`nativeBuffer` 已指向 `storage`，但 operation 尚未提交。

所有权图：

```text
IoOperation
  ├─ owns OVERLAPPED
  ├─ owns QByteArray storage
  ├─ WSABUF nativeBuffer ─→ storage.data()
  └─ owns shared_ptr<ConnectionContext>
                         └─ keeps connection alive
```

### 13.3 为什么 operation 持有 connection

关闭可能发生在 receive 仍在途时：

```text
ConnectionRegistry 移除 connection
  → socket 关闭或取消
  → receive 的最终 completion 尚未返回
```

如果 operation 不持有 connection，注册表移除后 connection 可能析构，而 completion handler 仍需要读取连接状态。

`shared_ptr` 只保证连接对象存活，不保证 socket 仍然可用。是否允许新 I/O 仍由状态机、`socketMutex` 和 socket 值共同判断。

---

## 14. `postReceive()` 的所有权移交

项目函数契约可以概括为：

```cpp
bool postReceive(
    std::shared_ptr<ConnectionContext> const& _connection);
```

| 参数 | 作用 | 生命周期要求 |
| --- | --- | --- |
| `_connection` | 指定要投递下一次 receive 的连接。函数会把它复制到新建的 `IoOperation::connection`。 | 调用时必须指向有效连接；复制出的 `shared_ptr` 会让连接活到 completion。 |

返回值：

- `true`：receive 已成功进入完成协议，包括立即成功和 `WSA_IO_PENDING`。
- `false`：服务正在停止、连接已终止、pending 无法注册或同步投递失败。

主要副作用：创建 receive operation、增加 pending、调用 `WSARecv()`；同步投递失败时还会回滚 pending 并触发连接关闭。

项目流程：

```text
1. make_unique<IoOperation>
2. local unique_ptr 拥有 operation
3. 锁定 connection.socketMutex
4. 检查 stopping、terminal、INVALID_SOCKET
5. tryBeginOperation() 增加 pending
6. unique_ptr.release() 得到 raw pointer
7. 调用 WSARecv(raw pointer)
8. 同步投递失败：reset(raw pointer) 收回并 finishOperation()
9. 成功或 pending：等待 completion worker 重新接管
```

关键代码形态：

```cpp
auto operation{
    std::make_unique<IoOperation>(IoOperationType::Receive,
                                  connection,
                                  ReceiveChunkSize)};

if (!this->tryBeginOperation())
{
    return false;
}

IoOperation* const operationPointer{operation.release()};
int const result{WSARecv(connection->socket,
                         &operationPointer->nativeBuffer,
                         1,
                         &bytesReceived,
                         &flags,
                         operationPointer,
                         nullptr)};

if (result == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING)
{
    operation.reset(operationPointer);
    this->finishOperation();
}
```

### 14.1 `release()` 的准确含义

`release()` 不代表 Windows 获得 C++ 所有权。Windows 不会执行 `delete operationPointer`。

它表示：

```text
local unique_ptr 暂时放弃自动析构
  → 应用把回收责任交给“完成协议”
  → completion worker 将 raw pointer 重新包装为 unique_ptr
```

### 14.2 同步投递失败为什么 `reset()`

同步投递失败时不会进入正常 completion 回收路径，因此当前线程必须：

1. 用 `reset(operationPointer)` 收回所有权。
2. 回滚 pending。
3. 进入连接错误处理。

### 14.3 立即成功为什么不 `reset()`

项目 socket 已关联 IOCP。默认情况下，立即成功仍会投递 completion packet：

```text
WSARecv 返回 0
  → operation 已完成
  → completion packet 仍将到达
  → completion worker 统一回收
```

如果提交线程立即 `reset()`，completion worker 之后得到的将是悬空 `OVERLAPPED*`。

---

## 15. completion 路径如何收回 operation

项目入口 `runCompletionWorker()` 没有形参，返回类型为 `void`。它使用 `this->m_completionPort` 等成员持续取得完成通知，收到退出控制包或停机条件后返回。

三个 completion handler 的核心参数含义一致：

| 参数 | 作用 |
| --- | --- |
| `_operation` | 通过 `std::unique_ptr<IoOperation>` 接管完成操作的唯一所有权。handler 返回时自动析构，或在部分发送等场景中再次移动并提交。 |
| `_success` | 原生完成是否成功。失败和取消同样必须回收 `_operation`。 |
| `_transferredBytes` | receive/send 的最终字节数。只在对应完成语义允许时使用；失败时不要当作有效业务长度。 |

项目 completion worker 的关键动作：

```cpp
this->finishOperation();

auto operation{
    std::unique_ptr<IoOperation>{static_cast<IoOperation*>(overlapped)}};
```

随后通过 `operation->type` 分发到 accept、receive 或 send handler。

所有权时间线：

```text
提交前：local unique_ptr
提交后：完成协议持有 raw pointer 的回收责任
completion：worker unique_ptr 重新接管
handler 结束：unique_ptr 自动析构
```

receive handler 只使用有效范围：

```cpp
connection->receiveBuffer.append(
    operation->storage.constData(),
    static_cast<int>(transferredBytes));
```

完成解析后再投递下一次 receive，从而保持每连接最多一个 receive 在途。

---

## 16. pending 计数为什么先增加

两个计数函数的契约：

| 函数 | 参数 | 返回值 | 副作用 |
| --- | --- | --- | --- |
| `tryBeginOperation()` | 无 | 服务未停止且成功登记操作时返回 `true`；正在停止时返回 `false`。 | 成功时将 pending 增加一次。 |
| `finishOperation()` | 无 | `void`。 | 将 pending 减少一次；减到零时通知等待停机的条件变量。 |

调用约束：每次成功的 `tryBeginOperation()` 必须与恰好一次 `finishOperation()` 配对。

项目顺序：

```text
tryBeginOperation() 增加 pending
  → WSARecv/WSASend
      ├─ 同步投递失败：当前线程 finishOperation()
      └─ 成功提交：completion worker finishOperation()
```

如果改为 API 返回后再增加，会产生立即完成竞态：

```text
Thread A: WSARecv 立即完成
Thread B: 先取得 completion，pending--
Thread A: 才执行 pending++
```

后果可能包括：

- pending 下溢。
- pending 被错误观察为零。
- 停机线程提前关闭 completion port。
- operation 尚未回收，worker 却开始退出。

正确不变量：

> 每次成功进入完成协议的操作恰好增加一次 pending，并由同步失败回滚或最终 completion 恰好减少一次。

---

## 17. `socketMutex` 解决什么竞态

错误时序：

```text
Thread A: 检查 connection.socket 有效
Thread B: closesocket(connection.socket)
Thread A: 对已经关闭、甚至已被系统复用的值调用 WSARecv
```

项目让提交与关闭共享 `socketMutex`：

```text
提交路径：lock → 检查 → 增加 pending → WSARecv → unlock
关闭路径：lock → 禁止新提交 → 取消/关闭 socket → unlock
```

检查和提交必须在同一个同步边界内，不能只在调用 API 前读取一次 socket 值。

---

## 18. 常见错误与症状

| 错误 | 典型症状 | 根因 |
| --- | --- | --- |
| 提交后释放 operation | 随机崩溃、堆损坏 | Windows 仍引用 `OVERLAPPED*` |
| buffer 在途时扩容 | 数据写入旧地址 | `WSABUF.buf` 已失效 |
| 复用在途 `OVERLAPPED` | 完成归属混乱 | 两个操作共享同一标识 |
| 把 `WSA_IO_PENDING` 当失败 | 正常连接立即关闭 | 误解提交返回值 |
| 取消后立即释放 | 停机或断线时崩溃 | 取消 completion 尚未返回 |
| 使用整个 buffer 容量 | 协议尾部垃圾 | 忽略 `transferredBytes` |
| send 引用临时字符串 | 客户端收到乱码 | 发送数据提前失效 |
| 提交与关闭未同步 | 偶发 `WSAENOTSOCK` | socket 在检查后被关闭 |
| API 后才增加 pending | 停机提前退出 | 立即完成竞态 |

---

## 19. 调试记录模板

每次跟踪一个 operation，记录：

```text
operation 类型：
operation 地址：
OVERLAPPED 地址：
buffer 地址：
connection id：
socket 值：

提交线程 ID：
提交前 pending：
WSARecv/WSASend 返回值：
同步错误码：

完成线程 ID：
完成 success：
完成错误码：
transferredBytes：
完成后 pending：

operation 析构位置：
connection 是否仍存活：
```

至少推演以下四种情况：

1. receive 立即完成。
2. receive 返回 `WSA_IO_PENDING` 后成功完成。
3. 对端正常关闭，成功完成且字节数为零。
4. 请求取消后返回取消完成。

---

## 20. 源码阅读顺序

### 20.1 `IoOperation` 声明

阅读：

```text
server_transport/internal/RemoteControlTransportImpl.h
```

回答：

1. 哪些成员拥有内存？
2. 哪些成员只是视图？
3. accept、receive 和 send 各使用哪些成员？
4. connection 为什么使用 `shared_ptr`？

### 20.2 `IoOperation` 构造

阅读：

```text
server_transport/src/RemoteControlTransportRuntime.cpp
```

回答：

1. 为什么显式初始化 `OVERLAPPED{}`？
2. `nativeBuffer.buf` 指向谁？
3. 为什么 `refreshSendBuffer()` 要使用 `sendOffset`？

### 20.3 `postReceive()` 与 `postSend()`

阅读：

```text
server_transport/src/RemoteControlTransport.cpp
```

回答：

1. 为什么先增加 pending？
2. 为什么 API 前执行 `release()`？
3. 为什么同步失败执行 `reset()`？
4. 为什么立即成功不执行 `reset()`？
5. `socketMutex` 与关闭路径如何配合？

### 20.4 completion worker

本阶段只观察 operation 回收：

```text
取得 OVERLAPPED*
  → finishOperation()
  → 转回 IoOperation*
  → unique_ptr 接管
  → handler
  → 析构或重新投递
```

IOCP 的调度语义留到阶段四。

---

## 21. 练习题

### 21.1 概念题

1. `WSABUF` 为什么不能保证 buffer 存活？
2. 为什么 API 返回不总等于操作结束？
3. `WSA_IO_PENDING` 表示成功还是失败？
4. 为什么一个在途操作必须独占一个 `OVERLAPPED`？
5. `CancelIoEx()` 后为什么仍要取得最终结果？
6. receive 成功且字节数为零表示什么？
7. 项目中谁拥有 receive buffer？
8. operation 为什么持有 connection 的 `shared_ptr`？
9. pending 为什么在 API 前增加？
10. 事件模式立即完成和 IOCP 模式立即完成的回收方式有什么差异？

### 21.2 所有权图练习

画出以下对象，并在边上标记 `owns`、`observes`、`keeps alive`：

```text
ConnectionRegistry
ConnectionContext
IoOperation
OVERLAPPED
QByteArray storage
WSABUF nativeBuffer
completion worker
```

### 21.3 时序练习

分别画出：

1. `WSARecv()` 同步投递失败。
2. `WSARecv()` 返回 `WSA_IO_PENDING` 后成功完成。
3. `WSARecv()` 立即成功并通过 IOCP 回收。
4. 连接关闭导致 receive 被取消。

每张图都要标出：

- pending 增减位置。
- operation 所有者。
- buffer 是否仍被 Windows 引用。
- connection 由谁保活。

---

## 22. 参考答案要点

### 22.1 `WSABUF`

它只有地址和长度，不知道内存来自哪个 C++ 对象，也不会复制、移动或释放数据，因此只能观察 buffer。

### 22.2 `WSA_IO_PENDING`

表示操作已经正常提交，但最终结果尚未产生。operation 和 buffer 必须继续存活。

### 22.3 取消

取消是操作的一种最终完成结果。`CancelIoEx()` 只提出请求，完成通知到达前系统仍可能引用操作内存。

### 22.4 connection 保活

连接可能先从 registry 移除，再收到取消 completion。operation 的 `shared_ptr` 让 completion handler 仍能安全访问连接上下文。

### 22.5 pending 顺序

先增加 pending 可以覆盖立即完成竞态。同步投递失败由提交线程回滚，成功提交由完成路径减少。

### 22.6 立即完成差异

- 纯事件模式：返回 `0` 表示操作已经完成，当前路径可处理结果。
- 项目 IOCP 模式：默认仍会收到 completion packet，由 worker 统一回收 operation。

---

## 23. 阶段验收

### 23.1 必须能够解释

- [ ] 同步 I/O 与 Overlapped I/O 的控制流差异。
- [ ] `OVERLAPPED`、`WSABUF`、buffer 的职责和所有权。
- [ ] 立即完成、`WSA_IO_PENDING`、同步投递失败。
- [ ] 为什么取消后仍不能释放 operation。
- [ ] 为什么只能使用 `[0, transferredBytes)`。
- [ ] 为什么 send operation 必须拥有发送数据。
- [ ] 为什么要处理部分发送。
- [ ] 为什么项目 operation 持有 connection。
- [ ] 为什么 pending 在 API 前增加。
- [ ] 为什么提交和关闭共用 `socketMutex`。

### 23.2 必须能够画出

- [ ] 一次 receive operation 生命周期图。
- [ ] `IoOperation` 的对象所有权图。
- [ ] 同步投递失败的回滚路径。
- [ ] pending receive 的完成路径。
- [ ] 取消后的最终回收路径。

### 23.3 闭卷复述

不看资料完整讲出：

```text
创建 operation 和 buffer
  → 增加 pending
  → 提交 WSARecv
  → 判断立即完成、pending 或同步失败
  → 等待最终完成
  → 使用 transferredBytes
  → 减少 pending
  → 释放 operation
```

如果无法明确说明每一步由谁拥有 operation、buffer 和 connection，就继续学习阶段三，不要进入阶段四。

## 24. 官方资料

阅读时只关注四项：返回值、错误码、内存生命周期、最终完成方式。

- [WSASocketW](https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-wsasocketw)
- [WSARecv](https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-wsarecv)
- [WSASend](https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-wsasend)
- [WSAGetOverlappedResult](https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-wsagetoverlappedresult)
- [WSACreateEvent](https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-wsacreateevent)
- [WSAWaitForMultipleEvents](https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-wsawaitformultipleevents)
- [CancelIoEx](https://learn.microsoft.com/en-us/windows/win32/fileio/cancelioex-func)

进入阶段四前，必须能准确回答：

> 当前这个 `OVERLAPPED`、`WSABUF`、buffer 和 connection，在最终完成结果到达前分别由谁保证存活？
