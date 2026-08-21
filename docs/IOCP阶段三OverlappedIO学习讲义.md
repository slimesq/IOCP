# IOCP 阶段三：Overlapped I/O 学习讲义

> 前置知识：阶段二的同步 Winsock、RAII、指针与基本错误处理。
> 贯穿项目：`D:\CodeRepository\claude\remote_control`。
> 学习范围：事件通知版 Overlapped receive/send；IOCP、并发 worker、异步 accept 和安全停机将在后续阶段学习。

## 1. Overlapped I/O 学习主线

Overlapped I/O 最难的部分不是 API 数量，而是“函数已经返回，操作却还没有结束”。先在单线程事件模型中掌握 operation 生命周期，可以把这一问题与多线程、completion port、连接状态机和停机流程分开理解。

常用术语：

| 术语 | 含义 |
| --- | --- |
| operation | 一次独立的 I/O 请求，例如一次 `WSARecv()` 或一次 `WSASend()`。 |
| 在途 operation | 已经提交成功，但还没有取得最终完成结果的 I/O 请求。 |
| completion | operation 最终结束时产生的结果，包括成功或失败、传输字节数和相关 flags。 |
| buffer | 用于保存接收数据或发送数据的一段连续内存。 |
| pending | operation 已提交成功，但尚未最终完成；它不是失败。 |
| event | Windows 同步对象。本阶段用它通知某次 operation 已经结束。 |
| nonsignaled / signaled | event 的两种状态；前者表示尚未收到通知，后者表示等待条件已经满足。 |
| `transferredBytes` | 当前这一次 operation 最终实际传输的字节数，不是 buffer 容量，也不是累计值。 |
| 视图 | 只记录另一块内存的地址和长度，不拥有、复制或释放那块内存。例如 `WSABUF`。 |

学习主线：

```text
回顾同步 recv
  → 认识 OVERLAPPED、WSABUF、buffer
  → 创建支持 Overlapped I/O 的 socket
  → 准备一个事件和一次 receive
  → 区分立即完成、pending、同步失败
  → 等待事件并取得最终结果
  → 把相关对象封装为 ReceiveOperation
  → 顺序执行多次 receive
  → 学习 WSASend 与部分发送
  → 理解项目 IoOperation 的成员所有权
```

这条主线先解决 operation 与 buffer 的生命周期，再为 completion port 模型建立基础。

建议分五个学习单元推进。完成当前单元的自检后，再进入下一个单元：

1. **建立心智模型（第 4～6 节）**
   - 解决的问题：为什么 API 返回后，operation 可能仍未结束。
   - 学完自检：能画出 `OVERLAPPED`、`WSABUF` 与真实 buffer 的关系。
2. **完成一次 receive（第 7～11 节）**
   - 解决的问题：如何提交、等待并取得一次 receive 的最终结果。
   - 学完自检：能独立判断立即完成、pending 和同步投递失败。
3. **管理 receive 生命周期（第 12～16 节）**
   - 解决的问题：如何安全保存对象、处理数据并继续下一次 receive。
   - 学完自检：能解释局部变量何时安全、何时会悬空。
4. **迁移到 send（第 17～19 节）**
   - 解决的问题：receive 流程如何复用于 send，以及怎样计算剩余发送范围。
   - 学完自检：能根据每次 `transferredBytes` 正确更新 `offset`。
5. **映射到项目（第 20～22 节）**
   - 解决的问题：项目中的 `IoOperation`、buffer 和 connection 分别由谁保持存活。
   - 学完自检：能完成所有权图和最终综合验收。

---

## 2. 知识范围

### 2.1 核心内容

- `WSA_FLAG_OVERLAPPED`。
- `OVERLAPPED`。
- `WSABUF`。
- `WSACreateEvent()`、`WSACloseEvent()`。
- `WSARecv()`、`WSASend()`。
- `WSA_IO_PENDING`。
- `WSAWaitForMultipleEvents()`。
- `WSAGetOverlappedResult()`。
- operation 与 buffer 生命周期。
- 非零长度 TCP receive 的零字节完成。
- send 部分完成。

### 2.2 后续内容

以下主题不属于事件通知版 Overlapped I/O 的前置知识：

| 符号或主题 | 后续阶段 |
| --- | --- |
| `CreateIoCompletionPort()`、`GetQueuedCompletionStatus()` | 阶段四 |
| completion worker、completion key | 阶段四 |
| `AcceptEx()` | 阶段五 |
| 连接状态机、有序发送队列、背压 | 阶段七 |
| pending I/O 计数、`CancelIoEx()`、并发关闭、安全停机 | 阶段九 |

项目代码中的这些符号涉及后续模型；当前只分析与 operation 所有权直接相关的类型和函数。

---

## 3. 学习完成标准

完成本阶段后，应能够：

1. 解释同步 I/O 与 Overlapped I/O 的控制流差异。
2. 解释 `OVERLAPPED`、`WSABUF` 和真实 buffer 的职责。
3. 正确判断 `WSARecv()` 和 `WSASend()` 的三种提交结果。
4. 使用事件等待一次 operation 的最终完成。
5. 只处理 `[0, transferredBytes)` 范围内的数据。
6. 识别非零长度 TCP receive 中，对端正常关闭产生的零字节完成。
7. 解释为什么在途 operation 不能销毁或复用。
8. 解释部分发送为什么需要继续发送剩余字节。
9. 看懂项目 `IoOperation` 中哪些成员拥有内存、哪些只是视图。
10. 解释项目中的 `connection` 如何保证连接上下文活到 operation 最终完成。

建议投入 6～10 小时。

---

## 4. 从同步 receive 开始

同步代码：

```cpp
std::array<char, 8192> buffer{};

int const receivedBytes{recv(socketHandle,
                             buffer.data(),
                             static_cast<int>(buffer.size()),
                             0)};
```

### 4.1 `recv()` 参数回顾

```cpp
int recv(
    SOCKET s,
    char* buf,
    int len,
    int flags);
```

| 参数 | 作用 |
| --- | --- |
| `s` | 已连接的 socket。 |
| `buf` | 接收数据写入的内存首地址。 |
| `len` | buffer 容量，单位为 byte。 |
| `flags` | 接收标志；普通读取使用 `0`。 |

返回值：

- 大于 `0`：实际收到的字节数。
- 等于 `0`：对端正常关闭。
- `SOCKET_ERROR`：调用 `WSAGetLastError()` 取得错误码。

### 4.2 同步模式为什么容易管理生命周期

```text
buffer 创建
  → recv 阻塞
  → recv 返回最终结果
  → 使用 buffer
  → buffer 离开作用域
```

`recv()` 返回时，系统已经不会继续使用该次调用的 buffer。

---

## 5. Overlapped I/O 的控制流变化

Overlapped receive：

```text
准备 OVERLAPPED、WSABUF、buffer
  → 调用 WSARecv 提交
  → WSARecv 返回
  → 操作可能仍在进行
  → 未来产生最终完成结果
  → 应用取得最终结果
  → 才能释放或复用对象
```

