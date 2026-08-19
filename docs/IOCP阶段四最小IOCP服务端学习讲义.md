# IOCP 阶段四：最小 IOCP 服务端学习讲义

> 贯穿项目：`D:\CodeRepository\claude\remote_control`  
> 前置讲义：[`IOCP阶段三OverlappedIO学习讲义.md`](./IOCP阶段三OverlappedIO学习讲义.md)  
> 适用环境：Windows、MSVC、C++17  
> 本讲义只提供关键代码片段和推演，不要求在当前项目中创建、构建或运行额外示例工程。

## 1. 阶段目标

完成本阶段后，应能够独立回答：

1. Overlapped I/O 与 IOCP 分别解决什么问题？
2. completion port、socket、completion key、`OVERLAPPED` 和 worker 是什么关系？
3. 如何创建 completion port，并把 socket 关联到它？
4. `GetQueuedCompletionStatus()` 的每个参数和四种结果组合分别表示什么？
5. 为什么 `GetQueuedCompletionStatus()==FALSE` 时仍可能必须回收 operation？
6. completion key 与 `OVERLAPPED*` 的生命周期范围有什么区别？
7. 为什么 worker 不固定服务某一条连接？
8. 如何使用 `PostQueuedCompletionStatus()` 让 worker 正常退出？
9. 为什么退出前必须先排空已投递操作？
10. 为什么 completion port 必须最后关闭？

建议投入 8～12 小时。

---

## 2. 前置知识检查

进入阶段四前，应已经掌握阶段三的以下内容：

- socket 使用 `WSA_FLAG_OVERLAPPED` 创建。
- 一个在途操作独占一个 `OVERLAPPED`。
- `WSABUF` 不拥有真实 buffer。
- operation 必须拥有或保活 buffer。
- `WSA_IO_PENDING` 是正常异步提交结果。
- 立即成功与同步投递失败的区别。
- 取消后仍要取得最终完成结果。
- send completion 可能只完成部分数据。

如果上述任意一点仍不清楚，应先回到阶段三。IOCP 只改变“从哪里取得完成通知”，不会自动解决 operation 生命周期问题。

---

## 3. Overlapped I/O 与 IOCP 的分工

### 3.1 阶段三：事件通知

```text
一个 socket
  → 提交 WSARecv
  → 一个 OVERLAPPED.hEvent
  → 一个线程等待 event
  → WSAGetOverlappedResult
```

事件模型容易理解，但如果每个连接、每个操作都需要独立等待线程，就不适合大量连接。

### 3.2 阶段四：completion port

```text
Socket A ─┐
Socket B ─┼─→ Completion Port ─→ Worker 1
Socket C ─┤                    ├→ Worker 2
Socket D ─┘                    └→ Worker 3
```

多个支持 Overlapped I/O 的 handle 可以关联到同一个 completion port。应用仍然预先提交 `WSARecv()`、`WSASend()`，但完成后不再等待每个 operation 的 event，而是由少量 worker 从 port 统一取得 completion packet。

### 3.3 一句话区分

> Overlapped I/O 解决“先提交具体操作，稍后取得结果”；IOCP 解决“很多 handle 的完成结果由少量 worker 从哪里统一取得”。

IOCP 不是 socket，也不是业务任务池。它不会自动解析 Packet、保证业务顺序、执行文件读取或完成连接状态管理。

---

## 4. IOCP 中的核心对象

| 对象 | 生命周期范围 | 作用 |
| --- | --- | --- |
| completion port | 服务端级 | 汇聚多个 handle 的完成通知 |
| socket | 监听端或连接级 | 表示一个网络 handle |
| completion key | handle 级 | socket 关联 port 时设置，随该 handle 的 completion 返回 |
| `OVERLAPPED*` | operation 级 | 标识某一次 accept、receive 或 send |
| operation buffer | operation 级 | 保存接收数据或待发送数据 |
| connection | 连接级 | 保存 socket、协议状态、队列和锁 |
| completion worker | 服务端级 | 从 port 取包并推进状态 |

### 4.1 关系图

```text
CompletionPort
  ├─ associated Socket A ─→ completion key A
  │       ├─ ReceiveOperation A1 ─→ OVERLAPPED* A1
  │       └─ SendOperation A2    ─→ OVERLAPPED* A2
  │
  └─ associated Socket B ─→ completion key B
          └─ ReceiveOperation B1 ─→ OVERLAPPED* B1

Workers
  ├─ Worker 1 从同一个 port 取任意 completion
  └─ Worker 2 从同一个 port 取任意 completion
```

一个 worker 不属于某个 socket。某条连接的 receive completion 可能由 Worker 1 处理，下一次 send completion 可能由 Worker 2 处理。

### 4.2 completion packet 的关键字段

worker 取得 completion 时会得到：

```text
success/error        操作最终成功还是失败
transferredBytes     本次完成字节数
completionKey        handle 关联时设置的值
OVERLAPPED*          具体 operation
```

其中：

- completion key 属于 handle。
- `OVERLAPPED*` 属于一次操作。
- `transferredBytes` 只属于这一次完成。
- error 必须在失败后立即读取。

---

## 5. 创建 completion port

使用 `CreateIoCompletionPort()` 创建空 port：

```cpp
HANDLE completionPort{CreateIoCompletionPort(
    INVALID_HANDLE_VALUE,
    nullptr,
    0,
    0)};

if (completionPort == nullptr)
{
    DWORD const error{GetLastError()};
    reportWin32Error(error);
}
```

