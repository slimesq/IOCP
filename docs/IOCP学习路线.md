# IOCP 系统学习路线

> 面向 C++17、无 Windows API 编程经验的学习者，以 `remote_control` 项目为贯穿案例。

## 1. 学习目标

完成本路线后，应能够：

1. 解释同步 I/O、非阻塞 I/O、Overlapped I/O 和 IOCP 的差异。
2. 使用 C++17 编写一个最小的 Windows TCP/IOCP 服务端。
3. 跟踪 `remote_control` 服务端从 `AcceptEx`、`WSARecv`、协议处理、`WSASend` 到关闭的完整链路。
4. 判断 `OVERLAPPED`、buffer、socket 和连接对象分别由谁拥有、何时可以释放。
5. 解释项目中的连接状态机、有序发送队列、背压、任务池和文件分批传输。
6. 解释 `CancelIoEx()` 之后为什么仍要排空 completion，以及 `stop()` 的顺序为什么不能随意调整。
7. 能在测试保护下为项目增加一个无系统副作用的协议命令，或安全修改连接状态与关闭逻辑。

本路线不要求先学完整个 Win32。理解项目所需的 Windows API 可以分成两部分：

- 核心部分：Win32 基础约定、Winsock、Overlapped I/O、IOCP、取消与资源释放。
- 外围部分：GDI 截图、鼠标注入、UAC、注册表和窗口管理；理解 IOCP 主链之后再学习。

## 2. 项目定位

目标项目：

```text
D:\CodeRepository\claude\remote_control
```

主要分层：

```text
RemoteControlClient
    └─ Qt QTcpSocket 异步客户端
              │
              ▼
RemoteControlCommon
    └─ Packet、命令、payload 编解码
              │
              ▼
RemoteControlServer
    ├─ Qt 应用生命周期与 Windows 主机能力
    └─ RemoteControl::ServerTransport
           └─ Windows IOCP 传输层
```

需要先建立两个边界：

1. 客户端使用 Qt `QTcpSocket`，并不直接使用 IOCP。
2. IOCP 核心集中在 `server_transport`；`WindowsPlatformIntegration.cpp` 中的 GDI、鼠标、UAC 等属于外围能力。

项目当前使用 C++17。当前快照的 `build/msvc-debug` 中，5 个普通 CTest 已于 2026-08-10 验证通过。后续修改代码后必须重新构建，不能把该结果当作永久基线。

## 3. 推荐学习方法

每个主题固定采用下面的闭环：

```text
概念讲解
  → 最小 C++17 示例
  → 对照项目源码
  → 断点或日志验证
  → 小练习
  → 用自己的话复述
```

不要以“逐行读完文件”为目标。每次阅读都应回答四个问题：

1. 当前代码运行在哪个线程？
2. 当前对象和资源由谁拥有？
3. 异步操作何时开始，何时才算真正结束？
4. 失败、断线或停机时，资源沿什么路径回收？

建议每天学习 1～2 小时，每周安排 5 天。整体约需 55～85 小时，即 6～10 周。若已经熟悉 C++ 智能指针和多线程，可压缩前两个阶段。

## 4. 开始前的 C++17 能力检查

### 4.1 普通语言能力

进入网络部分前，应能读懂：

- 头文件与源文件、声明与定义、命名空间。
- 类、`struct`、构造函数、析构函数和成员初始化列表。
- `const`、引用、指针、值语义。
- 大括号初始化、`enum class`、`static_cast`。
- `[[nodiscard]]`、`noexcept`、`std::optional`。
- `std::vector`、`std::deque`、`std::unordered_map`。

项目练习入口：

- `include/common/Protocol.h`
- `include/common/Packet.h`
- `src/common/Packet.cpp`
- `tests/ProtocolTests.cpp`

### 4.2 生命周期与所有权

必须掌握：

- RAII。
- `std::unique_ptr` 的唯一所有权。
- `std::shared_ptr` 的共享生命周期。
- `std::weak_ptr` 如何避免不必要地延长生命周期。
- `std::move` 和移动语义。
- lambda 的值捕获、引用捕获及其生命周期风险。

项目对应位置：

- PIMPL 与 `unique_ptr`：`server_transport/include/RemoteControlTransport.h:59`
- 连接共享生命周期：`server_transport/internal/RemoteControlTransportImpl.h:245`
- `IoOperation` 所有权：`server_transport/internal/RemoteControlTransportImpl.h:337`
- 后台任务使用 `weak_ptr`：`server_transport/src/RemoteControlTransportProtocol.cpp:317`

### 4.3 并发基础

进入 IOCP 实现前必须掌握：

- `std::thread`、`join()`、线程函数和 lambda。
- `std::mutex`、`std::lock_guard`。
- `std::unique_lock`、`std::condition_variable`。
- `std::atomic` 的 `load()`、`store()`、`exchange()`、`fetch_add()`、`fetch_sub()`。
- `compare_exchange_strong()` 和 `compare_exchange_weak()`。
- 数据竞争、死锁、锁保护范围和幂等操作。