关键变化：

> API 返回与 operation 最终结束不再是同一个时刻。

### 5.1 Windows 借用的是地址

提交时 Windows 得到：

```text
OVERLAPPED*  ─→ 标识本次 operation
WSABUF*      ─→ 描述 buffer
WSABUF.buf   ─→ 指向真实字节内存
```

Windows 不理解 `std::array`、`std::vector`、`QByteArray` 或对象作用域。它只使用原生地址。

`WSARecv()` 和 `WSASend()` 的服务提供者会在函数返回前捕获 `WSABUF` 描述符，因此 `WSABUF` 对象本身不必像真实 buffer 一样一直存活。真正不能提前失效的是 `WSABUF.buf` 指向的字节内存。项目仍把 `WSABUF` 放进 operation，主要是为了集中表达关系并便于后续投递。

最终完成前必须保证：

1. `OVERLAPPED` 对象仍存在，地址不变。
2. `WSABUF` 指向的内存仍存在，地址不变。
3. buffer 没有被移动、扩容或销毁。
4. socket 仍处于能够完成该 operation 的生命周期中。

在 operation 最终完成前，也不要访问正在被系统使用的内存：receive buffer 正由系统写入，send buffer 正由系统读取。

---

## 6. 三个核心对象

### 6.1 `OVERLAPPED`

```cpp
OVERLAPPED overlapped{};
```

规则：

- 必须零初始化。
- 一个在途 operation 独占一个实例。
- 最终完成前不能销毁。
- 最终完成前不能清零或复用。
- `Internal`、`InternalHigh` 由系统使用。
- socket receive/send 通常不使用文件偏移字段。
- 事件通知模式使用 `hEvent`。

### 6.2 `WSABUF`

可以简化理解为：

```cpp
struct WSABUF
{
    ULONG len;
    CHAR* buf;
};
```

准备 receive buffer：

```cpp
std::array<char, 8192> storage{};

WSABUF nativeBuffer{};
nativeBuffer.buf = storage.data();
nativeBuffer.len = static_cast<ULONG>(storage.size());
```

所有权：

```text
storage（std::array） ──拥有──> 8192 字节的真实内存
nativeBuffer.buf     ──指向──> storage.data()
```

`storage` 负责创建和释放真实内存；`WSABUF` 只保存地址和长度，不会复制、移动或释放这块内存。

需要区分两种生命周期：

```text
WSABUF 描述符：服务提供者在 WSARecv/WSASend 返回前捕获
WSABUF.buf 指向的真实字节：必须活到 operation 最终完成
```

### 6.3 真实 buffer

可以使用：

- `std::array<char, N>`。
- 已经确定大小的 `std::vector<char>`。
- 已经确定大小的 `QByteArray`。

错误示例：

```cpp
std::vector<char> storage;
storage.resize(8192);

WSABUF nativeBuffer{};
nativeBuffer.buf = storage.data();
nativeBuffer.len = static_cast<ULONG>(storage.size());

// WSARecv 已经进入 pending 后：
storage.resize(16384);  // 错误：可能改变 data() 地址。
```

---

## 7. 创建支持 Overlapped I/O 的 socket

```cpp
SOCKET socketHandle{WSASocketW(
    AF_INET,
    SOCK_STREAM,
    IPPROTO_TCP,
    nullptr,
    0,
    WSA_FLAG_OVERLAPPED)};
```

### 7.1 `WSASocketW()` 参数说明

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
| `af` | `AF_INET` | IPv4 地址族。 |
| `type` | `SOCK_STREAM` | 有序字节流 socket。 |
| `protocol` | `IPPROTO_TCP` | TCP 协议。 |
| `lpProtocolInfo` | `nullptr` | 不指定自定义协议提供者。 |
| `g` | `0` | 不使用 socket group。 |
| `dwFlags` | `WSA_FLAG_OVERLAPPED` | 允许该 socket 使用 Overlapped I/O。 |

返回值：

- 成功：有效 `SOCKET`。
- 失败：`INVALID_SOCKET`，调用 `WSAGetLastError()`。

### 7.2 错误与关闭函数

| 函数 | 参数 | 返回值与作用 |
| --- | --- | --- |
| `WSAGetLastError()` | 无 | 返回当前线程最近一次 Winsock 错误码。失败后立即读取。 |
| `closesocket(s)` | `s` 是要关闭的 socket | 成功返回 `0`，失败返回 `SOCKET_ERROR`。 |

主动关闭 socket 的前置条件是不存在在途 operation。带在途 operation 的取消与关闭属于阶段九。

---

## 8. 准备事件对象

```cpp
WSAEVENT eventHandle{WSACreateEvent()};
if (eventHandle == WSA_INVALID_EVENT)
{
    int const error{WSAGetLastError()};
    reportWinsockError(error);
}

OVERLAPPED overlapped{};
overlapped.hEvent = eventHandle;
```

`reportWinsockError(_error)` 是示意用错误报告函数；参数 `_error` 是刚刚保存的 Winsock 错误码。它不是新的 Winsock API。

### 8.1 `WSACreateEvent()`

```cpp
WSAEVENT WSACreateEvent();
```

参数：无。

返回值：

- 成功：返回一个初始为 nonsignaled 的 `WSAEVENT`。
- 失败：返回 `WSA_INVALID_EVENT`，调用 `WSAGetLastError()`。

### 8.2 `WSACloseEvent()`

```cpp
BOOL WSACloseEvent(WSAEVENT hEvent);
```

| 参数 | 作用 |
| --- | --- |
| `hEvent` | 要关闭的 `WSAEVENT`。 |

返回非零表示成功，返回 `FALSE` 表示失败。

只有 operation 已经最终完成后，才能关闭其 `hEvent`。

### 8.3 event 只是“完成通知灯”

可以把 event 理解为一盏只表示“operation 是否已经结束”的通知灯：

| event 状态 | 等待行为 | 能否判断 operation 成功 |
| --- | --- | --- |
| nonsignaled | `WSAWaitForMultipleEvents()` 正常情况下继续等待 | 不能，operation 通常还未结束 |
| signaled | `WSAWaitForMultipleEvents()` 正常返回 | 仍然不能，只能说明 operation 已结束 |

```text
WSACreateEvent 创建 event
  → 初始为 nonsignaled
  → operation 最终结束
  → Windows 将 event 设为 signaled
  → 应用再通过 WSAGetOverlappedResult 读取成功、失败和字节数
```

本阶段的一次性 receive 完成后直接关闭 event；复用同一个 event 的步骤在 19.1 节说明。

### 8.4 当前对象关系

```text
OVERLAPPED.hEvent ──保存──> eventHandle
WSABUF.buf         ──指向──> storage.data()
```

此时尚未提交 operation，所有对象仍可安全销毁。

---

## 9. 提交一次 `WSARecv()`

```cpp
DWORD flags{0};

int const result{WSARecv(socketHandle,
                         &nativeBuffer,
                         1,
                         nullptr,
                         &flags,
                         &overlapped,
                         nullptr)};
```