### 5.1 `CreateIoCompletionPort()` 参数说明

函数原型：

```cpp
HANDLE CreateIoCompletionPort(
    HANDLE FileHandle,
    HANDLE ExistingCompletionPort,
    ULONG_PTR CompletionKey,
    DWORD NumberOfConcurrentThreads);
```

该函数有两个用途：

1. `FileHandle == INVALID_HANDLE_VALUE`：创建新的 completion port。
2. `FileHandle` 是有效 handle：把 handle 关联到已有 completion port。

创建空 port 时的参数：

| 参数 | 示例值 | 作用 |
| --- | --- | --- |
| `FileHandle` | `INVALID_HANDLE_VALUE` | 表示当前调用只创建 port，不关联真实 handle。 |
| `ExistingCompletionPort` | `nullptr` | 表示创建新的 completion port。 |
| `CompletionKey` | `0` | 创建空 port 时没有 handle，此参数被忽略，通常传 `0`。 |
| `NumberOfConcurrentThreads` | `0` | 允许同时运行的 completion worker 上限。`0` 表示由系统按处理器数量选择默认值。 |

返回值：

- 成功：返回 completion port 的 `HANDLE`。
- 失败：返回 `nullptr`，立即调用 `GetLastError()`。

错误辅助函数：

| 函数 | 参数 | 返回值与作用 |
| --- | --- | --- |
| `GetLastError()` | 无 | 返回当前线程最近一次 Win32 错误码。必须在失败 API 后立即保存。 |
| `reportWin32Error(_error)` | `_error` 是已经保存的 `DWORD` 错误码 | 记录或格式化错误，不在内部重新猜测前一个 API 的 last error。 |

### 5.2 并发值不等于 worker 数量

`NumberOfConcurrentThreads` 不会创建线程。应用仍需自行创建 worker：

```cpp
std::vector<std::thread> workers;
workers.emplace_back([completionPort] { runCompletionWorker(completionPort); });
workers.emplace_back([completionPort] { runCompletionWorker(completionPort); });
```

概念区别：

| 概念 | 谁决定 | 含义 |
| --- | --- | --- |
| worker 数量 | 应用 | 实际创建多少个线程调用 `GetQueuedCompletionStatus()` |
| concurrency value | completion port | 同时允许多少个关联线程处于运行状态 |

最小练习可以创建 2 个 worker。项目根据硬件把 worker 数量限制在配置的最小值与最大值之间。

### 5.3 `std::thread` 示例函数说明

| 函数 | 参数 | 返回值与作用 |
| --- | --- | --- |
| `workers.emplace_back(_callable)` | `_callable` 是线程入口，这里调用 `runCompletionWorker(completionPort)` | 在容器末尾构造并启动一个 `std::thread`。 |
| `thread.joinable()` | 无 | 返回线程对象当前是否拥有可 `join()` 的执行线程。 |
| `thread.join()` | 无 | 阻塞等待该线程入口返回；调用后线程对象不再 joinable。 |

---

## 6. 把 socket 关联到 completion port

仍然使用 `CreateIoCompletionPort()`：

```cpp
ULONG_PTR const completionKey{0};

HANDLE associatedPort{CreateIoCompletionPort(
    reinterpret_cast<HANDLE>(socketHandle),
    completionPort,
    completionKey,
    0)};

if (associatedPort == nullptr)
{
    DWORD const error{GetLastError()};
    reportWin32Error(error);
}
```

### 6.1 关联模式参数说明

| 参数 | 示例值 | 作用与约束 |
| --- | --- | --- |
| `FileHandle` | `reinterpret_cast<HANDLE>(socketHandle)` | 要关联的 socket handle。socket 必须支持 Overlapped I/O，且不能已经关联到另一个 port。 |
| `ExistingCompletionPort` | `completionPort` | 已创建的目标 completion port。 |
| `CompletionKey` | `completionKey` | handle 级用户值。该 socket 的 completion 会返回同一个 key。 |
| `NumberOfConcurrentThreads` | `0` | 关联已有 port 时不重新设置其并发值，通常传 `0`。 |

成功时返回传入的 `completionPort`；失败返回 `nullptr`。

### 6.2 关联不转移 socket 所有权

completion port 不负责调用 `closesocket()`：

```text
ConnectionContext owns socket
CompletionPort observes/associates socket handle
```

关闭 port 也不能代替关闭连接 socket。socket 和 port 必须分别释放。

### 6.3 一个 handle 只能关联一次

socket 一旦关联到某个 completion port，在该 handle 生命周期内不能改关联到另一个 port。

正确顺序：

```text
创建 overlapped socket
  → 关联 completion port
  → 投递 WSARecv/WSASend
```

不要先投递依赖 IOCP 的操作，再补做关联。

---

## 7. completion key 的作用

### 7.1 handle 级上下文

关联 Socket A 时设置 Key A，此后 Socket A 的 completion 都返回 Key A：

```text
Socket A + Key A
  ├─ Receive A1 completion → Key A + OVERLAPPED* A1
  └─ Send A2 completion    → Key A + OVERLAPPED* A2
```

completion key 不能区分 A1 和 A2；具体 operation 由 `OVERLAPPED*` 区分。

### 7.2 key 可以存什么

常见选择：

- 连接对象指针。
- 稳定的连接 ID。
- handle 类型标识。
- `0`，把全部上下文放入 operation。

如果把指针放入 key，被指向对象必须活到该 handle 不再产生 completion。错误的 key 生命周期同样会造成悬空指针。

### 7.3 项目的选择