项目中的两个入门案例：

1. 固定任务池：`server_transport/src/RemoteControlTransportRuntime.cpp:139`
2. 原子连接状态机：`server_transport/src/RemoteControlTransportRuntime.cpp:75`

## 5. 四种 I/O 模型的区别

| 模型 | 应用关心的问题 | 典型行为 | 本项目位置 |
| --- | --- | --- | --- |
| 同步阻塞 I/O | “操作何时返回？” | `recv()` 没数据时阻塞线程 | 学习用最小示例 |
| 非阻塞/就绪模型 | “现在能不能读写？” | `select()` 等待 socket 就绪，再调用 `recv()` | 项目未使用 |
| Overlapped I/O | “我先提交一个具体操作，完成后通知我” | 提交 `WSARecv()` 时同时提供 buffer 和 `OVERLAPPED` | 服务端核心 |
| IOCP | “多个 handle 的完成通知统一从哪里取？” | worker 用 `GetQueuedCompletionStatus()` 消费 completion | 服务端核心 |

IOCP 不是 socket，也不是业务线程池。它的核心作用是：

1. 多个支持 Overlapped I/O 的 handle 关联到同一个 completion port。
2. 应用预先投递 `AcceptEx()`、`WSARecv()`、`WSASend()` 等操作。
3. 操作完成后，Windows 向 completion port 放入完成通知。
4. 少量 worker 统一取出通知并推进业务状态。

## 6. 阶段零：建立可运行基线

建议投入：1～2 小时。

### 目标

- 能构建 Debug 版本。
- 能运行普通 CTest。
- 知道哪些测试具有真实系统副作用。

### 操作

```powershell
Set-Location D:\CodeRepository\claude\remote_control

# 配置并构建 Debug
.\scripts\Build.ps1 -Config Debug

# 运行普通测试
ctest --test-dir .\build\msvc-debug --output-on-failure
```

普通 CTest 应包含：

- `RemoteControlProtocolTests`
- `RemoteControlClientWorkerLifecycleTests`
- `RemoteControlTransportLifecycleTests`
- `RemoteControlConnectionStateTests`
- `RemoteControlTransportResilienceTests`

`RemoteControlSmokeTests` 不属于普通 CTest。它会连接真实服务端并涉及屏幕、鼠标、文件打开等行为，只能在受控环境运行。

### 完成标准

- 能说明客户端、服务端、公共协议和 IOCP 静态库之间的关系。
- 能说明为什么初期不运行 smoke test。
- 能使用调试器打开项目源码并设置断点。

## 7. 阶段一：TCP 与公共协议

建议投入：4～6 小时。

### 目标

- 理解 TCP 是字节流，没有 Packet 边界。
- 理解半包、粘包和部分发送。
- 能手工解释项目 Packet 格式。

### 阅读顺序

1. `include/common/Protocol.h`
2. `include/common/Packet.h`
3. `src/common/Packet.cpp`
4. `src/common/Protocol.cpp`
5. `docs/ProtocolReference.md`
6. `tests/ProtocolTests.cpp`

### 必须回答

1. 为什么一次 `recv()` 或 Qt `readyRead` 不一定对应一个 Packet？
2. 为什么一次读取可能包含多个 Packet？
3. `Packet::tryParse()` 为什么接收可变的缓冲区并从中移除已解析数据？
4. checksum、payload 长度和最大长度分别防止什么问题？
5. 为什么 `WSASend()` 完成时仍可能只发送了部分数据？

### 练习

1. 手工写出空 payload 的 `TestConnection` Packet。
2. 为两个连续 Packet 增加粘包测试。
3. 把一个 Packet 分割为多个片段，逐段调用 `tryParse()`。
4. 画出“TCP 字节流缓冲区 → 完整 Packet”的过程。

### 完成标准

- 不再使用“一次读取等于一条消息”的思维理解 TCP。
- 能从十六进制字节说明 Packet 各字段。

## 8. 阶段二：Win32 与同步 Winsock 基础

建议投入：6～10 小时。

### 8.1 Win32 基础约定

只需先学习：

- `BOOL`、`DWORD`、`ULONG_PTR`、`HANDLE`。
- `TRUE`、`FALSE`、`nullptr`。
- `SOCKET`、`INVALID_SOCKET`、`SOCKET_ERROR`。
- 普通 Win32 错误使用 `GetLastError()`。
- Winsock 错误使用 `WSAGetLastError()`。
- `HANDLE` 通常使用 `CloseHandle()`；`SOCKET` 使用 `closesocket()`，两者不能混用。
- `XxxW` 表示 UTF-16 Windows API。
- `WinSock2.h` 应在 `Windows.h` 之前包含。
- `WIN32_LEAN_AND_MEAN`、`NOMINMAX` 用于减少 Windows 头文件宏污染。