### 9.1 `WSARecv()` 参数说明

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

1. **`s`**
   - 示例值：`socketHandle`。
   - 作用：指定已连接且支持 Overlapped I/O 的 socket。
2. **`lpBuffers`**
   - 示例值：`&nativeBuffer`。
   - 作用：指向 `WSABUF` 数组首元素。
   - 生命周期：`WSABUF` 指向的真实 buffer 必须存活到 operation 最终完成。
3. **`dwBufferCount`**
   - 示例值：`1`。
   - 作用：指定 `WSABUF` 数组中的元素数量；当前示例只有一个。
4. **`lpNumberOfBytesRecvd`**
   - 示例值：`nullptr`。
   - 作用：Overlapped I/O 下不在提交阶段读取接收字节数；最终字节数统一由 `WSAGetOverlappedResult()` 取得。
5. **`lpFlags`**
   - 示例值：`&flags`，调用前 `flags` 为 `0`。
   - 作用：输入 receive 标志。operation 延迟完成时，该变量不会得到最终 flags；最终值通过 `WSAGetOverlappedResult()` 的 `lpdwFlags` 参数取得。
6. **`lpOverlapped`**
   - 示例值：`&overlapped`。
   - 作用：标识本次 operation。
   - 生命周期：最终完成前必须保持对象存活且地址稳定。
7. **`lpCompletionRoutine`**
   - 示例值：`nullptr`。
   - 作用：当前示例使用事件通知，不使用 completion routine。

### 9.2 三种提交结果

1. **返回 `0`**
   - 含义：operation 已立即完成，不读取错误码。
   - 下一步：不等待，调用 `WSAGetOverlappedResult()` 取得最终结果。
2. **返回 `SOCKET_ERROR`，错误码为 `WSA_IO_PENDING`**
   - 含义：提交成功，但 operation 尚未完成。
   - 下一步：等待事件。
3. **返回 `SOCKET_ERROR`，错误码为其他值**
   - 含义：同步投递失败，operation 没有进入在途状态。
   - 下一步：在当前路径处理错误，不再等待完成通知。

提交返回值只用于判断“是否需要等待”，不从 `lpNumberOfBytesRecvd` 读取字节数。只要 operation 提交成功，无论立即完成还是稍后完成，都通过 `WSAGetOverlappedResult()` 取得统一的最终结果。

必须把“提交结果”和“最终完成结果”分开：

| 阶段 | 要回答的问题 | 取得结果的方式 |
| --- | --- | --- |
| 提交阶段 | Windows 是否接受了这次 operation？是否需要等待？ | `WSARecv()` 返回值和紧接着读取的 `WSAGetLastError()` |
| 完成阶段 | operation 最终成功还是失败？实际传输了多少字节？ | `WSAGetOverlappedResult()` |

> `WSA_IO_PENDING` 只表示提交成功，不能证明 operation 最终一定成功。

判断代码：

```cpp
if (result == 0)
{
    // 立即完成。
}
else
{
    int const error{WSAGetLastError()};
    if (error == WSA_IO_PENDING)
    {
        // 正常异步路径。
    }
    else
    {
        // 同步投递失败。
    }
}
```

### 9.3 `WSA_IO_PENDING` 不是失败

它表示：

```text
Windows 已接受 operation
  → operation 仍在进行
  → OVERLAPPED 和 WSABUF.buf 指向的真实 buffer 必须继续存活
```

`WSABUF` 描述符已经由服务提供者捕获，不要把“描述符对象”和“描述符指向的真实 buffer”混为一谈。

此时不能离开对象作用域，也不能重新使用该 `OVERLAPPED`。

---

## 10. 等待事件

只有 `WSARecv()` 返回 `WSA_IO_PENDING` 时才需要等待未来完成。

```cpp
WSAEVENT events[]{eventHandle};

DWORD const waitResult{WSAWaitForMultipleEvents(
    1,
    events,
    TRUE,
    WSA_INFINITE,
    FALSE)};
```

### 10.1 `WSAWaitForMultipleEvents()` 参数说明

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
| `cEvents` | `1` | 事件数组中的元素数量。 |
| `lphEvents` | `events` | 事件数组首地址。 |
| `fWaitAll` | `TRUE` | 等待全部事件；这里只有一个事件。 |
| `dwTimeout` | `WSA_INFINITE` | 无限等待，使当前流程不涉及超时后的取消。 |
| `fAlertable` | `FALSE` | 不使用 completion routine，因此不需要 alertable wait。 |

主要返回值：

- `WSA_WAIT_EVENT_0`：第一个事件满足条件。
- `WSA_WAIT_FAILED`：等待失败，调用 `WSAGetLastError()`。

当前流程只等待一个事件并使用无限超时；超时后的取消不在该流程中处理。

---

## 11. 取得最终结果

`WSARecv()` 返回 `0` 时，operation 已经完成；返回 `WSA_IO_PENDING` 时，要先等到事件 signaled。两条成功提交路径最终都调用：

```cpp
DWORD transferredBytes{0};
DWORD completedFlags{0};

BOOL const completed{WSAGetOverlappedResult(
    socketHandle,
    &overlapped,
    &transferredBytes,
    FALSE,
    &completedFlags)};
```

### 11.1 `WSAGetOverlappedResult()` 参数说明

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
| `s` | `socketHandle` | 提交该 receive 的 socket。 |
| `lpOverlapped` | `&overlapped` | 提交时使用的同一个 `OVERLAPPED`。 |
| `lpcbTransfer` | `&transferredBytes` | 输出最终传输字节数。 |
| `fWait` | `FALSE` | 调用前已经通过返回值或事件确认 operation 完成，所以这里只查询，不再次等待。 |
| `lpdwFlags` | `&completedFlags` | 输出该 operation 的最终 flags。 |

返回值：

- 非零：operation 成功完成。
- `FALSE`：operation 失败，调用 `WSAGetLastError()` 取得最终错误。

### 11.2 为什么不能只看事件

事件只表示 operation 已经结束，不表示它一定成功。最终成功、错误和字节数必须通过 `WSAGetOverlappedResult()` 取得。

### 11.3 何时可以释放对象

只有以下两条路径允许释放：

```text
WSARecv 返回 0，立即完成
  → WSAGetOverlappedResult 返回最终结果

或

WSARecv 返回 WSA_IO_PENDING
  → event 变为 signaled
  → WSAGetOverlappedResult 返回最终结果
```

同步投递失败没有进入在途状态，也可以由当前路径直接释放。

---

## 12. 组合成一次完整 receive

以下函数完整展示一次 event-based receive。函数返回前会等待 operation 最终完成，因此局部变量不会提前失效。

阅读代码时按五个阶段划分：

1. 创建 event、`OVERLAPPED`、`WSABUF` 和真实 buffer。
2. 调用 `WSARecv()`，判断立即完成、pending 或同步失败。
3. 仅在 pending 路径等待 event。
4. 调用 `WSAGetOverlappedResult()` 取得最终错误和 `transferredBytes`。
5. 关闭 event，再把结果分类为数据、对端关闭或失败。

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