项目关联 socket 时使用 key `0`：

```cpp
CreateIoCompletionPort(
    reinterpret_cast<HANDLE>(acceptedSocket),
    this->m_completionPort,
    0,
    0);
```

连接上下文保存在 `IoOperation::connection`，操作类型保存在 `IoOperation::type`。只有人工退出包使用保留的 `StopCompletionKey`。

优点：所有 operation 级状态集中在 `IoOperation`，不依赖 key 中的裸连接指针。

---

## 8. 投递 Overlapped I/O

阶段三已经完整讲解 `WSARecv()` 和 `WSASend()` 的参数。本阶段只关注 IOCP 模式下的所有权变化。

### 8.1 operation 结构

```cpp
enum class IoOperationType
{
    Receive,
    Send,
};

struct IoOperation final : OVERLAPPED
{
    IoOperation(IoOperationType _type,
                std::shared_ptr<ConnectionContext> _connection,
                std::vector<char> _storage)
        : OVERLAPPED{},
          type{_type},
          connection{std::move(_connection)},
          storage{std::move(_storage)}
    {
        this->nativeBuffer.buf = this->storage.data();
        this->nativeBuffer.len = static_cast<ULONG>(this->storage.size());
    }

    IoOperationType type{IoOperationType::Receive};
    std::shared_ptr<ConnectionContext> connection;
    std::vector<char> storage;
    std::size_t sendOffset{0};
    WSABUF nativeBuffer{};
};
```

构造函数参数：

| 参数 | 作用 | 生命周期要求 |
| --- | --- | --- |
| `_type` | 指定 receive 或 send，worker 据此分发 completion。 | 必须与实际提交的 Winsock API 一致。 |
| `_connection` | 关联连接上下文。 | operation 持有 `shared_ptr`，保证 connection 活到 completion。 |
| `_storage` | receive 容量或 send 数据。 | 移入 operation 后，在途期间不能改变导致地址失效。 |

### 8.2 IOCP 模式的所有权时间线

```text
提交前：local unique_ptr<IoOperation>
  → tryBeginOperation() 增加 pending
  → release() 得到 OVERLAPPED* 并调用 WSARecv/WSASend

同步投递失败：
  → local unique_ptr 重新 reset(raw pointer)
  → finishOperation()

立即成功或 WSA_IO_PENDING：
  → completion packet 进入 port
  → worker 取得 OVERLAPPED*
  → unique_ptr<IoOperation> 重新接管
  → handler
  → 析构或再次提交
```

### 8.3 立即成功仍走 completion

项目没有启用“成功时跳过 completion”的模式。因此 socket 关联 port 后：

```text
WSARecv/WSASend 返回 0
  → 操作立即完成
  → completion packet 默认仍进入 port
  → worker 统一回收 operation
```

提交线程不能因为返回 `0` 就释放 operation，否则 worker 稍后得到悬空 `OVERLAPPED*`。

---

## 9. 从 completion port 取包

关键 API 是 `GetQueuedCompletionStatus()`：

```cpp
DWORD transferredBytes{0};
ULONG_PTR completionKey{0};
OVERLAPPED* overlapped{nullptr};

BOOL const success{GetQueuedCompletionStatus(
    completionPort,
    &transferredBytes,
    &completionKey,
    &overlapped,
    INFINITE)};
```

### 9.1 `GetQueuedCompletionStatus()` 参数说明

函数原型：

```cpp
BOOL GetQueuedCompletionStatus(
    HANDLE CompletionPort,
    LPDWORD lpNumberOfBytesTransferred,
    PULONG_PTR lpCompletionKey,
    LPOVERLAPPED* lpOverlapped,
    DWORD dwMilliseconds);
```

| 参数 | 示例值 | 作用 |
| --- | --- | --- |
| `CompletionPort` | `completionPort` | 要等待的 completion port handle。 |
| `lpNumberOfBytesTransferred` | `&transferredBytes` | 输出该 completion 的传输字节数。失败时只有在确认取到了具体 I/O completion 后，才能按对应错误语义解释。 |
| `lpCompletionKey` | `&completionKey` | 输出 handle 关联时设置的 key，或人工投递包中指定的 key。 |
| `lpOverlapped` | `&overlapped` | 输出具体 operation 的 `OVERLAPPED*`。人工控制包可以故意传回 `nullptr`。 |
| `dwMilliseconds` | `INFINITE` | 等待时间，单位毫秒。`INFINITE` 表示一直等待；有限值超时后函数返回 `FALSE`。 |

返回值：

- `TRUE`：成功取出一个成功 I/O completion 或人工投递包。
- `FALSE`：可能取出了失败/取消 I/O completion，也可能没有取到 operation。必须结合 `overlapped` 判断。

失败后应立即保存：

```cpp
DWORD const error{success == TRUE ? ERROR_SUCCESS : GetLastError()};
```

不要先调用日志、容器或其他 Win32 函数，再读取 last error。

### 9.2 四种结果组合

| `success` | `overlapped` | 含义 | 必须动作 |
| --- | --- | --- | --- |
| `TRUE` | 非空 | 普通 I/O 成功完成 | 接管 operation，按类型处理，最终回收 |
| `FALSE` | 非空 | I/O 失败或被取消，但 completion 已取出 | 保存错误码，接管并回收 operation，进入失败/关闭路径 |
| `TRUE` | 空 | 常见于 `PostQueuedCompletionStatus()` 人工控制包 | 根据 completion key 处理控制消息 |
| `FALSE` | 空 | 超时、port 被关闭或等待本身失败 | 根据错误码和 worker 生命周期处理；没有普通 operation 可回收 |