项目对应头文件：

```text
server_transport/internal/RemoteControlTransportImpl.h:6-18
```

### 8.2 同步 Winsock 调用链

按顺序理解：

```text
WSAStartup()
  → socket()/WSASocketW()
  → bind()
  → listen()
  → accept()
  → recv()/send()
  → shutdown()
  → closesocket()
  → WSACleanup()
```

还需掌握：

- `sockaddr_in`、`AF_INET`、`INADDR_ANY`。
- `htons()`、`htonl()`、`ntohs()` 与网络字节序。
- 端口 `0` 表示让系统分配临时端口，再用 `getsockname()` 取得结果。

### 最小练习项目

先单独实现一个同步阻塞 echo server，不要一开始就写 IOCP：

```text
客户端发送任意字节
  → 服务端 recv
  → 服务端原样 send
  → 客户端关闭
```

要求：

- 使用 C++17 和 RAII 封装 `WSAStartup/WSACleanup`。
- 为 `SOCKET` 编写简单的资源所有者，析构时调用 `closesocket()`。
- 正确处理 `recv()==0`。
- 循环 `send()`，处理部分发送。
- 所有失败路径输出正确的 Winsock 错误码。

### 项目映射

- Winsock RAII：`server_transport/src/RemoteControlTransportRuntime.cpp:120`
- 地址、绑定和监听：`server_transport/src/RemoteControlTransport.cpp:99-146`
- 连接关闭：`server_transport/src/RemoteControlTransport.cpp:727-762`

### 完成标准

- 能在不看资料时复述同步 TCP server 的生命周期。
- 能区分 `SOCKET_ERROR`、`INVALID_SOCKET` 和对端正常关闭。

## 9. 阶段三：Overlapped I/O

建议投入：6～10 小时。

### 核心概念

Overlapped I/O 的关键不是“不阻塞”三个字，而是操作对象的生命周期：

```text
应用创建 OVERLAPPED + buffer
  → 调用 WSARecv/WSASend 投递操作
  → 函数返回，但内核可能仍在使用这些地址
  → 完成通知到达
  → 应用处理完成结果
  → 此时才可以释放或复用操作对象和 buffer
```

必须理解：

- socket 必须支持 Overlapped I/O；项目使用 `WSASocketW(..., WSA_FLAG_OVERLAPPED)`。
- 每个在途操作必须拥有独立的 `OVERLAPPED`。
- `WSABUF` 只是 `char* + length` 的视图，不拥有数据。
- `WSARecv()` 或 `WSASend()` 返回 `0` 表示投递成功。
- 返回 `SOCKET_ERROR` 且 `WSAGetLastError()==WSA_IO_PENDING` 也表示投递成功。
- 只有其他错误才是同步投递失败。
- 函数调用返回不代表操作对象可以销毁。

### 项目核心类型

阅读：

```text
server_transport/internal/RemoteControlTransportImpl.h:245-367
server_transport/src/RemoteControlTransportRuntime.cpp:329-354
```

重点分析 `IoOperation : OVERLAPPED`：

- `type`：区分 `Accept`、`Receive`、`Send`。
- `connection`：用 `shared_ptr` 保证连接上下文存活。
- `storage`：拥有 accept/receive 缓冲区。
- `sendBytes`：拥有发送数据。
- `sendOffset`：记录部分发送的进度。
- `nativeBuffer`：提供给 Winsock 的非 owning 视图。

### 练习

1. 画出 `IoOperation` 的所有权图。
2. 解释为什么不能在调用 `WSARecv()` 后立即销毁 `IoOperation`。
3. 解释为什么同一个 `OVERLAPPED` 不能同时用于两次在途操作。
4. 修改最小示例，用事件对象等待一次 overlapped receive；此阶段暂时不接 IOCP。

### 完成标准

能够准确回答：

> 当前这个 `OVERLAPPED`、buffer 和 connection 在完成通知到达前由谁保证存活？

## 10. 阶段四：最小 IOCP 服务端

建议投入：8～12 小时。

### 核心 API

按以下顺序学习：

1. `CreateIoCompletionPort(INVALID_HANDLE_VALUE, ...)`：创建空 completion port。
2. `CreateIoCompletionPort(socketHandle, port, key, ...)`：把 socket 关联到 port。
3. `WSARecv()`、`WSASend()`：投递 Overlapped I/O。
4. `GetQueuedCompletionStatus()`：取出完成通知。
5. `PostQueuedCompletionStatus()`：人工投递控制消息，例如 worker 退出通知。
6. `CloseHandle()`：最终释放 completion port。

### 最小工作线程循环

下面是结构示意，不是完整可运行代码：