[[nodiscard]] ReceiveResult receiveOneChunk(
    SOCKET _socket,
    std::array<char, 8192>& _storage)
{
    WSAEVENT eventHandle{WSACreateEvent()};
    if (eventHandle == WSA_INVALID_EVENT)
    {
        return {ReceiveResultKind::Failed, 0, WSAGetLastError()};
    }

    OVERLAPPED overlapped{};
    overlapped.hEvent = eventHandle;

    WSABUF nativeBuffer{};
    nativeBuffer.buf = _storage.data();
    nativeBuffer.len = static_cast<ULONG>(_storage.size());

    DWORD flags{0};
    int const result{WSARecv(_socket,
                             &nativeBuffer,
                             1,
                             nullptr,
                             &flags,
                             &overlapped,
                             nullptr)};

    int waitError{0};
    if (result == SOCKET_ERROR)
    {
        int const submitError{WSAGetLastError()};
        if (submitError != WSA_IO_PENDING)
        {
            static_cast<void>(WSACloseEvent(eventHandle));
            return {ReceiveResultKind::Failed, 0, submitError};
        }

        WSAEVENT events[]{eventHandle};
        DWORD const waitResult{WSAWaitForMultipleEvents(
            1,
            events,
            TRUE,
            WSA_INFINITE,
            FALSE)};

        if (waitResult == WSA_WAIT_FAILED)
        {
            waitError = WSAGetLastError();
        }
    }

    DWORD transferredBytes{0};
    DWORD completedFlags{0};
    BOOL const completed{WSAGetOverlappedResult(
        _socket,
        &overlapped,
        &transferredBytes,
        TRUE,
        &completedFlags)};

    int finalError{waitError};
    if (completed == FALSE)
    {
        finalError = WSAGetLastError();
    }

    static_cast<void>(WSACloseEvent(eventHandle));

    if (finalError != 0)
    {
        return {ReceiveResultKind::Failed, 0, finalError};
    }
    if (transferredBytes == 0)
    {
        return {ReceiveResultKind::PeerClosed, 0, 0};
    }
    return {ReceiveResultKind::Data, transferredBytes, 0};
}
```

### 12.1 自定义函数参数说明

| 参数 | 作用 | 生命周期要求 |
| --- | --- | --- |
| `_socket` | 已连接且支持 Overlapped I/O 的 socket。 | 函数返回前保持有效。 |
| `_storage` | 调用者提供的接收 buffer。 | 函数会等待最终完成，因此引用在整个 operation 期间有效。 |

返回 `ReceiveResult`：

- `Data`：`_storage[0..transferredBytes)` 有效。
- `PeerClosed`：本例提交的是非零长度 TCP receive，因此零字节成功完成表示对端正常关闭发送方向。
- `Failed`：`error` 保存错误码。

### 12.2 为什么 `WSAGetOverlappedResult()` 使用 `TRUE`

| 调用前的情况 | `TRUE` 的行为 |
| --- | --- |
| `WSARecv()` 返回 `0` | operation 已经完成，函数立即返回。 |
| pending 且 event 已变为 signaled | operation 已经完成，函数立即返回。 |
| event 等待发生异常 | 若 operation 仍未结束，继续等待其最终完成，避免局部 `OVERLAPPED` 和 buffer 提前失效。 |

`fWait` 只有在 operation 使用 event-based completion notification 时才能设为 `TRUE`；当前示例满足这个前提。

等待异常后的取消与排空属于安全停机主题。

---

## 13. 正确使用 receive 结果

```cpp
std::array<char, 8192> storage{};
ReceiveResult const result{receiveOneChunk(socketHandle, storage)};

if (result.kind == ReceiveResultKind::Data)
{
    processBytes(storage.data(), result.transferredBytes);
}
else if (result.kind == ReceiveResultKind::PeerClosed)
{
    closeAfterPeerShutdown();
}
else
{
    reportWinsockError(result.error);
}
```

### 13.1 示例辅助函数参数

| 函数 | 参数 | 作用 |
| --- | --- | --- |
| `processBytes(_data, _size)` | `_data` 是有效数据首地址；`_size` 是实际字节数 | 把字节追加到协议接收缓冲区。 |
| `closeAfterPeerShutdown()` | 无 | 对端正常关闭后释放当前同步练习中的连接资源。 |
| `reportWinsockError(_error)` | `_error` 是已保存的 Winsock 错误码 | 输出错误，不重新读取可能已变化的 last error。 |

### 13.2 只处理有效范围

错误：

```cpp
processBytes(storage.data(), storage.size());
```

正确：

```cpp
processBytes(storage.data(), result.transferredBytes);
```

buffer 容量不等于本次完成字节数。

### 13.3 TCP 仍然没有消息边界

一次 receive completion 可能得到：

- 半个 Packet。
- 一个完整 Packet。
- 多个 Packet。

Overlapped I/O 不会改变 TCP 字节流语义。收到的字节仍要追加到累计缓冲区，再循环解析完整 Packet。

---

## 14. 局部变量何时安全、何时危险

### 14.1 局部变量安全的条件

```text
创建局部 OVERLAPPED、event、buffer
  → 提交 WSARecv
  → 在同一函数中等待最终完成
  → 处理结果
  → 函数返回
```

所有局部对象都活到 operation 最终完成。

### 14.2 以下代码为什么危险

```cpp
void postReceiveAndReturn(SOCKET _socket)
{
    std::array<char, 8192> storage{};
    WSABUF nativeBuffer{
        static_cast<ULONG>(storage.size()), storage.data()};
    OVERLAPPED overlapped{};

    DWORD flags{0};
    static_cast<void>(WSARecv(_socket,
                              &nativeBuffer,
                              1,
                              nullptr,
                              &flags,
                              &overlapped,
                              nullptr));
}  // operation 可能仍在途，三个对象却全部失效。
```

问题不是“栈对象”本身，而是函数在最终完成前返回。

---

## 15. 把对象聚合为 `ReceiveOperation`

在理解单次流程后，再把相关对象放入一个类型：

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

private:
    OVERLAPPED m_overlapped{};
    std::array<char, 8192> m_storage{};
    WSABUF m_nativeBuffer{};
    WSAEVENT m_event{WSA_INVALID_EVENT};
};
```

### 15.1 设计目的

```text
ReceiveOperation
  ├─ m_overlapped：标识当前 receive operation
  ├─ m_storage：拥有接收数据使用的真实内存
  ├─ m_event：保存并负责关闭事件句柄
  └─ m_nativeBuffer.buf：指向 m_storage，不拥有内存
```

### 15.2 为什么禁止复制和移动

`WSABUF::buf` 指向对象内部的 `m_storage`。复制或移动后，内部指针可能仍指向旧对象地址。初学版本直接禁止复制和移动，避免额外复杂度。

### 15.3 析构前置条件

该类型只聚合资源，不会自动等待在途 operation。使用者仍必须保证：

> `ReceiveOperation` 析构前，对应 receive 已经最终完成。

不要误以为“使用类封装”就自动解决异步生命周期。