最重要的判断：

> `success == FALSE` 不等于“什么都没取到”；只要 `overlapped != nullptr`，就有一个 operation 必须处理和释放。

### 9.3 超时与 port 关闭

使用有限超时时间且没有 completion 时：

```text
success = FALSE
overlapped = nullptr
GetLastError() = WAIT_TIMEOUT
```

等待中的 worker 遇到 completion port 被关闭时，通常返回失败、`overlapped == nullptr`，错误表示 port 等待已被放弃。正常停机优先使用人工退出包；关闭 port 只作为最后释放或异常唤醒手段。

---

## 10. 最小 completion worker

### 10.1 worker 函数契约

```cpp
void runCompletionWorker(HANDLE _completionPort);
```

| 参数 | 作用 | 生命周期要求 |
| --- | --- | --- |
| `_completionPort` | worker 等待的 completion port。 | 必须活到 worker 返回；不能在线程仍等待时按正常路径提前关闭。 |

返回值为 `void`。收到保留的退出包，或 port 已在异常停机路径关闭时，函数返回。

### 10.2 关键代码

```cpp
inline constexpr ULONG_PTR StopCompletionKey{
    static_cast<ULONG_PTR>(-1)};

void runCompletionWorker(HANDLE _completionPort)
{
    while (true)
    {
        DWORD transferredBytes{0};
        ULONG_PTR completionKey{0};
        OVERLAPPED* overlapped{nullptr};

        BOOL const success{GetQueuedCompletionStatus(
            _completionPort,
            &transferredBytes,
            &completionKey,
            &overlapped,
            INFINITE)};
        DWORD const error{success == TRUE ? ERROR_SUCCESS : GetLastError()};

        if (overlapped == nullptr)
        {
            if (success == TRUE)
            {
                if (completionKey == StopCompletionKey)
                {
                    return;
                }

                handleControlPacket(completionKey, transferredBytes);
                continue;
            }

            if (error == ERROR_ABANDONED_WAIT_0)
            {
                return;
            }

            handleWorkerWaitFailure(error);
            continue;
        }

        finishOperation();

        auto operation{
            std::unique_ptr<IoOperation>{static_cast<IoOperation*>(overlapped)}};

        switch (operation->type)
        {
            case IoOperationType::Receive:
                handleReceiveCompletion(
                    std::move(operation), success == TRUE, transferredBytes, error);
                break;

            case IoOperationType::Send:
                handleSendCompletion(
                    std::move(operation), success == TRUE, transferredBytes, error);
                break;
        }
    }
}
```

### 10.3 示例辅助函数参数

| 函数 | 参数 | 作用 |
| --- | --- | --- |
| `finishOperation()` | 无 | pending 减少一次；每个取出的普通 operation 恰好调用一次。 |
| `handleControlPacket(_completionKey, _transferredBytes)` | 人工包的 key 和自定义字节数字段 | 处理非退出类控制包；最小服务端可以不定义其他控制包。 |
| `handleWorkerWaitFailure(_error)` | `_error` 是 `GetLastError()` 的保存值 | 处理超时或普通等待失败；该路径没有普通 operation。 |
| `handleReceiveCompletion(_operation, _success, _transferredBytes, _error)` | operation 唯一所有权、成功标志、字节数、错误码 | 处理 receive 的成功、断线、失败或取消，并决定是否投递下一操作。 |
| `handleSendCompletion(_operation, _success, _transferredBytes, _error)` | operation 唯一所有权、成功标志、字节数、错误码 | 更新发送进度，必要时继续部分发送或完成发送项。 |

### 10.4 为什么先恢复 `unique_ptr`

worker 从 Windows 得到的只是 `OVERLAPPED*`。通过继承关系恢复 `IoOperation*` 后，应尽快由 RAII 接管：

```cpp
auto operation{
    std::unique_ptr<IoOperation>{static_cast<IoOperation*>(overlapped)}};
```

此后即使 handler 提前返回或抛出项目允许的异常，operation 也有明确回收路径。

---

## 11. receive completion

最小处理逻辑：

```cpp
void handleReceiveCompletion(std::unique_ptr<IoOperation> _operation,
                             bool _success,
                             DWORD _transferredBytes,
                             DWORD _error)
{
    std::shared_ptr<ConnectionContext> const connection{
        _operation->connection};

    if (!_success)
    {
        closeConnection(connection, _error);
        return;
    }

    if (_transferredBytes == 0)
    {
        closeConnection(connection, ERROR_SUCCESS);
        return;
    }

    connection->receiveBuffer.insert(
        connection->receiveBuffer.end(),
        _operation->storage.begin(),
        _operation->storage.begin() + _transferredBytes);

    processReceivedBytes(connection);
    postReceive(connection);
}
```

### 11.1 参数说明

| 参数 | 作用 |
| --- | --- |
| `_operation` | 当前 receive operation 的唯一所有权，同时保活 connection 和 receive buffer。 |
| `_success` | completion 是否成功。`false` 包括网络失败和取消。 |
| `_transferredBytes` | 成功时的有效接收字节数。只使用 `[0, _transferredBytes)`。 |
| `_error` | 失败时保存的 Win32/Winsock completion 错误；成功时可为 `ERROR_SUCCESS`。 |

业务辅助函数：