```cpp
while (true)
{
    DWORD transferredBytes{0};
    ULONG_PTR completionKey{0};
    OVERLAPPED* overlapped{nullptr};

    BOOL const success{GetQueuedCompletionStatus(
        completionPort,
        &transferredBytes,
        &completionKey,
        &overlapped,
        INFINITE)};

    if (overlapped == nullptr)
    {
        // 处理人工退出通知或 completion port 关闭。
        continue;
    }

    auto* operation{static_cast<IoOperation*>(overlapped)};
    // 即使 success == FALSE，也必须回收 operation 并处理失败完成。
}
```

### 必须区分

- `GetQueuedCompletionStatus()==FALSE` 且 `overlapped!=nullptr`：某个 I/O 以失败或取消状态完成，仍有操作对象必须处理。
- `overlapped==nullptr`：没有普通 I/O 操作对象，可能是人工控制包或 port 被关闭。
- completion key 属于 handle；`OVERLAPPED*` 属于一次操作。

项目关联 socket 时把 completion key 设置为 `0`，连接和操作类型保存在 `IoOperation` 中；只有退出通知使用 `StopCompletionKey`。

### 最小练习项目要求

在同步 echo server 的基础上实现 IOCP 版本：

- 一个 completion port。
- 2 个 completion worker。
- 每个连接最多一个 receive 在途。
- 每个连接最多一个 send 在途。
- `IoOperation` 自己拥有 buffer。
- 正确处理部分发送。
- 使用 `PostQueuedCompletionStatus()` 让 worker 退出。
- 关闭前排空所有已投递操作。

第一版可以先用同步 `accept()`，只把 receive/send 改成 IOCP；稳定后再加入 `AcceptEx()`。这样更容易定位错误。

### 完成标准

- 能画出 handle、completion port、operation、connection 和 worker 的关系。
- 能解释 worker 为什么不固定属于某一条连接。
- 能解释完成通知顺序为什么不能直接当作业务顺序。

## 11. 阶段五：`AcceptEx` 与项目启动流程

建议投入：4～6 小时。

### `AcceptEx` 要点

- 它是 Winsock 扩展函数，需要用 `WSAIoctl(SIO_GET_EXTENSION_FUNCTION_POINTER)` 动态取得函数指针。
- 调用前需要预先创建用于接收连接的 socket。
- 地址缓冲区需要为本地地址和远端地址分别额外预留 16 bytes。
- 项目把接收数据长度设为 `0`，因此有连接到达时即可完成，不等待首批业务数据。
- accept 完成后必须设置 `SO_UPDATE_ACCEPT_CONTEXT`。
- accepted socket 还需要单独关联到同一个 completion port。

### 项目阅读顺序

```text
RemoteControlServer::start()
  → RemoteControlTransport::start()
  → RemoteControlTransport::Impl::start()
  → loadAcceptEx()
  → replenishAccepts()
  → postAccept()
  → AcceptEx()
  → runCompletionWorker()
  → handleAcceptCompletion()
  → postReceive()
```

源码位置：

- 应用边界：`src/server/RemoteControlServer.cpp:9-47`
- IOCP 初始化：`server_transport/src/RemoteControlTransport.cpp:82-170`
- 动态加载 `AcceptEx`：同文件 `260-274`
- 预投递 accept：同文件 `276-333`
- completion worker：同文件 `417-450`
- accept 完成处理：同文件 `502-564`

默认会保持 8 个 `AcceptEx` 在途。一次 accept 完成后先补充新 accept 槽位，再初始化 accepted socket。

### 断点建议

依次设置：

```text
RemoteControlTransport::Impl::start
RemoteControlTransport::Impl::loadAcceptEx
RemoteControlTransport::Impl::postAccept
RemoteControlTransport::Impl::runCompletionWorker
RemoteControlTransport::Impl::handleAcceptCompletion
RemoteControlTransport::Impl::postReceive
```

### 完成标准

- 能说明为什么 `AcceptEx()` 调用返回后 `IoOperation` 仍不能释放。
- 能说明为什么要预投递多个 accept。
- 能说明 accepted socket 为什么还要再次关联 IOCP。

## 12. 阶段六：跟踪项目最短业务链 `TestConnection`

建议投入：6～10 小时。

### 完整链路

```text
客户端创建连接
  → 服务端 AcceptEx completion
  → handleAcceptCompletion()
  → postReceive()
  → WSARecv completion
  → handleReceiveCompletion()
  → processReceivedPackets()
  → Packet::tryParse()
  → handleInitialPacket(TestConnection)
  → ConnectionPhase::OneShot
  → sendFinalPacket()
  → enqueuePacket()/enqueueBytes()
  → postSend()
  → WSASend completion
  → handleSendCompletion()
  → closeConnection(RequestComplete)
```

### 服务端断点顺序