---

## 16. 顺序执行多次 receive

多次 receive 可以先按以下顺序执行：

```text
创建一次 operation
  → 提交
  → 等待最终完成
  → 处理字节
  → operation 析构
  → 创建下一次 operation
```

示意代码：

```cpp
while (true)
{
    std::array<char, 8192> storage{};
    ReceiveResult const result{receiveOneChunk(socketHandle, storage)};

    if (result.kind == ReceiveResultKind::Failed)
    {
        reportWinsockError(result.error);
        break;
    }
    if (result.kind == ReceiveResultKind::PeerClosed)
    {
        break;
    }

    processBytes(storage.data(), result.transferredBytes);
}
```

这个循环在等待时仍会阻塞当前线程，因此不是最终高并发架构。它的目标只是验证 Overlapped I/O 的提交和生命周期。

---

## 17. 学习 `WSASend()`

关键调用：

```cpp
int const result{WSASend(socketHandle,
                         &nativeBuffer,
                         1,
                         nullptr,
                         0,
                         &overlapped,
                         nullptr)};
```

### 17.1 `WSASend()` 参数说明

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

1. **`s`**
   - 示例值：`socketHandle`。
   - 作用：指定已连接且支持 Overlapped I/O 的 socket。
2. **`lpBuffers`**
   - 示例值：`&nativeBuffer`。
   - 作用：指向待发送的 `WSABUF` 数组。
   - 生命周期：真实发送数据必须存活到 operation 最终完成。
3. **`dwBufferCount`**
   - 示例值：`1`。
   - 作用：指定 `WSABUF` 数组中的元素数量。
4. **`lpNumberOfBytesSent`**
   - 示例值：`nullptr`。
   - 作用：Overlapped I/O 下不在提交阶段读取发送字节数；最终发送字节数统一由 `WSAGetOverlappedResult()` 取得。
5. **`dwFlags`**
   - 示例值：`0`。
   - 作用：指定发送标志；普通 TCP 发送不使用特殊标志。
6. **`lpOverlapped`**
   - 示例值：`&overlapped`。
   - 作用：标识本次 send operation。
   - 生命周期：最终完成前必须保持对象存活且地址稳定。
7. **`lpCompletionRoutine`**
   - 示例值：`nullptr`。
   - 作用：当前示例使用事件通知，不使用 completion routine。

三种提交结果与 `WSARecv()` 相同：

- 返回 `0`：立即完成，不需要等待。
- `SOCKET_ERROR + WSA_IO_PENDING`：正常 pending，需要等待事件。
- `SOCKET_ERROR + 其他错误`：同步投递失败。

前两种情况都要通过 `WSAGetOverlappedResult()` 取得本次最终发送字节数。

### 17.2 receive 与 send 的相同点和不同点

两者的完成流程相同：

```text
准备 OVERLAPPED、WSABUF、真实 buffer 和 event
  → 提交 WSARecv 或 WSASend
  → 判断立即完成、pending 或同步失败
  → pending 时等待 event
  → WSAGetOverlappedResult 取得最终结果
  → operation 最终完成后释放或复用相关对象
```

真正需要区分的是 buffer 的方向和完成后的处理：

| 对比项 | receive | send |
| --- | --- | --- |
| Windows 如何使用 buffer | 向 buffer 写入收到的数据 | 从 buffer 读取待发送数据 |
| 在途期间应用能否访问 buffer | 不能读取或修改 | 不能读取、修改或释放 |
| `transferredBytes == 0` | 表示对端正常关闭发送方向 | 表示没有发送进展，不能无限重投 |
| 部分完成后的处理 | 处理本次收到的有效范围，再提交下一次 receive | 更新 offset，继续发送剩余范围 |

> `WSASend()` 成功完成，只表示 Windows 传输层已经消费了相应 buffer，不表示远端应用已经收到或处理这些数据。

---

## 18. send buffer 必须持续存活

错误：

```cpp
std::string response{"hello"};
postOverlappedSend(socketHandle, response.data(), response.size());
response.clear();  // send 可能仍在使用旧地址。
```

这里的 `postOverlappedSend(_socket, _data, _size)` 是示意函数：`_socket` 是目标 socket，`_data` 是待发送字节首地址，`_size` 是字节数。错误的根因是该函数只接收地址和长度，没有取得数据所有权。

正确思路：send operation 自己拥有发送数据。

```cpp
struct SendOperation final
{
    explicit SendOperation(std::vector<char> _bytes)
        : bytes{std::move(_bytes)},
          event{WSACreateEvent()}
    {
        this->overlapped.hEvent = this->event;
        this->refreshBuffer();
    }

    ~SendOperation()
    {
        if (this->event != WSA_INVALID_EVENT)
        {
            WSACloseEvent(this->event);
        }
    }

    [[nodiscard]] bool canSubmit() const noexcept
    {
        return this->event != WSA_INVALID_EVENT &&
               this->offset < this->bytes.size();
    }

    SendOperation(SendOperation const&) = delete;
    SendOperation(SendOperation&&) = delete;
    SendOperation& operator=(SendOperation const&) = delete;
    SendOperation& operator=(SendOperation&&) = delete;

    void refreshBuffer() noexcept
    {
        if (this->offset >= this->bytes.size())
        {
            this->nativeBuffer = {};
            return;
        }

        this->nativeBuffer.buf = this->bytes.data() + this->offset;
        this->nativeBuffer.len = static_cast<ULONG>(
            this->bytes.size() - this->offset);
    }

    std::vector<char> bytes;
    std::size_t offset{0};
    OVERLAPPED overlapped{};
    WSABUF nativeBuffer{};
    WSAEVENT event{WSA_INVALID_EVENT};
};
```

### 18.1 构造函数参数

| 参数 | 作用 |
| --- | --- |
| `_bytes` | 非空的完整待发送数据。按值接收后移动到 operation，使 operation 拥有数据。 |

`refreshBuffer()` 没有参数，根据 `offset` 让 `nativeBuffer` 指向剩余字节。

构造后必须先调用 `canSubmit()`；返回 `false` 表示 event 创建失败或没有剩余数据，不能提交 send。`refreshBuffer()` 对空数据和已全部发送的情况会生成空 `WSABUF`，避免对空地址做指针运算。

析构函数会关闭 event，但前置条件仍是 send 已最终完成；不能依靠析构函数终止仍在途的 operation。

---

## 19. 处理部分发送

假设总数据为 1000 bytes：

```text
第一次完成 400
  → offset = 400
  → 下一次发送 [400, 1000)

第二次完成 600
  → offset = 1000
  → 整个业务 buffer 才发送完成
```

关键逻辑：

以下代码只在 `WSASend()` 已成功完成，并且已经取得本次 `transferredBytes` 后执行：

```cpp
if (transferredBytes == 0)
{
    // 没有发送进展，进入错误处理，不再重投。
}
else
{
    operation.offset += transferredBytes;

    if (operation.offset < operation.bytes.size())
    {
        // 剩余范围是 [operation.offset, operation.bytes.size())。
        // 到这里仅更新了发送进度，尚未准备或提交下一次 operation。
    }
    else
    {
        // 完整发送结束。
    }
}
```