| 函数 | 参数 | 作用 |
| --- | --- | --- |
| `closeConnection(_connection, _error)` | `_connection` 是目标连接；`_error` 是触发关闭的错误码 | 进入幂等关闭流程；实际项目还会使用更明确的关闭原因枚举。 |
| `processReceivedBytes(_connection)` | `_connection` 持有累计接收缓冲区 | 从 TCP 字节流中解析零个或多个完整消息。 |
| `postReceive(_connection)` | `_connection` 是仍活动的连接 | 创建并提交下一次 receive operation。 |

### 11.2 零字节完成

TCP receive 成功且 `_transferredBytes == 0` 表示对端正常关闭，不是“当前暂时没有数据”。

### 11.3 下一次 receive

项目在当前 receive completion 处理完后才调用 `postReceive()`，从而保持每连接最多一个 receive 在途。

最小 echo server 可以在收到字节后复制到 send operation，等待 send 完成后再投递下一次 receive。这样控制流最容易验证。

---

## 12. send completion 与部分发送

```cpp
void handleSendCompletion(std::unique_ptr<IoOperation> _operation,
                          bool _success,
                          DWORD _transferredBytes,
                          DWORD _error)
{
    std::shared_ptr<ConnectionContext> const connection{
        _operation->connection};

    if (!_success || _transferredBytes == 0)
    {
        closeConnection(connection, _error);
        return;
    }

    _operation->sendOffset += _transferredBytes;
    if (_operation->sendOffset < _operation->storage.size())
    {
        refreshSendBuffer(*_operation);
        postSend(std::move(_operation));
        return;
    }

    postReceive(connection);
}
```

### 12.1 参数说明

| 参数 | 作用 |
| --- | --- |
| `_operation` | 当前 send operation 的唯一所有权，并拥有完整发送数据。 |
| `_success` | completion 是否成功。 |
| `_transferredBytes` | 本次实际完成的发送字节数，不保证覆盖全部 buffer。 |
| `_error` | completion 失败时的错误码。 |

### 12.2 辅助函数参数

| 函数 | 参数 | 作用 |
| --- | --- | --- |
| `refreshSendBuffer(_operation)` | `_operation` 是已完成一部分的 send operation | 根据 `sendOffset` 更新 `WSABUF`，只指向剩余字节。 |
| `postSend(_operation)` | `_operation` 是 send operation 的唯一所有权 | 提交下一次 `WSASend()`；成功后回收责任转移到 completion 路径。 |
| `postReceive(_connection)` | `_connection` 是仍然活动的连接 | 为该连接投递下一次 receive。 |

### 12.3 为什么零字节 send 要终止

如果 send completion 成功但字节数为零，继续原样重投会形成无进展循环。项目把它视为连接无法继续发送并进入关闭路径。

---

## 13. 最小 IOCP echo 链路

阶段四第一版可以继续使用同步 `accept()`，只把 receive/send 改成 IOCP：

```text
主线程同步 accept
  → accepted socket 关联 completion port
  → 创建 ConnectionContext
  → postReceive

Worker 取得 receive completion
  → 复制有效字节到 send operation
  → postSend

Worker 取得 send completion
  ├─ 部分发送：继续 postSend
  └─ 全部发送：postReceive

对端关闭或 I/O 失败
  → 幂等关闭 connection
  → 取消/关闭 socket
  → 取消 completion 仍由 worker 回收
```

### 13.1 为什么先保留同步 `accept()`

这样可以一次只增加一个变量：

- TCP 建连仍沿用阶段二。
- operation 生命周期沿用阶段三。
- 本阶段只新增 port、关联、worker 和 completion packet。

稳定后再在阶段五把同步 `accept()` 替换为 `AcceptEx()`。

### 13.2 accepted socket 处理函数契约

```cpp
bool registerAcceptedSocket(
    HANDLE _completionPort,
    SOCKET _acceptedSocket,
    ULONG_PTR _completionKey);
```

| 参数 | 作用 |
| --- | --- |
| `_completionPort` | 目标 completion port。 |
| `_acceptedSocket` | 同步 `accept()` 返回、支持 Overlapped I/O 的连接 socket。 |
| `_completionKey` | 该 socket 的 handle 级 key；项目风格可使用 `0`。 |

返回 `true` 表示关联成功；返回 `false` 时调用者仍拥有 socket，并负责关闭它。

---

## 14. 人工投递 worker 退出包

使用 `PostQueuedCompletionStatus()`：

```cpp
BOOL const posted{PostQueuedCompletionStatus(
    completionPort,
    0,
    StopCompletionKey,
    nullptr)};

if (posted == FALSE)
{
    DWORD const error{GetLastError()};
    reportWin32Error(error);
}
```

### 14.1 `PostQueuedCompletionStatus()` 参数说明

函数原型：

```cpp
BOOL PostQueuedCompletionStatus(
    HANDLE CompletionPort,
    DWORD dwNumberOfBytesTransferred,
    ULONG_PTR dwCompletionKey,
    LPOVERLAPPED lpOverlapped);
```

| 参数 | 示例值 | 作用 |
| --- | --- | --- |
| `CompletionPort` | `completionPort` | 接收人工 completion packet 的 port。 |
| `dwNumberOfBytesTransferred` | `0` | 自定义字节数字段。退出包不需要传输字节，因此传 `0`。 |
| `dwCompletionKey` | `StopCompletionKey` | 自定义 key。worker 用它识别退出控制包。 |
| `lpOverlapped` | `nullptr` | 自定义 operation 指针。退出包没有普通 I/O operation，因此传空。 |

返回值：