```text
server_transport/src/RemoteControlTransport.cpp:502
server_transport/src/RemoteControlTransport.cpp:335
server_transport/src/RemoteControlTransport.cpp:566
server_transport/src/RemoteControlTransportProtocol.cpp:32
server_transport/src/RemoteControlTransportProtocol.cpp:86
server_transport/src/RemoteControlTransportProtocol.cpp:189
server_transport/src/RemoteControlTransport.cpp:656
server_transport/src/RemoteControlTransport.cpp:377
server_transport/src/RemoteControlTransport.cpp:590
server_transport/src/RemoteControlTransport.cpp:727
```

### 每个断点记录

- 当前线程 ID。
- `connection->id`。
- 当前 `ConnectionPhase`。
- 当前操作的 `IoOperationType`。
- `transferredBytes`。
- `receiveBuffer.size()`。
- `queuedSendBytes` 和 `sendPending`。
- 当前持有的锁。

### 完成标准

不看代码也能画出一次 `TestConnection` 的：

- 操作时序。
- 对象所有权。
- 线程切换。
- 正常关闭路径。
- 任一步骤失败后的关闭入口。

## 13. 阶段七：状态机、有序发送和背压

建议投入：8～12 小时。

### 13.1 连接状态机

```text
AwaitingRequest
  ├─→ OneShot
  ├─→ FileTransfer
  ├─→ ScreenStream
  └─→ ControlStream

任意活动状态 → Closing → Closed
```

阅读：

- 定义：`server_transport/internal/RemoteControlTransportImpl.h:65-145`
- 实现：`server_transport/src/RemoteControlTransportRuntime.cpp:75-117`
- 测试：`tests/ConnectionStateMachineTests.cpp`

`tryBeginClosing()` 使用 CAS，确保多个线程同时发现错误、超时或停机时，只有一个线程赢得资源清理权。

### 13.2 有序发送队列

阅读：

```text
server_transport/src/RemoteControlTransport.cpp:590-725
```

重点理解：

- 每条连接最多一个 `WSASend` 在途。
- 新响应在已有发送时只进入 `sendQueue`。
- 完成一个发送后才取下一项。
- 部分发送通过 `sendOffset` 继续投递剩余字节。
- `closeAfterSend` 确保最终响应真正发完后再关闭。
- 每连接发送积压有界，超过限制会触发 `Backpressure` 关闭。

### 练习

1. 为状态机补一个非法重复分类测试。
2. 增加关闭竞争线程数，观察只有一个线程赢得 `Closing`。
3. 画出连续入队三个响应时，`sendPending` 和 `sendQueue` 的变化。
4. 推演第一次 `WSASend` 只完成一半时的状态。

### 完成标准

- 能解释为什么 IOCP 本身不能替代项目的有序发送队列。
- 能解释背压为什么是资源保护机制，而不只是一个错误码。

## 14. 阶段八：任务池、文件传输和屏幕流

建议投入：8～12 小时。

### 为什么不能把所有工作放在 completion worker

completion worker 应尽快完成：

- 取得完成通知。
- 更新少量状态。
- 解析已经到达的数据。
- 投递下一次 I/O 或业务任务。

不应直接执行：

- 大文件读取。
- 递归目录删除。
- Shell 打开文件。
- 截图和 PNG 编码。
- GUI 操作。

否则少量 completion worker 会被阻塞，影响所有连接。

### 项目任务池

- Shell 命令池：默认 2 个 worker。
- 文件池：默认 4 个 worker。
- 截图池：默认 2 个 worker。
- completion worker：根据硬件限制在 2～4 个。

阅读：

- `server_transport/src/RemoteControlTransportRuntime.cpp:139-215`
- `server_transport/src/RemoteControlTransportFileTransfer.cpp`
- `server_transport/src/RemoteControlTransportProtocol.cpp:283-393`

### 文件传输的关键设计

```text
文件 worker 读取有限一批
  → 放入发送队列
  → worker 返回，不等待网络
  → WSASend completion 表示该批发完
  → 再向文件池提交下一批读取
```

目录每批最多 64 项，下载每批最多 64 KiB。慢客户端不会长期占住文件 worker，也不会让服务端无限预读文件。

### 屏幕流的关键设计

- 每条屏幕连接只允许一帧在途。
- 提前到达的额外帧请求最多合并一个。
- 截图和 PNG 编码在截图池执行。
- 16 ms 内不同连接可以共享已序列化帧缓存。

### 完成标准

- 能解释 completion worker 与普通任务池的职责边界。
- 能解释文件传输为什么由“发送完成”驱动下一批生产。
- 能解释为什么屏幕流和控制流使用两条独立连接。

## 15. 阶段九：取消、关闭与安全停机

建议投入：10～15 小时。这是整个路线最重要的阶段。

### 15.1 单连接关闭

阅读：