如果还有剩余数据，可以让新的 operation 自己拥有剩余字节；也可以按 19.1 节重置当前 operation，再调用 `refreshBuffer()` 指向剩余范围。两种方式都必须等前一次 send 最终完成后才能执行。

### 19.1 可选：复用 event 和 `OVERLAPPED`

理解部分发送只需要掌握 `offset` 和剩余范围。只有选择复用同一个 `SendOperation` 时，才需要下面的重置步骤。

`WSACreateEvent()` 创建的是 manual-reset event，也就是需要显式恢复状态的事件。一次 operation 完成后，event 处于 signaled 状态；复用同一个 event 前，要调用：

```cpp
BOOL WSAResetEvent(WSAEVENT hEvent);
```

| 参数 | 作用 |
| --- | --- |
| `hEvent` | 要恢复为 nonsignaled 状态的 `WSAEVENT`。示例传入 `operation.event`。 |

返回值：

- `TRUE`：重置成功，可以继续准备下一次 operation。
- `FALSE`：重置失败，立即调用 `WSAGetLastError()`，不要再次提交。

完整准备顺序：

```cpp
BOOL const reset{WSAResetEvent(operation.event)};
if (reset == FALSE)
{
    int const error{WSAGetLastError()};
    reportWinsockError(error);
}
else
{
    operation.overlapped = {};
    operation.overlapped.hEvent = operation.event;
    operation.refreshBuffer();
    // 此时才可以提交剩余数据。
}
```

`operation.overlapped = {};` 用于清除前一次 operation 留下的内部状态，随后必须重新设置 `hEvent`。这些操作只能发生在前一次 send 最终完成之后。

规则：

1. `transferredBytes` 是本次完成量，不是累计量。
2. 完成字节数为 `0` 时不能无限重试，应视为无进展错误。
3. 前一次 send 最终完成后，才可以更新 `offset` 和 `WSABUF`。
4. `refreshBuffer()` 只更新剩余范围；复用原 operation 再次提交前，还要执行 19.1 节的重置步骤。

---

## 20. 项目 `IoOperation` 所有权分析

分析范围：

```text
server_transport/internal/RemoteControlTransportImpl.h
  → IoOperation 声明

server_transport/src/RemoteControlTransportRuntime.cpp
  → IoOperation 构造函数
  → refreshSendBuffer()
```

`postReceive()`、completion worker、pending 计数和 `stop()` 涉及 completion port 与安全停机，不影响本节的所有权分析。

### 20.1 结构成员

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

成员关系：

| 成员 | 作用 |
| --- | --- |
| `OVERLAPPED` 基类 | 让该对象本身表示一次原生异步 operation。 |
| `type` | 标记 operation 是 accept、receive 还是 send；completion 分发在阶段四学习。 |
| `connection` | 保存一个 `shared_ptr<ConnectionContext>` 强引用，使连接上下文至少存活到 operation 被回收。 |
| `acceptSocket` | accept operation 使用的 socket；具体用法在阶段五学习。receive 和 send 不使用它。 |
| `storage` | 拥有 receive 使用的 buffer；accept operation 也会复用该成员，具体用法在阶段五学习。 |
| `sendBytes` | 拥有发送数据。 |
| `sendOffset` | 记录部分发送进度。 |
| `nativeBuffer` | 指向 `storage` 或 `sendBytes`，本身不拥有字节。 |

所有权链：

```text
在途 operation 的所有者
  → 保证 IoOperation 活到最终完成
  → IoOperation 内部拥有 OVERLAPPED 基类和 storage/sendBytes
  → IoOperation.connection 通过 shared_ptr 保持 ConnectionContext 存活
```

事件练习中，“在途 operation 的所有者”就是尚未返回的 `receiveOneChunk()` 调用。项目中的所有权转移与回收将在阶段四和阶段九展开。

### 20.2 receive 构造函数

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

参数：

| 参数 | 作用 |
| --- | --- |
| `_type` | operation 类型。 |
| `_connection` | 与 operation 关联的上下文；移动到 `connection` 后形成强引用，保证上下文不会早于 operation 消失。 |
| `_bufferSize` | `storage` 的字节数。 |

观察重点：

```text
IoOperation.storage
  └─ 拥有 receive 使用的真实字节内存

IoOperation.nativeBuffer.buf
  └─ 指向 storage.data()，但不拥有这块内存

IoOperation.connection
  └─ 通过 shared_ptr 保持 ConnectionContext 存活
```

### 20.3 send buffer 更新

```cpp
void IoOperation::refreshSendBuffer()
{
    this->nativeBuffer.buf = this->sendBytes.data() + this->sendOffset;
    this->nativeBuffer.len = static_cast<ULONG>(
        this->sendBytes.size() - this->sendOffset);
}
```

该函数没有参数，使用成员 `sendBytes` 和 `sendOffset` 计算剩余发送范围。

---

## 21. 常见错误

| 错误 | 症状 | 根因 |
| --- | --- | --- |
| 把 `WSA_IO_PENDING` 当失败 | 正常 operation 被中止 | 未区分提交成功与最终完成 |
| 提交后函数立即返回 | 随机崩溃、内存破坏 | 局部 `OVERLAPPED` 和 buffer 失效 |
| 在途期间扩容 buffer | 数据写入旧地址 | `WSABUF.buf` 已悬空 |
| 同一 `OVERLAPPED` 同时投递两次 | 完成结果混乱 | operation 标识被复用 |
| event signaled 就认为成功 | 错误未被发现 | 没有调用 `WSAGetOverlappedResult()` |
| 使用整个 buffer 容量 | 协议出现尾部垃圾 | 忽略完成字节数 |
| receive 零字节后继续投递 | 反复关闭或空循环 | 未识别对端正常关闭 |
| send 数据来自临时字符串 | 客户端收到乱码 | send buffer 提前失效 |
| 假设一次 send 完成全部数据 | 响应被截断 | 未处理部分发送 |
| 复用 event 和 `OVERLAPPED` 前未重置 | 等待立即返回旧状态或完成信息混乱 | 沿用了上一次 operation 的状态 |

---

## 22. 阶段练习与验收

按编号完成练习，先根据验收标准自行检查，再查看“参考答案与解释”。API 参数题重在理解参数作用、控制流和生命周期，不要求死记函数原型。

### 22.1 任务一：画对象关系图

**练习**

画出以下对象：

```text
SOCKET
OVERLAPPED
WSAEVENT
WSABUF
storage
```

把 `OVERLAPPED` 标注为当前 operation 的原生标识，并使用“拥有”“指向”“保存句柄”“操作目标”说明对象关系。然后解释：为什么同一个 `OVERLAPPED` 不能同时代表两个在途 operation？

**验收标准**