- 成功：返回非零，packet 已进入 port。
- 失败：返回 `FALSE`，立即调用 `GetLastError()`。

### 14.2 为什么每个 worker 需要一个退出包

一个 packet 只会唤醒并交给一个 worker：

```cpp
for (std::size_t index{0}; index < workers.size(); ++index)
{
    PostQueuedCompletionStatus(
        completionPort,
        0,
        StopCompletionKey,
        nullptr);
}
```

循环参数：

| 值 | 作用 |
| --- | --- |
| `workers.size()` | 当前需要退出的 worker 数量。 |
| `index` | 只用于控制投递次数，不写入 completion packet。 |

### 14.3 人工包与 I/O completion 的区别

`PostQueuedCompletionStatus()` 不会替应用执行 I/O，也不会自动改变 pending 计数。它只是把指定的三个值放入 completion queue。

因此退出包必须与普通 operation 分开：

```text
退出包：overlapped == nullptr，key == StopCompletionKey
普通包：overlapped != nullptr
```

---

## 15. 关闭 completion port

最终使用 `CloseHandle()`：

```cpp
BOOL const closed{CloseHandle(completionPort)};
if (closed == FALSE)
{
    DWORD const error{GetLastError()};
    reportWin32Error(error);
}
completionPort = nullptr;
```

### 15.1 `CloseHandle()` 参数说明

函数原型：

```cpp
BOOL CloseHandle(HANDLE hObject);
```

| 参数 | 作用 |
| --- | --- |
| `hObject` | 要关闭的内核对象 handle。这里必须是 completion port，而不是 `SOCKET`。 |

返回非零表示成功，返回 `FALSE` 时调用 `GetLastError()`。

### 15.2 为什么不能提前关闭

提前关闭 port 会破坏 operation 的最终回收路径：

```text
仍有 WSARecv/WSASend 在途
  → CloseHandle(completionPort)
  → 后续完成/取消结果无法按正常 worker 路径消费
  → IoOperation、buffer、connection 可能泄漏或悬空
```

completion port 必须晚于：

1. 禁止新 I/O。
2. 取消或关闭 socket。
3. 排空所有普通 operation。
4. worker 收到退出包。
5. `join()` 所有 worker。

### 15.3 `CloseHandle()` 不关闭 socket

completion port 与 socket 是不同资源：

- port 使用 `CloseHandle()`。
- socket 使用 `closesocket()`。

关闭 port 不会替应用正确完成每条连接的协议关闭和 socket 释放。

---

## 16. 最小安全停机顺序

```text
1. 设置 stopping，禁止新 operation 注册
2. 停止同步 accept 或关闭监听 socket
3. 对所有活动连接发起关闭
4. CancelIoEx/closesocket 使在途 I/O 最终完成或取消
5. completion workers 继续消费普通 completion
6. 等待 pending operation 计数归零
7. 为每个 worker 投递一个 StopCompletionKey 包
8. join 所有 workers
9. CloseHandle(completionPort)
```

阶段四只需要理解该顺序。连接状态机、任务池停止和完整竞态证明将在阶段九深入学习。

### 16.1 pending 的作用

```text
pending > 0
  → 至少有 operation 尚未走完最终 completion
  → worker 不能全部退出
  → port 不能关闭

pending == 0
  → 所有成功提交的普通 operation 都已取得最终 completion
```

pending 为零后仍要投递退出包并 `join()`，因为 worker 可能正在执行最后一个 handler 的用户态代码。

---

## 17. completion 顺序不是业务顺序

不能假设：

```text
先调用 WSARecv A
再调用 WSASend B
所以一定先处理 A completion，再处理 B completion
```

原因：

1. 不同 operation 在内核中独立完成。
2. 不同 socket 的网络进度不同。
3. 多个 worker 并发处理 packet。
4. worker 被唤醒后执行速度不同。
5. 业务可能在 handler 中继续投递新 operation。

IOCP queue 的实现细节不能替代业务状态机。项目仍需要：

- 每连接 receive/send 在途限制。
- 有序发送队列。
- connection 状态机。
- 锁和原子状态。
- pending 计数。

阶段四的最小 echo server通过“send 完成后再 post 下一次 receive”简化业务顺序。后续再逐步增加并发。

---

## 18. 映射到项目启动流程

### 18.1 `start(_port)` 函数契约

```cpp
bool RemoteControlTransport::Impl::start(quint16 _port);
```

| 参数 | 作用 |
| --- | --- |
| `_port` | 要监听的 TCP 端口。传 `0` 时可由系统分配临时端口。 |

返回 `true` 表示 completion port、监听端、worker 和初始异步操作全部建立；任一步失败返回 `false` 并调用 `stop()` 回滚已创建资源。

### 18.2 创建 port

项目代码：

```cpp
this->m_completionPort = CreateIoCompletionPort(
    INVALID_HANDLE_VALUE,
    nullptr,
    0,
    0);
```

项目使用系统默认 concurrency value，然后自行根据硬件和配置创建 2～4 个 completion worker。

### 18.3 关联 socket

监听 socket 和每个 accepted socket 都关联到同一 port：

```cpp
CreateIoCompletionPort(
    reinterpret_cast<HANDLE>(acceptedSocket),
    this->m_completionPort,
    0,
    0);
```

项目 key 为 `0`，连接由 `IoOperation::connection` 保活。

### 18.4 worker 入口

```cpp
this->m_completionThreads.emplace_back(
    [this] { this->runCompletionWorker(); });
```