```text
server_transport/src/RemoteControlTransport.cpp:727-793
```

流程：

```text
tryBeginClosing()
  → 清空发送队列和文件状态
  → 从 ConnectionRegistry 移除
  → socketMutex 下阻止新的 I/O 投递
  → CancelIoEx()
  → shutdown()
  → closesocket()
  → 标记 Closed
```

注意：socket 已经关闭，不表示所有 `OVERLAPPED` 都已经归还。取消的操作仍会以失败 completion 返回；`IoOperation` 持有的 `shared_ptr` 会让连接上下文继续存活。

### 15.2 整体停机顺序

阅读：

```text
server_transport/src/RemoteControlTransport.cpp:173-252
```

项目顺序：

```text
禁止注册新 I/O
  → 取消并关闭监听 socket
  → 关闭所有活动连接
  → 停止截图、文件和 Shell 任务池
  → 停止超时监控线程
  → completion worker 继续排空取消/完成通知
  → 等待 pending I/O 计数归零
  → 为 completion worker 投递退出包
  → join completion workers
  → 关闭 completion port
```

### 15.3 三个必须始终成立的不变量

1. 每条活动连接最多一个 `WSARecv` 和一个 `WSASend` 在途。
2. 每个成功投递的 `OVERLAPPED` 恰好增加一次 pending 计数，并由一次 completion 恰好减少一次。
3. 只有赢得 `Closing` 状态转换的线程可以移除连接并关闭 socket。

### 练习

1. 解释 `CancelIoEx()` 后为什么不能立即释放 `IoOperation`。
2. 推演先关闭 completion port、再取消 socket 会发生什么。
3. 推演不等待 pending 计数归零就退出 worker 会发生什么。
4. 推演两个线程同时调用 `closeConnection()` 时状态机如何防止重复关闭。
5. 阅读并运行：
   - `tests/TransportLifecycleTests.cpp`
   - `tests/TransportResilienceTests.cpp`

### 完成标准

- 能逐步解释 `stop()`，包括每一步保护的资源和竞态。
- 能指出任意两步交换顺序可能造成的悬空指针、丢失 completion、重复关闭或死锁。

## 16. 阶段十：客户端与外围 Windows API

建议投入：8～12 小时。

这一阶段用于理解整个项目，而不是理解 IOCP 本体。

### 客户端最小知识

- Qt 事件循环。
- signal/slot。
- QObject thread affinity。
- `QThread`。
- `Qt::QueuedConnection` 与 `Qt::BlockingQueuedConnection`。
- 异步 `QTcpSocket`。

推荐阅读：

1. `docs/ClientArchitecture.md`
2. `include/client/RemoteClient.h`
3. `src/client/RemoteClient.cpp`
4. `src/client/FileDownloadWorker.cpp`
5. `src/client/ScreenStreamWorker.cpp`
6. `src/client/ControlStreamWorker.cpp`

### 最后学习的外围 Win32 API

- GDI 截图：`GetDC`、`CreateCompatibleDC`、`CreateDIBSection`、`BitBlt`。
- 鼠标：`SetCursorPos`、`INPUT`、`SendInput`。
- 模拟锁屏：`ClipCursor`、`FindWindowW`、`ShowWindow`。
- 文件打开与 UAC：`ShellExecuteW`。
- 管理员身份：`CheckTokenMembership`。
- 进程等待：`OpenProcess`、`WaitForSingleObject`。

主要位于：

```text
src/server/WindowsPlatformIntegration.cpp
src/server/WindowsRemoteControlHostServices.cpp
```

## 17. 推荐源码阅读总顺序

不要一开始就阅读最大的实现文件。建议顺序如下：

1. `README.md`
2. `docs/ProtocolReference.md`
3. `include/common/Protocol.h`
4. `include/common/Packet.h`
5. `src/common/Packet.cpp`
6. `server_transport/include/RemoteControlHostServices.h`
7. `server_transport/include/RemoteControlTransport.h`
8. `server_transport/internal/RemoteControlTransportImpl.h` 中：
   - `IoOperationType`
   - `ConnectionPhase`
   - `ConnectionStateMachine`
   - `ConnectionContext`
   - `ConnectionRegistry`
   - `IoOperation`
9. `server_transport/src/RemoteControlTransportRuntime.cpp` 中：
   - `WinsockRuntime`
   - `ConnectionStateMachine`
   - `IoOperation`
   - `TaskPool`
10. `server_transport/src/RemoteControlTransport.cpp` 中：
    - `start()`
    - `loadAcceptEx()`
    - `postAccept()`
    - `postReceive()`
    - `postSend()`
    - `runCompletionWorker()`
    - 三个 completion handler
11. `server_transport/src/RemoteControlTransportProtocol.cpp`
12. `server_transport/src/RemoteControlTransportFileTransfer.cpp`
13. `server_transport/src/RemoteControlTransport.cpp` 中：
    - `closeConnection()`
    - `tryBeginOperation()`
    - `finishOperation()`
    - `stop()`