- [ ] 图中包含全部五个对象。
- [ ] 能指出真实字节由谁拥有。
- [ ] 能指出 `WSABUF` 为什么不是所有者。
- [ ] 能指出 event 由哪个字段引用。
- [ ] 能指出 operation 与 socket 是目标关系，不是所有权关系。
- [ ] 能解释一个在途 operation 必须独占一个 `OVERLAPPED`。

**参考答案与解释**

```text
storage             ──拥有──> 真正的字节内存
WSABUF.buf           ──指向──> storage.data()
OVERLAPPED.hEvent    ──保存──> WSAEVENT 句柄
WSARecv / WSASend    ──操作──> SOCKET
OVERLAPPED           ──标识──> 当前这一次 I/O operation
```

逐行解释：

1. `storage` 是真正存放数据的内存。例如 receive 完成后，收到的字节就在这里。
2. `WSABUF.buf` 只保存 `storage` 的地址。它没有自己的数据，也不会负责释放 `storage`。
3. `OVERLAPPED.hEvent` 保存事件句柄。operation 完成后，Windows 会把这个 event 设为 signaled。
4. `WSARecv()` 或 `WSASend()` 在某个 `SOCKET` 上提交 operation，但不会取得 socket 的所有权。
5. `OVERLAPPED` 是这一次 operation 的原生状态记录，Windows 会在其中维护内部状态和完成信息。

可以把 `OVERLAPPED` 理解为一张 I/O 工单：一次 operation 使用一张工单，完成结果也写回这张工单。如果两个尚未完成的 operation 共用同一个 `OVERLAPPED`，就相当于两项任务共用同一张工单，两次操作会修改同一份状态，应用也无法判断结果属于哪一次操作。因此：

> 一个在途 operation 必须独占一个 `OVERLAPPED`，直到该 operation 最终完成。

### 22.2 任务二：读懂 API 参数

**练习**

根据前面的函数声明和调用示例，依次解释以下调用中的每个实参：

- `WSASocketW()` 六个参数。
- `WSARecv()` 七个参数。
- `WSAWaitForMultipleEvents()` 五个参数。
- `WSAGetOverlappedResult()` 五个参数。
- `WSASend()` 七个参数。

每个实参说明四项内容：当前示例值、输入或输出方向、实际作用、是否涉及 operation 生命周期。无需默写参数类型和完整函数原型。

**验收标准**

- [ ] 看着函数声明，能从左到右解释每个实参的当前值和用途。
- [ ] 能解释 `nullptr`、`1`、`TRUE`、`FALSE` 和 `WSA_INFINITE` 在当前示例中的含义。
- [ ] 能指出哪些参数指向的对象必须活到最终完成。
- [ ] 能区分“提交返回值”和“最终完成字节数”，不从提交期输出参数读取最终结果。

**参考答案与解释**

| 参数或字段 | 当前用法 | 关键原因 |
| --- | --- | --- |
| `lpBuffers` | 描述 receive 或 send 的真实 buffer | `WSABUF` 描述符会被捕获，但它指向的真实内存必须活到最终完成。 |
| `lpOverlapped` | 指向当前 operation 独占的 `OVERLAPPED` | Windows 使用它保存本次 operation 的内部状态和完成信息。 |
| `lpOverlapped->hEvent` | 保存当前 operation 使用的 event | pending operation 完成后，Windows 将 event 设为 signaled。 |
| `lpNumberOfBytesRecvd` / `lpNumberOfBytesSent` | 传 `nullptr` | 最终字节数统一由 `WSAGetOverlappedResult()` 取得。 |

### 22.3 任务三：判断三种提交结果

**练习**

写出以下情况的含义和下一步：

1. `WSARecv()` 返回 `0`。
2. 返回 `SOCKET_ERROR`，错误为 `WSA_IO_PENDING`。
3. 返回 `SOCKET_ERROR`，紧接着调用 `WSAGetLastError()` 得到 `WSAECONNRESET`。
4. 把 `WSARecv()` 换成 `WSASend()` 后，三种提交结果的分类是否改变？

**验收标准**

- [ ] 不把 `WSA_IO_PENDING` 当作失败。
- [ ] 知道返回 `0` 时不需要等待，但仍通过 `WSAGetOverlappedResult()` 取得最终字节数。
- [ ] 知道 Overlapped I/O 的 `lpNumberOfBytesRecvd` 和 `lpNumberOfBytesSent` 按官方建议传 `nullptr`。
- [ ] 知道同步投递失败没有进入在途状态。
- [ ] 能把同一套三分法应用到 `WSASend()`。

**参考答案与解释**

```text
返回 0
  → 立即完成
  → 不等待
  → WSAGetOverlappedResult 取得最终结果

WSA_IO_PENDING
  → 提交成功、尚未最终完成
  → 等待 event
  → WSAGetOverlappedResult 取得最终结果

其他错误
  → 同步投递失败
  → 当前路径处理错误
```

`WSARecv()` 和 `WSASend()` 使用相同的提交结果三分法。

### 22.4 任务四：写出一次 event-based receive

**练习**

完成两项内容：

1. 画出一次 pending receive 的完整时序，并标出对象何时可以释放。
2. 不构建项目，可以查看 API 函数声明，但不要照抄 `receiveOneChunk()`；独立写出 `WSARecv()`、`WSAWaitForMultipleEvents()` 和 `WSAGetOverlappedResult()` 的关键调用片段，并处理三种提交结果。

可使用以下骨架组织控制流：

```text
WSARecv
  ├─ 返回 0：？
  ├─ WSA_IO_PENDING：？
  └─ 其他错误：？
```

**验收标准**

- [ ] `OVERLAPPED` 和 storage 都活到最终结果取得。
- [ ] 能说明服务提供者会捕获 `WSABUF` 描述符，但其指向的真实字节必须继续存活。
- [ ] 不把 event signaled 直接当作成功。
- [ ] 能处理 `WSAGetOverlappedResult()` 返回 `FALSE` 的最终失败路径。
- [ ] 只在最终完成后关闭 event。
- [ ] 能解释局部变量为什么在 `receiveOneChunk()` 中是安全的。
- [ ] 代码片段对 `lpNumberOfBytesRecvd` 传 `nullptr`，并统一从 `WSAGetOverlappedResult()` 取得字节数。

**参考答案与解释**

```text
对象创建
  → WSARecv
  → 判断立即完成、pending 或同步失败
  → pending 时等待 event
  → 成功提交路径调用 WSAGetOverlappedResult
  → 使用 [0, transferredBytes)
  → 关闭 event
  → 对象析构
```

event 只表示 operation 已结束；成功、错误和字节数仍由 `WSAGetOverlappedResult()` 给出。`WSABUF` 是描述符，`WSABUF.buf` 指向的 storage 才是必须持续存活的真实 buffer。

如果 `WSAGetOverlappedResult()` 返回 `FALSE`，应立即保存 `WSAGetLastError()`，不得把 buffer 当作成功接收的数据处理。

### 22.5 任务五：解释 receive 数据语义

**练习**

回答：

1. 为什么本练习使用非零长度 TCP receive 时，成功且 `transferredBytes == 0` 表示对端正常关闭发送方向？
2. 为什么不能处理整个 buffer 容量？
3. 为什么一次 completion 不能直接当作一个 Packet？