`runCompletionWorker()` 没有形参、返回 `void`，通过 `this->m_completionPort` 取得 port，通过成员函数分发 completion。

### 18.5 项目 worker 的关键顺序

```text
GetQueuedCompletionStatus
  → overlapped 为空：检查 StopCompletionKey 或停机
  → overlapped 非空：finishOperation
  → unique_ptr<IoOperation> 接管
  → 按 operation->type 分发
```

失败 completion 同样经过该路径，不会因为 `success == FALSE` 跳过 operation 回收。

---

## 19. 映射到项目停机流程

项目在普通 operation 排空后：

```cpp
for (std::size_t index{0};
     index < this->m_completionThreads.size();
     ++index)
{
    PostQueuedCompletionStatus(
        this->m_completionPort,
        0,
        StopCompletionKey,
        nullptr);
}
```

然后：

```text
join completion workers
  → clear thread objects
  → CloseHandle(completionPort)
  → completionPort = nullptr
```

如果人工唤醒失败，项目关闭 port 作为异常唤醒手段；正常路径仍优先让每个 worker 收到退出包。

### 19.1 `stop()` 函数契约

`stop()` 没有形参，返回 `void`。它必须是幂等的：多个调用者或析构函数重复调用时，只有第一次执行实际停机，其余调用安全返回。

本阶段重点观察：

- pending 何时归零。
- 退出包投递次数。
- worker 何时返回。
- port 何时关闭。

---

## 20. 常见错误与症状

| 错误 | 典型症状 | 根因 |
| --- | --- | --- |
| socket 未关联 port 就投递 | worker 永远收不到预期 completion | handle 没有正确关联 |
| 把 key 当作 operation | receive/send 类型无法区分 | key 属于 handle，不属于单次操作 |
| 只检查 GQCS 的 `BOOL` | 失败 operation 泄漏 | `FALSE + overlapped 非空` 仍需回收 |
| GQCS 失败后迟读取错误码 | 日志错误码不可信 | last error 被后续调用覆盖 |
| 人工退出包使用普通 operation 指针 | worker 误释放非 I/O 对象 | 控制包与普通包没有协议边界 |
| 只投递一个退出包 | 只有一个 worker 退出 | 一个 packet 只交给一个 worker |
| pending 未归零就让 worker 退出 | completion 无人处理 | 消费者提前停止 |
| worker 未 `join()` 就关闭 port | 等待线程异常返回或竞态 | port 生命周期短于 worker |
| 关闭 port 代替关闭 socket | 连接资源残留 | port 不拥有 socket |
| 认为 IOCP 保证业务顺序 | 响应乱序或状态竞争 | 多 operation、多 worker 并发 |
| 在 worker 中执行慢任务 | 所有连接 completion 延迟 | completion worker 被阻塞 |

---

## 21. 调试记录模板

### 21.1 port 与关联

```text
completionPort HANDLE：
CreateIoCompletionPort 创建错误码：
worker 数量：
concurrency value：

socket 值：
socket completion key：
关联返回 HANDLE：
关联错误码：
```

### 21.2 operation 提交与完成

```text
connection id：
operation 地址：
operation type：
提交线程 ID：
提交返回值：
提交错误码：
提交后 pending：

完成 worker ID：
GQCS success：
GQCS error：
completion key：
OVERLAPPED 地址：
transferredBytes：
完成后 pending：
operation 析构位置：
```

### 21.3 worker 退出

```text
worker 总数：
成功投递退出包数量：
收到退出包的 worker ID：
join 完成数量：
CloseHandle 时 pending：
```

---

## 22. 源码阅读顺序

### 22.1 port 创建与线程启动

阅读：

```text
RemoteControlTransport::Impl::start
```

回答：

1. port 在何时创建？
2. 创建失败如何回滚？
3. 监听 socket 何时关联？
4. worker 数量如何计算？
5. 初始异步操作在 worker 启动前还是后投递？

### 22.2 receive/send 提交

阅读：

```text
postReceive
postSend
tryBeginOperation
finishOperation
```

回答：

1. pending 在 API 前还是后增加？
2. 同步投递失败由谁回收？
3. 立即成功由谁回收？
4. `socketMutex` 保护什么？

### 22.3 worker

阅读：

```text
runCompletionWorker
handleReceiveCompletion
handleSendCompletion
```

回答：

1. `overlapped == nullptr` 如何处理？
2. `success == FALSE` 是否仍恢复 `unique_ptr`？
3. operation 类型在哪里保存？
4. completion key 为什么没有保存连接？

### 22.4 stop

只观察阶段四相关部分：

```text
等待 pending == 0
  → 投递 StopCompletionKey
  → join workers
  → CloseHandle(completionPort)
```

任务池、监听取消和完整关闭竞态留到阶段九。

---

## 23. 练习题

### 23.1 概念题

1. Overlapped I/O 与 IOCP 分别解决什么问题？
2. `CreateIoCompletionPort()` 为什么既能创建 port，又能关联 handle？
3. concurrency value 为什么不等于 worker 数量？
4. completion key 为什么属于 handle？
5. `OVERLAPPED*` 为什么属于 operation？
6. `GetQueuedCompletionStatus()==FALSE` 时为什么仍可能有 operation？
7. 人工退出包为什么使用 `overlapped == nullptr`？
8. 为什么需要为每个 worker 投递退出包？
9. 为什么 pending 为零后仍要 `join()`？
10. 为什么 port 必须最后关闭？

### 23.2 结果矩阵练习

为以下组合写出处理动作：