14. `tests/ConnectionStateMachineTests.cpp`
15. `tests/TransportLifecycleTests.cpp`
16. `tests/TransportResilienceTests.cpp`
17. 客户端和外围 Windows 能力。

## 18. 八周参考计划

这是每天约 1～2 小时、每周 5 天的参考安排；不需要机械赶进度。

### 第 1 周：C++17 生命周期与 TCP

- Day 1：RAII、`unique_ptr`、移动语义。
- Day 2：`shared_ptr`、`weak_ptr`、lambda 捕获。
- Day 3：TCP 字节流、半包和粘包。
- Day 4：阅读 `Packet` 和协议测试。
- Day 5：手工 Packet 与粘包测试。

### 第 2 周：线程、同步和同步 Winsock

- Day 1：`thread`、`mutex`、`lock_guard`。
- Day 2：`condition_variable` 和任务池。
- Day 3：`atomic` 和状态机 CAS。
- Day 4：Winsock 初始化、socket、bind、listen。
- Day 5：同步 echo server。

### 第 3 周：Overlapped I/O

- Day 1：`OVERLAPPED` 和 `WSABUF`。
- Day 2：`WSARecv` 的三种返回情况。
- Day 3：`WSASend` 和部分发送。
- Day 4：buffer 与操作对象生命周期。
- Day 5：事件对象版 overlapped receive。

### 第 4 周：最小 IOCP

- Day 1：创建和关联 completion port。
- Day 2：`GetQueuedCompletionStatus()`。
- Day 3：多 worker 与 completion key。
- Day 4：IOCP echo server。
- Day 5：退出通知和安全关闭。

### 第 5 周：项目 IOCP 主链

- Day 1：`RemoteControlTransport` 公开边界和 PIMPL。
- Day 2：`start()` 与 `AcceptEx`。
- Day 3：`postReceive()` 与 receive completion。
- Day 4：协议解析和首包分类。
- Day 5：`TestConnection` 全链断点。

### 第 6 周：发送、流控和任务池

- Day 1：单发送在途和发送队列。
- Day 2：部分发送和 `closeAfterSend`。
- Day 3：背压和容量限制。
- Day 4：文件任务池及分批下载。
- Day 5：屏幕流和帧请求合并。

### 第 7 周：关闭与测试

- Day 1：连接状态机和并发关闭。
- Day 2：`CancelIoEx()` 与取消 completion。
- Day 3：pending I/O 计数。
- Day 4：`stop()` 顺序。
- Day 5：生命周期与韧性测试。

### 第 8 周：端到端理解与小修改

- Day 1：客户端四种连接模型。
- Day 2：Qt worker 生命周期。
- Day 3：增加无副作用的 `ServerVersion` 命令设计。
- Day 4：实现、协议测试和 transport 测试。
- Day 5：复盘调用链、线程图和资源生命周期图。

如果某一周的完成标准没有达到，应停下来补齐，不要只按日历推进。

## 19. 渐进式项目练习

按风险从低到高完成：

1. 手算并解析 `TestConnection` Packet。
2. 增加半包或粘包协议测试。
3. 为状态机增加非法转换测试。
4. 写同步 Winsock echo server。
5. 写事件版 Overlapped I/O echo server。
6. 写最小 IOCP echo server。
7. 给同一 `connection_id` 整理 accept、recv、send、close 日志。
8. 为项目增加无系统副作用的 `ServerVersion` 命令。
9. 给新命令补协议测试和 transport 测试。
10. 增加慢客户端或半包断开的故障注入测试。
11. 解释 `stop()` 中任意两步交换可能造成的问题。

所有正式修改都应在测试保护下进行。涉及远程文件执行、删除、屏幕、鼠标和锁定的测试只能在明确授权的受控环境运行。

## 20. 常见误区

1. **把 `WSA_IO_PENDING` 当成失败。** 这是正常的异步投递结果。
2. **函数返回后立即释放 buffer。** 内核可能仍在使用它。
3. **认为关闭 socket 后不会再收到 completion。** 取消操作仍需要被消费。
4. **只检查 `GetQueuedCompletionStatus()` 的 BOOL 返回值。** 还必须检查 `overlapped` 是否为空。
5. **认为 IOCP 自动保证业务顺序。** 项目仍需状态机和每连接发送队列。
6. **在 completion worker 中执行慢任务。** 会拖慢所有连接。
7. **一次 `WSARecv` 对应一个 Packet。** TCP 不提供消息边界。
8. **一次 `WSASend` 一定发送全部数据。** 必须处理部分发送。
9. **把 completion key 和 `OVERLAPPED*` 混为一谈。** 前者属于 handle，后者表示一次操作。
10. **认为 pending 计数为零就可以直接销毁所有 worker。** 仍需让正在执行 handler 的线程退出并 `join()`。
11. **先学 GDI、UAC 等外围 API。** 它们与 IOCP 主链无关，会分散注意力。
12. **直接逐行阅读 `RemoteControlTransport.cpp`。** 应先建立类型、所有权和正常链路。