**验收标准**

- [ ] 能识别非零长度 TCP receive 的零字节完成。
- [ ] 只处理 `[0, transferredBytes)`。
- [ ] 能说明半包和粘包仍然存在。

**参考答案与解释**

```text
非零长度 TCP receive 成功且 transferredBytes == 0
  → TCP 对端正常关闭发送方向

有效数据范围
  → [0, transferredBytes)

一次 completion
  → 只代表本次收到的一批 TCP 字节
  → 不提供应用消息边界
```

### 22.6 任务六：推演部分发送

**练习**

总长度为 1500 bytes：

- 第一次完成 600。
- 第二次完成 500。
- 第三次完成 400。

写出每次完成后的 `offset` 和下一次 `WSABUF` 范围。

然后回答：

1. 为什么在途期间不能修改或释放完整 send buffer？
2. 如果某次成功完成但 `transferredBytes == 0`，为什么不能继续无限重投？
3. 何时才可以更新 `offset`、刷新 `WSABUF` 并投递剩余数据？

**验收标准**

- [ ] `offset` 使用累计完成量。
- [ ] 每次 `WSABUF` 只指向剩余范围。
- [ ] 能解释 send operation 为什么必须拥有发送数据。
- [ ] 知道完成字节数为零时不能无限重投。
- [ ] 只在前一次 send 最终完成后更新发送进度。

**参考答案与解释**

```text
第一次：offset = 600，下一次 [600, 1500)
第二次：offset = 1100，下一次 [1100, 1500)
第三次：offset = 1500，发送完成

transferredBytes == 0
  → 没有取得进展
  → 停止重投并进入错误处理
```

send 最终完成前，系统仍可能读取 send buffer；只有取得最终完成结果后，才能累计 `offset` 并准备下一次 operation。复用当前 event 和 `OVERLAPPED` 继续发送时，按 19.1 节完成重置。

### 22.7 任务七：只读项目 `IoOperation`

**练习**

从 `IoOperation` 中找出并画成所有权链：

1. receive buffer 所有者。
2. send buffer 所有者。
3. 不拥有数据的原生视图。
4. 部分发送进度字段。
5. 保持 `ConnectionContext` 存活的成员。

**验收标准**

- [ ] 能画出 `IoOperation` 内部所有权图。
- [ ] 能解释 `nativeBuffer` 指向哪个拥有数据的成员。
- [ ] 能解释 `connection` 为什么使用 `shared_ptr`，以及它保持谁存活。
- [ ] 只阅读声明、构造函数和 `refreshSendBuffer()`。
- [ ] 不依赖 completion worker、pending 计数和安全停机知识。

**参考答案与解释**

```text
storage
  └─ 拥有 receive 使用的真实字节内存

sendBytes
  └─ 拥有 send 使用的完整待发送数据

nativeBuffer.buf
  └─ 指向 storage 或 sendBytes 中的一段内存，本身不拥有数据

sendOffset
  └─ 记录 sendBytes 中已经完成发送的字节数

connection
  └─ 通过 shared_ptr 保持 ConnectionContext 存活
```

### 22.8 最终综合验收

**练习**

闭卷完成三项内容，不要求默写 API 原型：

1. 对比同步 `recv()` 和 event-based Overlapped receive 的控制流与 buffer 生命周期。
2. 使用以下关键词，自行组织并复述完整流程：`WSA_FLAG_OVERLAPPED`、event、`OVERLAPPED`、`WSABUF`、真实 buffer、返回 `0`、`WSA_IO_PENDING`、同步失败、等待、`WSAGetOverlappedResult()`、`transferredBytes`、释放。
3. 回答：当前 `OVERLAPPED`、真实 buffer 和 `ConnectionContext` 在最终完成前分别由谁保证存活？

**验收标准**

- [ ] 能解释同步调用返回与 Overlapped operation 最终完成的区别。
- [ ] 能在不看资料的情况下组织完整控制流，不要求背诵函数原型。
- [ ] 每一步都能说明 operation 和 buffer 是否仍被系统使用。
- [ ] 能说明项目中的 `IoOperation`、真实 buffer 和 `ConnectionContext` 分别由谁保持存活。
- [ ] 能解释为什么一个在途 operation 必须独占一个 `OVERLAPPED`。
- [ ] 能同时解释 receive 零字节和 send 部分完成。
- [ ] 复述过程不依赖阶段四及之后的知识。

全部任务通过后，阶段三才算完成。

---

## 23. 下一阶段衔接

阶段四只更换“最终完成通知与回收 operation 的位置”，不会改变本阶段已经得到的 I/O 语义：

```text
阶段三：每次 operation 通过 event 取得完成结果
阶段四：多个 handle 的 operation 统一从 completion port 取得结果
```

进入阶段四时，直接带入下面四个结论：

| 阶段三已经解决的问题 | 直接带入阶段四的结论 | 对应复习位置 |
| --- | --- | --- |
| `WSARecv()`、`WSASend()` 怎样提交 | 返回 `0` 和 `WSA_IO_PENDING` 都是成功提交，其他错误是同步失败 | 第 9、17 节 |
| operation 内部对象怎样存活 | `OVERLAPPED`、真实 buffer 和 connection 必须活到最终完成 | 第 14、15、18、20 节 |
| receive completion 的数据怎样使用 | 只处理实际完成范围；非零长度 TCP receive 完成 0 bytes 表示对端正常关闭 | 第 13 节 |
| send completion 怎样推进 | 使用 `sendOffset` 累计完成量，未发送完就更新 `WSABUF` 后重新提交 | 第 19 节 |

阶段四将在这些结论之上回答一个新问题：提交函数返回后，operation 的所有权怎样交给 completion worker，并由 worker 在成功或失败 completion 中统一回收？

阶段四将引入：

- completion port。
- completion worker。
- completion key。
- 普通 completion 与人工控制包。
- worker 退出。

这些概念建立在本阶段的 operation 生命周期之上。

---

## 24. 官方资料

阅读时只关注参数、返回值和内存必须存活多久：

- [WSASocketW](https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-wsasocketw)
- [WSARecv](https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-wsarecv)
- [WSASend](https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-wsasend)
- [Overlapped I/O and Event Objects](https://learn.microsoft.com/en-us/windows/win32/winsock/overlapped-i-o-and-event-objects-2)
- [WSACreateEvent](https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-wsacreateevent)
- [WSAResetEvent](https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-wsaresetevent)（仅用于 19.1 节的复用方案）
- [WSACloseEvent](https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-wsacloseevent)
- [WSAWaitForMultipleEvents](https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-wsawaitformultipleevents)
- [WSAGetOverlappedResult](https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-wsagetoverlappedresult)

进入阶段四前，必须准确回答：

> `WSARecv()` 返回后，`OVERLAPPED`、真实 buffer 和关联的 `ConnectionContext` 分别由谁保证存活？应用通过什么步骤确认 operation 已最终完成并可以安全释放？