```text
TRUE  + overlapped 非空
FALSE + overlapped 非空
TRUE  + overlapped 为空 + StopCompletionKey
FALSE + overlapped 为空 + WAIT_TIMEOUT
FALSE + overlapped 为空 + port 已关闭
```

每项必须写明：

- 是否有 operation 要回收。
- 是否调用 `finishOperation()`。
- 是否读取 `transferredBytes`。
- worker 是继续还是退出。

### 23.3 所有权图练习

画出：

```text
RemoteControlTransport::Impl
completionPort
completionThreads
ConnectionRegistry
ConnectionContext
IoOperation
WSABUF
OVERLAPPED
```

在边上标记：

- owns。
- associates。
- keeps alive。
- observes。
- temporarily reclaims。

### 23.4 时序图练习

分别画出：

1. receive pending 后成功 completion。
2. send 部分完成并重新投递。
3. receive 被关闭操作取消。
4. pending 归零后 worker 正常退出。

---

## 24. 参考答案要点

### 24.1 GQCS 失败

`FALSE` 只说明本次返回不是成功 I/O completion。若 `overlapped` 非空，说明失败或取消 operation 的 completion 已经出队，必须接管并回收。

### 24.2 key 与 operation

key 在 handle 关联时设置，同一 socket 的多个 completion 返回同一个 key。`OVERLAPPED*` 在每次投递时指定，区分具体 receive/send。

### 24.3 worker 退出包

一个人工 packet 只由一个 worker 取得。N 个等待 worker 需要 N 个退出包，除非使用关闭 port 的异常退出策略。

### 24.4 pending 与 join

pending 为零说明没有未消费的普通 operation；`join()` 还要等待 worker 完成最后的用户态 handler 并真正返回。

### 24.5 port 最后关闭

port 是 operation 最终 completion 的交付通道，也是 worker 的等待对象。提前关闭会同时破坏 operation 回收和 worker 生命周期。

---

## 25. 阶段验收

### 25.1 API

- [ ] 能逐项说明 `CreateIoCompletionPort()` 的四个参数。
- [ ] 能分别说明创建模式和关联模式。
- [ ] 能逐项说明 `GetQueuedCompletionStatus()` 的五个参数。
- [ ] 能逐项说明 `PostQueuedCompletionStatus()` 的四个参数。
- [ ] 能说明 `CloseHandle()` 在本阶段关闭什么资源。

### 25.2 设计

- [ ] 能画出 port、socket、key、operation、connection 和 worker 的关系。
- [ ] 能解释 worker 为什么不固定属于连接。
- [ ] 能解释 key 与 `OVERLAPPED*` 的区别。
- [ ] 能解释立即成功为什么仍由 IOCP worker 回收。
- [ ] 能解释部分发送如何重新投递。
- [ ] 能解释 IOCP 为什么不保证业务顺序。

### 25.3 停机

- [ ] 能解释 pending 计数保护什么。
- [ ] 能解释失败 completion 为什么也要回收。
- [ ] 能解释为什么每个 worker 需要一个退出包。
- [ ] 能解释为什么先排空、再退出 worker、最后关闭 port。

### 25.4 闭卷复述

不看资料完整讲出：

```text
创建 completion port
  → 创建 worker
  → socket 关联 port
  → 创建 operation 并增加 pending
  → WSARecv/WSASend
  → Windows 投递 completion packet
  → worker 调用 GetQueuedCompletionStatus
  → 无论成功失败都接管普通 operation
  → finishOperation
  → handler 推进 receive/send/close
  → 停机时等待 pending 归零
  → 每个 worker 一个退出包
  → join workers
  → CloseHandle(port)
```

如果无法准确说明 `FALSE + overlapped 非空` 的处理，或不能解释 port 为什么最后关闭，就不要进入阶段五。

---

## 26. 建议学习安排

### 第一次：port 与关联

1. 阅读第 3～7 节。
2. 手工填写 `CreateIoCompletionPort()` 两种模式的参数表。
3. 画出 handle 与 key 的关系。

### 第二次：worker 与结果矩阵

1. 阅读第 9～10 节。
2. 默写 GQCS 四种组合。
3. 推演失败 completion 的 operation 回收。

### 第三次：receive/send 链路

1. 阅读第 11～13 节。
2. 画出最小 echo 时序。
3. 推演一次部分发送。

### 第四次：项目映射与停机

1. 阅读第 14～19 节。
2. 跟踪项目 `start()`、worker 和 `stop()`。
3. 完成所有权图和闭卷复述。

---

## 27. 官方资料

阅读时重点关注参数、返回值、错误码和生命周期：

- [I/O Completion Ports](https://learn.microsoft.com/en-us/windows/win32/fileio/i-o-completion-ports)
- [CreateIoCompletionPort](https://learn.microsoft.com/en-us/windows/win32/api/ioapiset/nf-ioapiset-createiocompletionport)
- [GetQueuedCompletionStatus](https://learn.microsoft.com/en-us/windows/win32/api/ioapiset/nf-ioapiset-getqueuedcompletionstatus)
- [PostQueuedCompletionStatus](https://learn.microsoft.com/en-us/windows/win32/api/ioapiset/nf-ioapiset-postqueuedcompletionstatus)
- [CloseHandle](https://learn.microsoft.com/en-us/windows/win32/api/handleapi/nf-handleapi-closehandle)

进入阶段五前，必须能准确回答：

> 一个 socket 的某次 I/O 完成后，Windows 通过 completion port 返回了哪些信息，应用如何据此找到 operation、connection 和正确的回收路径？