## 21. 每次学习的笔记模板

建议为每个函数维护下面的记录：

```text
函数名：
所属文件：
执行线程：
调用入口：
主要职责：
读取的共享状态：
修改的共享状态：
持有的锁：
创建/获得的资源：
资源所有者：
异步操作开始条件：
异步操作完成条件：
同步失败路径：
异步失败路径：
停机路径：
下一步调用：
我仍不确定的问题：
```

每学完一个阶段，至少画两张图：

1. 调用时序图。
2. 对象与资源所有权图。

## 22. 官方资料

优先阅读 Microsoft Learn 的概念页和项目实际使用的 API：

- [Getting started with Winsock](https://learn.microsoft.com/en-us/windows/win32/winsock/getting-started-with-winsock)
- [WSAStartup](https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-wsastartup)
- [WSARecv](https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-wsarecv)
- [AcceptEx](https://learn.microsoft.com/en-us/windows/win32/api/mswsock/nf-mswsock-acceptex)
- [I/O Completion Ports](https://learn.microsoft.com/en-us/windows/win32/fileio/i-o-completion-ports)
- [CreateIoCompletionPort](https://learn.microsoft.com/en-us/windows/win32/api/ioapiset/nf-ioapiset-createiocompletionport)
- [GetQueuedCompletionStatus](https://learn.microsoft.com/en-us/windows/win32/api/ioapiset/nf-ioapiset-getqueuedcompletionstatus)
- [PostQueuedCompletionStatus](https://learn.microsoft.com/en-us/windows/win32/api/ioapiset/nf-ioapiset-postqueuedcompletionstatus)
- [Canceling Pending I/O Operations](https://learn.microsoft.com/en-us/windows/win32/fileio/canceling-pending-i-o-operations)

阅读 API 文档时，不要求背诵所有参数。第一次只记录：

1. 成功返回值。
2. 同步失败返回值。
3. 异步 pending 如何表示。
4. 参数指向的内存需要存活多久。
5. 完成或取消后如何取得最终结果。

## 23. 最终验收清单

当下面问题都能独立回答时，才算真正理解项目中的 IOCP：

- [ ] IOCP 与 Overlapped I/O 分别解决什么问题？
- [ ] 为什么 `WSA_IO_PENDING` 是正常结果？
- [ ] 为什么每个在途操作需要独立 `OVERLAPPED`？
- [ ] `IoOperation` 为什么继承 `OVERLAPPED`？
- [ ] `IoOperation` 为什么持有 connection 的 `shared_ptr`？
- [ ] 为什么 `WSABUF` 不负责 buffer 生命周期？
- [ ] 为什么每连接只保留一个 `WSARecv` 和一个 `WSASend` 在途？
- [ ] completion key 为什么没有用于保存项目连接？
- [ ] `GetQueuedCompletionStatus()==FALSE` 时为什么仍可能有操作要处理？
- [ ] `AcceptEx` 完成后为什么需要 `SO_UPDATE_ACCEPT_CONTEXT`？
- [ ] accepted socket 为什么需要关联到 completion port？
- [ ] TCP 半包、粘包和部分发送分别在哪里处理？
- [ ] 为什么文件读取和 PNG 编码不能放在 completion worker？
- [ ] 文件下载为什么由发送完成驱动下一批读取？
- [ ] `tryBeginClosing()` 如何防止重复关闭？
- [ ] `CancelIoEx()` 后为什么仍需排空 completion？
- [ ] pending I/O 计数保护了什么生命周期？
- [ ] 为什么关闭 IOCP 必须晚于排空操作和 worker `join()`？
- [ ] `TestConnection` 从 accept 到 close 的完整调用链是什么？
- [ ] 修改协议命令时需要同步修改哪些模块和测试？

## 24. 第一阶段的立即行动

第一次学习建议只完成以下任务，不要急着进入 `AcceptEx`：

1. 构建项目并运行普通 CTest。
2. 阅读 `Protocol.h`、`Packet.h`、`Packet.cpp`。
3. 手工说明一个 `TestConnection` Packet。
4. 阅读 `TaskPool`，画出提交、唤醒、执行和停止时序。
5. 阅读 `ConnectionStateMachine`，解释 CAS 的唯一获胜者。
6. 编写最小同步 Winsock echo server。

完成上述任务后，再进入 Overlapped I/O。这样遇到 IOCP 问题时，能够区分它究竟来自 TCP、C++ 生命周期、线程同步，还是 Windows 异步 I/O 本身。
