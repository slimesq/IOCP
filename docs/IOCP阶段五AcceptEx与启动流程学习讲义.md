# IOCP 阶段五：AcceptEx 与启动流程学习讲义

> 前置知识：阶段三的 Overlapped operation 生命周期，以及阶段四的 completion port、completion worker、提交所有权转移和 `GetQueuedCompletionStatus()` 结果矩阵。
> 贯穿项目：`D:\CodeRepository\claude\remote_control`。
> 学习范围：把同步 `accept()` 替换为 `AcceptEx()`，完成异步接入、accepted socket 初始化、预投递 accept 和项目启动主链。receive/send 的细节直接沿用阶段四。

## 1. 阶段五学习主线

阶段四已经能够处理一个“已经连接”的 socket：

```text
connected socket
  → 关联 completion port
  → postReceive
  → completion worker 处理 receive/send
```

阶段五继续解决：

> 新连接尚未建立时，怎样把“等待客户端连接”也变成一个 Overlapped operation，并让它的完成通知进入同一个 completion port？

阶段四的结论在这里直接使用：

| 阶段四结论 | 阶段五中的用途 |
| --- | --- |
| 每个 operation 都需要稳定的 `OVERLAPPED` 和相关内存 | accept operation 也要保存 `OVERLAPPED`、地址 buffer 和预创建 socket |
| 提交前先把 operation 所有权交给 completion 路径 | 调用 `AcceptEx()` 前同样先执行 `release()` |
| 立即完成和 pending 都由 completion worker 回收 operation | `AcceptEx()` 返回 `TRUE` 或 `ERROR_IO_PENDING` 时都不能在提交路径释放 operation |
| `FALSE + overlapped != nullptr` 仍表示取到了失败 completion | accept 失败 completion 也要回收 operation、关闭预创建 socket，并补充 accept 槽位 |
| 新 connected socket 必须关联 completion port | accepted socket 完成初始化后仍要单独关联同一个 port |

常用术语：

| 术语 | 含义 |
| --- | --- |
| listening socket | 已经完成 `bind()` 和 `listen()`，负责接收连接请求的 socket。 |
| accepted socket | 用于表示某一条新客户端连接的 socket。`AcceptEx()` 要求调用者提前创建它。 |
| accept operation | 一次独立的异步接入请求，拥有自己的 `OVERLAPPED`、accepted socket 和地址 buffer。 |
| accept slot | 一个已经成功提交、正在等待完成的 accept operation。 |
| accept depth | 服务器希望长期保持的在途 accept operation 数量；项目默认值为 `8`。 |
| 地址 buffer | `AcceptEx()` 用来写入本地地址和远端地址的连续内存。本项目不在接入阶段读取其中的地址，但仍必须按要求提供。 |
| accept context | accepted socket 从 listening socket 继承的上下文；accept 完成后通过 `SO_UPDATE_ACCEPT_CONTEXT` 更新。 |
| replenishment | 一个 accept completion 消耗槽位后，立即提交新的 accept operation，把在途数量恢复到目标值。 |

完整学习主线：

```text
阶段四：同步 accept 得到 connected socket
  → listening socket 关联 completion port
  → 动态取得 AcceptEx 函数指针
  → 为一次 accept 创建 operation、accepted socket 和地址 buffer
  → 提交 AcceptEx
  → accept completion 进入同一个 completion port
  → worker 找回 accept operation
  → 先补充新的 accept slot
  → 设置 SO_UPDATE_ACCEPT_CONTEXT
  → accepted socket 关联 completion port
  → 创建 connection context
  → postReceive
```

建议分五个学习单元推进：

1. **建立接入模型（第 4～6 节）**
   - 解决的问题：listening socket、accepted socket、accept operation 和 port 如何配合。
   - 学完自检：能画出一次 accept 从提交到 completion 的对象关系。
2. **准备提交条件（第 7～9 节）**
   - 解决的问题：如何取得函数指针并准备 operation、buffer 和 socket。
   - 学完自检：能解释 `WSAIoctl()` 的九个实参和地址 buffer 的尺寸。
3. **提交并维持接入能力（第 10～12 节）**
   - 解决的问题：`AcceptEx()` 的参数、提交结果、operation 所有权和 accept depth。
   - 学完自检：能判断三种提交结果，并把在途 accept 补到目标值。
4. **处理完成并初始化连接（第 13～18 节）**
   - 解决的问题：accept completion 怎样更新上下文、关联 port 并开始 receive。
   - 学完自检：能按正确顺序写出 completion handler。
5. **映射项目并综合验收（第 19～21 节）**
   - 解决的问题：怎样把前四个单元对应到项目启动主链。
   - 学完自检：能闭卷复述默认 8 个 accept slot 的持续补充过程。

---

## 2. 知识范围

### 2.1 核心内容

- `LPFN_ACCEPTEX` 和 `WSAID_ACCEPTEX`。
- `WSAIoctl(SIO_GET_EXTENSION_FUNCTION_POINTER)`。
- `AcceptEx()` 的八个参数。
- listening socket 与 accepted socket 的不同职责。
- accept operation、accepted socket 和地址 buffer 的生命周期。
- 本地地址、远端地址分别额外预留 16 bytes。
- `dwReceiveDataLength == 0` 的完成时机。
- `AcceptEx()` 的立即完成、pending 和同步失败。
- accept completion 的 IOCP 分发。
- `SO_UPDATE_ACCEPT_CONTEXT`。
- accepted socket 关联 completion port。
- 预投递多个 accept 与完成后的及时补位。
- 项目从 `RemoteControlServer::start()` 到 `postReceive()` 的启动主链。

### 2.2 后续内容

| 主题 | 后续阶段 |
| --- | --- |
| `Packet::tryParse()`、`TestConnection` 请求与响应 | 阶段六 |
| 连接状态机、有序发送队列、背压 | 阶段七 |
| 阻塞业务任务池 | 阶段八 |
| 全局 pending I/O 计数、取消、并发关闭和安全停机 | 阶段九 |

本阶段只要求在“服务器正常运行、没有同时停机”的条件下理解 accept 主链。项目中的 `m_stopping`、`tryBeginOperation()`、`finishOperation()` 和取消逻辑用于解决更复杂的停机竞态，不作为本阶段代码推导的前置条件。

---

## 3. 学习完成标准

完成本阶段后，应能够：

1. 解释同步 `accept()` 为什么仍是阶段四模型中的阻塞点。
2. 解释 `AcceptEx()` 为什么需要 listening socket 和预创建的 accepted socket。
3. 使用 `WSAIoctl()` 动态取得 `AcceptEx()` 函数指针，并说明九个参数。
4. 计算本项目 accept 地址 buffer 的最小尺寸。
5. 逐个解释 `AcceptEx()` 的八个参数及当前项目传值。
6. 解释 `dwReceiveDataLength == 0` 为什么使 accept 在连接到达后完成，而不等待业务数据。
7. 判断 `TRUE`、`FALSE + ERROR_IO_PENDING` 和其他同步错误下的 operation 所有权。
8. 解释为什么 accept completion 的 `transferredBytes == 0` 不是“对端关闭”。
9. 解释 accept 失败 completion 为什么仍要关闭 accepted socket 并补充槽位。
10. 使用 `SO_UPDATE_ACCEPT_CONTEXT` 完成 accepted socket 上下文更新，并说明 `setsockopt()` 的五个参数。
11. 解释 accepted socket 为什么不会自动继承 listening socket 的 IOCP 关联。
12. 按“补位、更新上下文、关联 port、创建 connection、提交 receive”的顺序处理成功 completion。
13. 解释默认 8 个 accept slot 与 worker 数量、listen backlog、最大连接数之间的区别。
14. 看懂项目 `loadAcceptEx()`、`postAccept()`、`replenishAccepts()` 和 `handleAcceptCompletion()` 的核心链路。

建议投入 4～6 小时。

---

## 4. 同步 `accept()` 是最后一个接入阻塞点

阶段四为了专注 IOCP，把连接建立简化为同步调用：

```cpp
SOCKET const acceptedSocket{accept(listenSocket, nullptr, nullptr)};
if (acceptedSocket == INVALID_SOCKET)
{
    int const errorCode{WSAGetLastError()};
}
```

`accept()` 的三个参数已经属于同步 Winsock 基础：

| 参数 | 当前作用 |
| --- | --- |
| `listenSocket` | 已经执行 `listen()` 的 listening socket。 |
| 第一个 `nullptr` | 不读取客户端地址。 |
| 第二个 `nullptr` | 因为不读取地址，所以也不提供地址长度。 |

如果当前没有连接请求，阻塞 listening socket 上的 `accept()` 会等待。服务器通常需要额外的接入线程：

```text
accept thread
  → 阻塞等待连接
  → accept 返回 connected socket
  → 关联 completion port
  → postReceive
```

这种方式可以工作，但接入完成通知没有进入阶段四已经建立的统一 completion 队列。

`AcceptEx()` 改变的是连接建立方式：

```text
提交 AcceptEx
  → 调用线程继续执行
  → 客户端连接到达
  → listening socket 对应的 completion packet 进入 port
  → 任意 completion worker 处理新连接
```

两种方式的核心差异：

| 对比项 | `accept()` | `AcceptEx()` |
| --- | --- | --- |
| 调用模型 | 同步 | Overlapped |
| 新 socket 的产生方式 | 函数返回新 socket | 调用者提前创建 socket，并传入函数 |
| 完成通知 | 函数返回 | event 或 completion port |
| operation 对象 | 不需要 | 必须提供稳定的 `OVERLAPPED` |
| 地址内存 | 可选输出参数 | 必须提供符合尺寸要求的连续 buffer |
| 多个等待请求 | 通常靠循环或多个线程 | 可预投递多个独立 operation |

重点不是“`AcceptEx()` 永远比 `accept()` 正确”，而是它能够把连接建立纳入已有的 Overlapped I/O 与 IOCP 生命周期模型。

---

## 5. 一次 `AcceptEx` 的核心对象

一次在途 accept 至少涉及以下对象：

```text
listening SOCKET
  ├─ 已完成 bind / listen
  ├─ 已关联 completion port
  └─ 被多个 AcceptEx operation 共享

accept operation
  ├─ 包含 OVERLAPPED
  ├─ 保存一个预创建 accepted SOCKET
  └─ 拥有本地地址区 + 远端地址区

completion port
  └─ 接收 listening socket 上 AcceptEx 的 completion packet
```

### 5.1 listening socket 是共享对象

服务器只有一个 listening socket 时，多个 `AcceptEx()` 都可以使用它：

```text
Accept operation A ─┐
Accept operation B ─┼─> 同一个 listening socket
Accept operation C ─┘
```

它必须在提交 accept 前：

1. 创建为支持 Overlapped I/O 的 socket。
2. 完成 `bind()`。
3. 完成 `listen()`。
4. 关联到 completion port。

### 5.2 每个 accept operation 都必须独立

```text
operation A
  ├─ OVERLAPPED A
  ├─ accepted socket A
  └─ address buffer A

operation B
  ├─ OVERLAPPED B
  ├─ accepted socket B
  └─ address buffer B
```

不能让两个在途 `AcceptEx()` 共用同一个：

- `OVERLAPPED`。
- accepted socket。
- 地址 buffer。

Windows 可能同时写入不同 operation 的状态和地址信息。共用对象会让一次 completion 覆盖另一次 operation 的数据。

### 5.3 此时还没有 connection context

提交 accept 时，客户端连接尚未建立，因此项目中的 accept operation：

```cpp
operation->connection == nullptr;
```

等 accept 成功完成、socket 更新上下文并关联 port 后，才创建 `ConnectionContext` 并提交第一次 receive。

---

## 6. 启动顺序总览

项目的外层启动调用非常薄：

```text
RemoteControlServer::start(_port)
  → RemoteControlTransport::start(_port)
  → RemoteControlTransport::Impl::start(_port)
```

`_port` 表示服务器请求监听的 TCP 端口，外层函数只负责把它传给 transport；真正的 socket 和 IOCP 初始化都在 `Impl::start()` 中完成。

阶段五关注的初始化顺序：

```text
创建 completion port
  → 创建 WSA_FLAG_OVERLAPPED listening socket
  → 设置监听选项
  → bind
  → listen
  → loadAcceptEx
  → listening socket 关联 completion port
  → 创建 completion workers
  → replenishAccepts
  → 验证 accept depth 已达到目标值
```

### 6.1 为什么先 `listen()`，再加载和调用 `AcceptEx()`

`AcceptEx()` 的第一个参数必须是已经进入监听状态的 socket。`loadAcceptEx()` 也使用这个 socket 查询当前 Winsock provider 提供的扩展函数指针。

### 6.2 为什么先关联 listening socket，再提交 accept

accept operation 是在 listening socket 上提交的。只有 listening socket 已关联 completion port，这次 operation 最终完成时，packet 才能进入项目使用的 port。

accepted socket 此时还没有自己的普通 receive/send operation；它要在 accept 成功后另行关联。

### 6.3 为什么项目先启动 worker，再预投递 accept

IOCP 可以暂存尚未被 worker 取出的 packet，因此这不是内核层面的硬性要求。项目采用这个顺序后，任何立即到达的 accept completion 都已经有 worker 可以处理，启动过程更直接。

---

## 7. 动态取得 `AcceptEx()` 函数指针

`AcceptEx()` 是 Winsock 扩展函数。代码使用 `WSAIoctl()` 和 `WSAID_ACCEPTEX` 查询函数指针。

需要的声明来自：

```cpp
#include <winsock2.h>
#include <mswsock.h>
```

成员保存函数指针：

```cpp
LPFN_ACCEPTEX m_acceptExFunction{nullptr};
```

`LPFN_ACCEPTEX` 是与 `AcceptEx()` 参数和返回值匹配的函数指针类型。函数指针必须保存到所有 accept 提交结束之后，不能只存在于 `loadAcceptEx()` 的局部变量中。

### 7.1 `WSAIoctl()` 原型

```cpp
int WSAAPI WSAIoctl(
    SOCKET s,
    DWORD dwIoControlCode,
    LPVOID lpvInBuffer,
    DWORD cbInBuffer,
    LPVOID lpvOutBuffer,
    DWORD cbOutBuffer,
    LPDWORD lpcbBytesReturned,
    LPWSAOVERLAPPED lpOverlapped,
    LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine);
```

九个参数的通用作用：

| 参数 | 作用 |
| --- | --- |
| `s` | 指定要控制或查询的 socket，同时确定对应的 Winsock provider。 |
| `dwIoControlCode` | 指定要执行的 IOCTL 操作。 |
| `lpvInBuffer` | 输入 buffer 的首地址。 |
| `cbInBuffer` | 输入 buffer 的字节数。 |
| `lpvOutBuffer` | 输出 buffer 的首地址。 |
| `cbOutBuffer` | 输出 buffer 的容量，单位为 byte。 |
| `lpcbBytesReturned` | 输出实际写入了多少 byte。 |
| `lpOverlapped` | 可选的 Overlapped operation；同步查询时可使用 `nullptr`。 |
| `lpCompletionRoutine` | 可选 completion routine；当前不使用，传 `nullptr`。 |

### 7.2 加载代码

```cpp
bool RemoteControlTransport::Impl::loadAcceptEx()
{
    GUID acceptExGuid{};
    acceptExGuid = WSAID_ACCEPTEX;

    DWORD bytesReturned{0};
    int const result{WSAIoctl(
        this->m_listenSocket,
        SIO_GET_EXTENSION_FUNCTION_POINTER,
        &acceptExGuid,
        sizeof(acceptExGuid),
        &this->m_acceptExFunction,
        sizeof(this->m_acceptExFunction),
        &bytesReturned,
        nullptr,
        nullptr)};

    return result != SOCKET_ERROR && this->m_acceptExFunction != nullptr;
}
```

当前九个实参的具体含义：

| 实参 | 当前作用 |
| --- | --- |
| `this->m_listenSocket` | 使用 listening socket 对应的 provider 查询扩展函数。 |
| `SIO_GET_EXTENSION_FUNCTION_POINTER` | 表示“根据 GUID 取得扩展函数指针”。 |
| `&acceptExGuid` | 输入 `WSAID_ACCEPTEX`，指明目标是 `AcceptEx()`。 |
| `sizeof(acceptExGuid)` | 输入 buffer 的大小，即一个 `GUID` 的大小。 |
| `&this->m_acceptExFunction` | 接收查询到的 `LPFN_ACCEPTEX`。 |
| `sizeof(this->m_acceptExFunction)` | 输出 buffer 的容量，即函数指针变量自身的大小。 |
| `&bytesReturned` | 接收实际写入输出 buffer 的字节数。 |
| 第一个 `nullptr` | 本次查询按同步方式执行，不提供 `OVERLAPPED`。 |
| 第二个 `nullptr` | 不使用 completion routine。 |

返回值：

- `0`：查询成功。
- `SOCKET_ERROR`：查询失败，立即调用 `WSAGetLastError()` 取得错误码。

`loadAcceptEx()` 只取得函数入口，不会提交 accept，也不会产生 accept completion。

---

## 8. 为 accept 扩展 `IoOperation`

阶段四的 operation 已经能够区分 receive 和 send。阶段五只增加 accept 所需的类别和成员：

```cpp
enum class IoOperationType
{
    Accept,
    Receive,
    Send,
};

struct IoOperation final : OVERLAPPED
{
    IoOperationType type{IoOperationType::Receive};
    std::shared_ptr<ConnectionContext> connection;
    SOCKET acceptSocket{INVALID_SOCKET};
    QByteArray storage;
};
```

这里只关注新增关系：

| 成员 | accept operation 中的作用 |
| --- | --- |
| `OVERLAPPED` 基类 | 唯一标识这一次 accept，并由 completion packet 带回其地址。 |
| `type` | 让 worker 把 completion 分发到 accept handler。 |
| `connection` | accept 提交时为空，因为连接上下文尚未创建。 |
| `acceptSocket` | 保存为本次 `AcceptEx()` 预创建的 socket。 |
| `storage` | 保存本地地址区和远端地址区，必须活到 operation 最终完成。 |

### 8.1 operation 内存所有权与 socket 资源责任不同

```text
unique_ptr<IoOperation>
  └─ 拥有 IoOperation 的堆内存

IoOperation::acceptSocket
  └─ 记录一个 SOCKET handle
```

项目中的 `IoOperation` 析构函数不会自动调用 `closesocket()`。因此必须保证：

- 提交同步失败：提交路径显式关闭 `acceptSocket`。
- completion 失败：completion handler 显式关闭 `acceptSocket`。
- completion 成功：把 socket 责任转交给连接上下文，并把 operation 中的值改为 `INVALID_SOCKET`。

### 8.2 地址 buffer 尺寸

项目使用：

```cpp
constexpr DWORD AcceptAddressPadding{16};

int const addressPartSize{
    static_cast<int>(sizeof(sockaddr_storage) + AcceptAddressPadding)};
int const addressBufferSize{2 * addressPartSize};
```

因为项目把首批接收数据长度设为 `0`，buffer 只有两个地址区域：

```text
storage
  ├─ 本地地址区域：sizeof(sockaddr_storage) + 16
  └─ 远端地址区域：sizeof(sockaddr_storage) + 16
```

计算式：

```text
总大小
  = 首批数据区大小
  + 本地地址区大小
  + 远端地址区大小

  = 0
  + (sizeof(sockaddr_storage) + 16)
  + (sizeof(sockaddr_storage) + 16)
```

额外的 16 bytes 是 `AcceptEx()` 地址内部格式的要求，不是字符串结束符，也不是为了手动内存对齐。

`storage` 在 operation 在途期间不能 resize、clear 或被替换，否则传给 Windows 的 `data()` 地址可能失效。

---

## 9. 为每个 accept 预创建 socket

`AcceptEx()` 不会像同步 `accept()` 那样替调用者创建并返回一个新 socket。每个 operation 都要先创建自己的 accepted socket：

```cpp
operation->acceptSocket =
    WSASocketW(AF_INET,
               SOCK_STREAM,
               IPPROTO_TCP,
               nullptr,
               0,
               WSA_FLAG_OVERLAPPED);
```

`WSASocketW()` 已在阶段三介绍。这里六个实参的当前含义是：

| 实参 | 当前作用 |
| --- | --- |
| `AF_INET` | 创建 IPv4 socket，与 listening socket 的地址族一致。 |
| `SOCK_STREAM` | 创建面向连接的字节流 socket。 |
| `IPPROTO_TCP` | 使用 TCP。 |
| `nullptr` | 不指定自定义 `WSAPROTOCOL_INFO`，由前三个参数选择 provider。 |
| `0` | 不加入 socket group。 |
| `WSA_FLAG_OVERLAPPED` | 允许后续使用 Overlapped I/O 和 IOCP。 |

返回值：

- 非 `INVALID_SOCKET`：socket 创建成功。
- `INVALID_SOCKET`：创建失败，立即调用 `WSAGetLastError()`。

传给 `AcceptEx()` 前，这个 socket 必须处于：

```text
已创建
  + 未 bind
  + 未 connect
  + 尚未承载其他连接
```

一个 accepted socket 只能属于一个在途 accept operation。项目为每个槽位创建独立 socket，不让多个 `AcceptEx()` 竞争同一个 handle。

---

## 10. 提交一次 `AcceptEx()`

### 10.1 函数原型

```cpp
BOOL AcceptEx(
    SOCKET sListenSocket,
    SOCKET sAcceptSocket,
    PVOID lpOutputBuffer,
    DWORD dwReceiveDataLength,
    DWORD dwLocalAddressLength,
    DWORD dwRemoteAddressLength,
    LPDWORD lpdwBytesReceived,
    LPOVERLAPPED lpOverlapped);
```

八个参数的作用：

| 参数 | 作用 |
| --- | --- |
| `sListenSocket` | 已执行 `listen()` 的 listening socket；accept operation 在它上面提交。 |
| `sAcceptSocket` | 调用者预创建、尚未 bind 或 connect 的 socket；成功后代表新连接。 |
| `lpOutputBuffer` | 接收首批数据、本地地址和远端地址的连续 buffer；即使首批数据长度为 `0` 也必须提供。 |
| `dwReceiveDataLength` | buffer 开头为首批业务数据预留的 byte 数，不包含两个地址区域。为 `0` 时只等待连接，不等待数据。 |
| `dwLocalAddressLength` | 为本地地址保留的 byte 数，至少为该协议最大地址结构大小再加 16。 |
| `dwRemoteAddressLength` | 为远端地址保留的 byte 数，同样至少额外加 16，且不能为 `0`。 |
| `lpdwBytesReceived` | 同步完成时接收首批数据字节数；pending 后不会在未来写入这个变量。 |
| `lpOverlapped` | 标识本次 accept 的 `OVERLAPPED`；必须非空并保持有效直到最终 completion。 |

### 10.2 项目调用

```cpp
DWORD bytesReceived{0};
IoOperation* const operationPointer{operation.release()};

BOOL const result{this->m_acceptExFunction(
    this->m_listenSocket,
    operationPointer->acceptSocket,
    operationPointer->storage.data(),
    0,
    sizeof(sockaddr_storage) + AcceptAddressPadding,
    sizeof(sockaddr_storage) + AcceptAddressPadding,
    &bytesReceived,
    operationPointer)};
```

当前八个实参的具体含义：

| 实参 | 当前作用 |
| --- | --- |
| `this->m_listenSocket` | 项目唯一的 IPv4 listening socket，已经关联 completion port。 |
| `operationPointer->acceptSocket` | 只属于当前 operation 的预创建 socket。 |
| `operationPointer->storage.data()` | 当前 operation 独占的地址 buffer 首地址。 |
| `0` | 不在 accept 阶段接收业务数据，连接一到达即可完成。 |
| `sizeof(sockaddr_storage) + 16` | 本地地址区域容量。 |
| `sizeof(sockaddr_storage) + 16` | 远端地址区域容量。 |
| `&bytesReceived` | 只接收同步完成时的首批数据字节数；本项目预期为 `0`。 |
| `operationPointer` | `IoOperation` 的地址也是其 `OVERLAPPED` 基类地址，completion 时可转回原类型。 |

`bytesReceived` 可以是局部变量，因为 `AcceptEx()` 只会在同步完成时写入它；如果返回 pending，Windows 不会在函数返回后再写这个地址。与之相反，`storage` 和 `operationPointer` 会被在途 operation 持续使用，必须一直存活到最终 completion。

### 10.3 为什么数据长度使用 `0`

```text
dwReceiveDataLength = 0
  → 客户端完成连接建立
  → AcceptEx 可以完成
  → accepted socket 初始化
  → 单独 postReceive 接收业务数据
```

如果把它设为非零：

```text
客户端连接到达
  → 还没有发送足够触发读取的数据
  → AcceptEx 继续等待
```

本项目把“建立连接”和“接收业务数据”拆成两个 operation，使接入完成时机清晰，并直接复用阶段四的 `postReceive()`。

### 10.4 `bytesReceived == 0` 不是断开

阶段四中的规则是：非零长度 `WSARecv()` 成功完成且传输 `0` bytes，表示对端正常关闭。

本节的 accept 明确请求接收 `0` bytes，因此成功 accept completion 返回 `0` bytes 是预期结果，不能套用 receive completion 的关闭规则。operation 类型决定了相同字节数的语义。

---

## 11. 提交结果与所有权

`AcceptEx()` 有三种需要区分的提交结果：

1. **返回 `TRUE`**
   - 含义：operation 已立即完成。
   - completion：项目仍通过 IOCP 接收 packet。
   - 最终回收者：completion worker。
2. **返回 `FALSE`，错误码为 `ERROR_IO_PENDING`**
   - 含义：operation 已成功启动，但尚未完成。
   - completion：最终完成时通过 IOCP 接收 packet。
   - 最终回收者：completion worker。
3. **返回 `FALSE`，错误码为其他值**
   - 含义：同步提交失败。
   - completion：不会等待，也不会收到对应 packet。
   - 最终回收者：当前提交函数。

项目没有启用“同步成功时跳过 completion packet”的特殊模式，因此 `TRUE` 仍沿用阶段四的规则：提交路径不能释放 operation。

`AcceptEx()` 文档和项目代码检查的是 `ERROR_IO_PENDING`。它在这里仍表示阶段三、四已经掌握的 pending 分支：operation 提交成功，但最终结果尚未产生。

### 11.1 为什么先 `release()`，再调用 `AcceptEx()`

```text
unique_ptr 拥有 operation
  → release 得到唯一原生指针
  → 调用 AcceptEx
  → 立即 completion 可能被 worker 取出
```

如果等 `AcceptEx()` 返回后才转移所有权，另一个 worker 可能已经通过相同地址恢复了 `unique_ptr`，提交线程又继续持有原来的 `unique_ptr`，从而产生重复所有权。

### 11.2 同步失败时恢复所有权

```cpp
if (!result)
{
    int const errorCode{WSAGetLastError()};
    if (errorCode != ERROR_IO_PENDING)
    {
        operation.reset(operationPointer);
        this->m_pendingAcceptOperationCount.fetch_sub(1);
        closesocket(operation->acceptSocket);
        operation->acceptSocket = INVALID_SOCKET;
        return false;
    }
}
```

正确顺序：

1. `AcceptEx()` 返回 `FALSE` 后立即保存 `WSAGetLastError()`。
2. `ERROR_IO_PENDING` 表示提交成功，不恢复 `unique_ptr`。
3. 其他错误表示不会有对应 completion，使用 `reset()` 恢复唯一所有权。
4. 关闭预创建 socket。
5. 函数结束时由 `unique_ptr` 释放 operation 内存。

### 11.3 accept 计数为什么也要在调用前增加

项目的简化顺序是：

```text
pendingAcceptCount += 1
  → release operation
  → AcceptEx
```

立即 completion 可能很快被 worker 处理。worker 会先执行 `pendingAcceptCount -= 1`，因此提交前必须已经把本次 operation 计入；若同步提交失败，再在当前路径把计数减回去。

这与 operation 所有权转移是同一种竞态意识：凡是 completion handler 可能立即观察的状态，都要在提交前准备完成。

---

## 12. 预投递多个 accept 并维持接入深度

只保持一个 accept operation 在途时：

```text
accept A 完成
  → 初始化连接 A
  → 再提交 accept B
```

在 A 完成到 B 成功提交之间，应用层没有已经准备好的 accept operation。预投递多个 accept 可以让内核同时持有多个等待槽位：

```text
listening socket
  ├─ AcceptEx operation 1
  ├─ AcceptEx operation 2
  ├─ AcceptEx operation 3
  └─ ... operation 8
```

项目默认：

```cpp
static constexpr int DefaultInitialAcceptCount{8};
```

虽然配置名是 `initialAcceptCount`，项目并非只在启动时使用它；运行期间每次 accept completion 后，仍把它作为目标 accept depth。

### 12.1 补位函数

先只保留服务器正常运行时的补位主线：

```cpp
void RemoteControlTransport::Impl::replenishAccepts()
{
    std::lock_guard<std::mutex> const lock{this->m_acceptMutex};

    while (this->m_pendingAcceptOperationCount.load() <
           this->m_options.initialAcceptCount)
    {
        if (!this->postAccept())
        {
            return;
        }
    }
}
```

`replenishAccepts()` 没有参数。它读取成员中的当前在途数量和配置目标值，循环调用 `postAccept()`。

`postAccept()` 也没有参数。它为一个槽位自行创建 operation、地址 buffer 和 accepted socket；返回 `true` 表示本次 accept 已经立即完成或成功进入 pending，返回 `false` 表示没有新增在途槽位。

### 12.2 互斥量保护的是“检查并补充”整体

假设两个 worker 几乎同时处理 accept completion：

```text
原来 pending = 8
worker A 完成一个：pending = 7
worker B 完成一个：pending = 6
```

如果两个线程都在没有同步的情况下读取并补充，可能重复判断和超额提交。项目用 `m_acceptMutex` 串行执行补位循环，使最终数量回到目标值而不是超过目标。

### 12.3 目标值为 8 不表示什么

`initialAcceptCount == 8` 只表示希望保持 8 个 accept operation 在途。它不表示：

- 只有 8 个 completion worker。
- 最多允许 8 个客户端连接。
- `listen()` backlog 只能容纳 8 个请求。
- 同时只能有 8 个 receive operation。

这些是不同层次的容量概念。

### 12.4 提交失败后的数量

```text
目标 pending = 8
当前 pending = 7
postAccept 同步失败
  → operation 和 accepted socket 在提交路径回收
  → pending 仍为 7
  → replenishAccepts 返回
```

同步失败的 operation 不能留在 pending 数量中。以后再次调用 `replenishAccepts()` 时，仍会看到这个空缺并重新尝试；当前补位函数不会把失败槽位伪装成成功在途。

---

## 13. completion worker 只新增 `Accept` 分支

阶段四已经完整解释 `GetQueuedCompletionStatus()` 结果矩阵和 `unique_ptr` 恢复。阶段五不重写 worker，只增加一种 operation 分发：

```cpp
auto operation{
    std::unique_ptr<IoOperation>{static_cast<IoOperation*>(overlapped)}};

switch (operation->type)
{
    case IoOperationType::Accept:
        this->handleAcceptCompletion(std::move(operation), success == TRUE);
        break;
    case IoOperationType::Receive:
        this->handleReceiveCompletion(
            std::move(operation), success == TRUE, transferredBytes);
        break;
    case IoOperationType::Send:
        this->handleSendCompletion(
            std::move(operation), success == TRUE, transferredBytes);
        break;
}
```

`handleAcceptCompletion()` 的两个参数：

| 参数 | 作用 |
| --- | --- |
| `_operation` | 已由 worker 恢复唯一所有权的 accept operation；其中保存 accepted socket 和地址 buffer。 |
| `_success` | `GetQueuedCompletionStatus()` 是否报告这次 accept 成功完成。 |

本项目的 accept 数据长度为 `0`，所以 handler 不需要 `transferredBytes`。receive 和 send 仍按阶段四的规则使用该值。

失败 completion 也已经消耗了一个 accept slot，并且 operation 中仍有一个预创建 socket，因此不能直接丢弃：

```text
失败 accept completion
  → 恢复 operation
  → 减少在途 accept 计数
  → 补充新 accept
  → 关闭失败 operation 的 accepted socket
  → 释放 operation
```

---

## 14. completion 到达后先取出 socket 并补位

handler 开头先做四件事：

```cpp
SOCKET const acceptedSocket{_operation->acceptSocket};
_operation->acceptSocket = INVALID_SOCKET;

this->m_pendingAcceptOperationCount.fetch_sub(1);
this->replenishAccepts();
```

### 14.1 为什么先复制 socket，再写入 `INVALID_SOCKET`

```text
accept operation
  └─ 原来负责保存 acceptedSocket

handler 局部变量
  └─ 接管 acceptedSocket 的资源处理责任
```

operation 会在 handler 返回时销毁。把成员改为 `INVALID_SOCKET` 可以明确表示：后续所有成功或失败路径都只处理局部变量 `acceptedSocket`，不会再次把同一个 handle 当成 operation 中未处理的资源。

### 14.2 为什么先减少计数

当前 completion 表示原来的 accept operation 已不再在途。只有先执行减一，`replenishAccepts()` 才能看见空出的槽位。

### 14.3 为什么先补位，再初始化新连接

accepted socket 的后续初始化可能包含：

- 更新 accept context。
- 关联 completion port。
- 创建 connection context。
- 提交第一次 receive。

先补位可以缩短 accept depth 低于目标值的时间：

```text
一个 accept 完成
  → pending 8 → 7
  → 立即 post 新 accept
  → pending 7 → 8
  → 再初始化刚刚建立的连接
```

即使当前 completion 失败，也要补位，因为失败的 operation 同样已经退出在途集合。

---

## 15. 使用 `SO_UPDATE_ACCEPT_CONTEXT`

`AcceptEx()` 成功后，accepted socket 已代表一条连接，但它还没有继承 listening socket 的完整属性。项目通过 `setsockopt()` 更新 accept context。

accepted socket 会经历三个清晰状态：

1. **提交 `AcceptEx()` 前**
   - socket 状态：已创建，但未 bind、未 connect。
   - 当前处理：只作为某一个 accept operation 的目标 socket。
2. **accept 成功、context 尚未更新**
   - socket 状态：已连接，但 listening socket 的属性尚未继承完整。
   - 当前处理：优先执行 `setsockopt(SO_UPDATE_ACCEPT_CONTEXT)`，不要直接进入普通 socket 管理流程。
3. **context 更新成功后**
   - socket 状态：具有正常的 accepted socket 上下文。
   - 当前处理：可以关联 IOCP、设置其他选项并进入 receive/send 主链。

### 15.1 `setsockopt()` 原型

```cpp
int WSAAPI setsockopt(
    SOCKET s,
    int level,
    int optname,
    char const* optval,
    int optlen);
```

五个参数的通用作用：

| 参数 | 作用 |
| --- | --- |
| `s` | 要修改选项的目标 socket。 |
| `level` | 选项所属协议层。 |
| `optname` | 要设置的具体选项。 |
| `optval` | 指向选项值内存的指针。 |
| `optlen` | 选项值的字节数。 |

### 15.2 当前调用

```cpp
SOCKET const listenSocket{this->m_listenSocket};

int const result{setsockopt(
    acceptedSocket,
    SOL_SOCKET,
    SO_UPDATE_ACCEPT_CONTEXT,
    reinterpret_cast<char const*>(&listenSocket),
    sizeof(listenSocket))};
```

当前五个实参的具体含义：

1. **`acceptedSocket`**
   - 指定刚刚由 `AcceptEx()` 建立连接的 socket。
2. **`SOL_SOCKET`**
   - 表示这是 socket 层选项。
3. **`SO_UPDATE_ACCEPT_CONTEXT`**
   - 要求 accepted socket 继承 listening socket 的 accept 上下文。
4. **`reinterpret_cast<char const*>(&listenSocket)`**
   - 选项值是 listening socket handle 自身的字节表示，不是 handle 指向的对象。
5. **`sizeof(listenSocket)`**
   - 指定一个 `SOCKET` 值的大小，不是 `SOCKET*` 的业务含义。

返回值：

- `0`：设置成功。
- `SOCKET_ERROR`：设置失败，立即使用 `WSAGetLastError()` 保存错误码。

### 15.3 listening socket 必须仍然有效

`optval` 指向的值是 listening socket。调用发生时，该 handle 必须仍有效。项目使用 accept 相关互斥量协调 context 更新与关闭 listening socket；并发安全停机的完整推导留到阶段九。

### 15.4 这个选项不会完成哪些事情

`SO_UPDATE_ACCEPT_CONTEXT` 不会：

- 把 accepted socket 关联到 completion port。
- 创建 `ConnectionContext`。
- 自动提交 `WSARecv()`。
- 把地址 buffer 变成业务数据。

它只完成 accepted socket 的 accept context 更新，后续步骤仍由应用执行。

---

## 16. accepted socket 仍要关联 completion port

listening socket 和 accepted socket 是两个不同 handle：

```text
listening socket
  └─ 关联 port，用于接收 AcceptEx completion

accepted socket
  └─ accept 成功后另行关联同一个 port
       └─ 用于接收后续 WSARecv / WSASend completion
```

项目复用阶段四已经掌握的关联调用：

```cpp
HANDLE const associatedPort{CreateIoCompletionPort(
    reinterpret_cast<HANDLE>(acceptedSocket),
    this->m_completionPort,
    0,
    0)};
```

四个实参在本节的含义：

1. **`reinterpret_cast<HANDLE>(acceptedSocket)`**
   - 指定要关联的新 accepted socket。
2. **`this->m_completionPort`**
   - 指定 listening socket 和所有客户端 socket 共享的现有 port。
3. **第三个实参 `0`**
   - 作为当前项目给普通 socket 使用的 completion key。
4. **第四个实参 `0`**
   - 关联已有 port 时，该并发参数被忽略。

成功时返回传入的同一个 completion port；失败时返回 `nullptr`，并使用 `GetLastError()`，不是 `WSAGetLastError()`。

### 16.1 为什么不会自动继承关联

IOCP 关联属于具体 handle。listening socket 已关联，不代表由它接受的另一个 socket handle 自动关联。

`SO_UPDATE_ACCEPT_CONTEXT` 更新的是 Winsock accept context，也不等同于 IOCP handle 关联。

### 16.2 关联失败怎样收尾

```text
accepted socket 无法关联 port
  → 不能安全进入项目的 receive/send IOCP 主链
  → closesocket(acceptedSocket)
  → 不创建 connection
  → 不提交 receive
```

不能保留一个已经连接、却没有 completion 接收路径的 socket。

---

## 17. 创建连接上下文并提交第一次 receive

context 更新和 IOCP 关联都成功后，accepted socket 才进入阶段四已经建立的 connected-socket 流程：

```cpp
std::shared_ptr<ConnectionContext> const connection{
    this->m_connectionRegistry.add(acceptedSocket)};

if (!connection)
{
    closesocket(acceptedSocket);
    return;
}

static_cast<void>(this->postReceive(connection));
```

`m_connectionRegistry.add()` 的当前输入是 `acceptedSocket`，成功后返回代表该连接的共享上下文；容量限制、连接阶段和注册表并发属于后续学习内容。

`postReceive()` 的参数与返回值：

| 项目接口 | 作用 |
| --- | --- |
| `_connection` | 已保存 accepted socket 的连接上下文；receive operation 会持有它的强引用。 |
| 返回 `true` | 第一次 receive 已成功提交。 |
| 返回 `false` | receive 未进入在途状态，项目的关闭或停机路径负责收尾。 |

所有权链变为：

```text
accept operation
  → completion handler 局部 acceptedSocket
  → ConnectionContext 保存 socket
  → receive operation 保存 ConnectionContext 强引用
```

本阶段到 `postReceive()` 为止。收到哪些字节、怎样解析 packet、怎样响应 `TestConnection`，从阶段六开始。

---

## 18. 成功与失败 completion 的完整处理顺序

下面代码保留阶段五的关键步骤，省略日志、停止竞态、连接容量细节和其他 socket 选项：

```cpp
void handleAcceptCompletion(std::unique_ptr<IoOperation> _operation,
                            bool _success)
{
    SOCKET const acceptedSocket{_operation->acceptSocket};
    _operation->acceptSocket = INVALID_SOCKET;

    this->m_pendingAcceptOperationCount.fetch_sub(1);
    this->replenishAccepts();

    if (!_success)
    {
        closesocket(acceptedSocket);
        return;
    }

    SOCKET const listenSocket{this->m_listenSocket};
    int const contextResult{setsockopt(
        acceptedSocket,
        SOL_SOCKET,
        SO_UPDATE_ACCEPT_CONTEXT,
        reinterpret_cast<char const*>(&listenSocket),
        sizeof(listenSocket))};

    if (contextResult == SOCKET_ERROR)
    {
        closesocket(acceptedSocket);
        return;
    }

    HANDLE const associatedPort{CreateIoCompletionPort(
        reinterpret_cast<HANDLE>(acceptedSocket),
        this->m_completionPort,
        0,
        0)};

    if (!associatedPort)
    {
        closesocket(acceptedSocket);
        return;
    }

    std::shared_ptr<ConnectionContext> const connection{
        this->m_connectionRegistry.add(acceptedSocket)};
    if (!connection)
    {
        closesocket(acceptedSocket);
        return;
    }

    static_cast<void>(this->postReceive(connection));
}
```

顺序与理由：

| 顺序 | 操作 | 理由 |
| --- | --- | --- |
| 1 | 从 operation 取出 accepted socket | handler 接管该 handle 的处理责任 |
| 2 | pending accept 计数减一 | 当前 operation 已不再在途 |
| 3 | 立即补充 accept | 尽快恢复接入深度 |
| 4 | 检查 completion 成功与否 | 失败 socket 不能继续初始化 |
| 5 | `SO_UPDATE_ACCEPT_CONTEXT` | 完成 accepted socket 的 Winsock accept 上下文 |
| 6 | 关联 completion port | 为后续 receive/send completion 建立通知路径 |
| 7 | 创建 connection context | 把新 socket 纳入应用连接生命周期 |
| 8 | `postReceive()` | 开始阶段四的 receive 主链 |

任何一步失败都遵循同一原则：

> 尚未成功转交给连接上下文的 accepted socket，由当前 handler 关闭；已经补充的新 accept operation 不受影响。

---

## 19. 映射到项目启动主链

建议按下面顺序只读源码。先记住一次源项目根目录，后面只写相对路径：

> 源项目根目录：`D:\CodeRepository\claude\remote_control`

1. **应用入口**
   - 位置：`src\server\RemoteControlServer.cpp:27`
   - 观察：应用层只把 `_port` 交给 transport。
2. **public transport 入口**
   - 位置：`server_transport\src\RemoteControlTransport.cpp:809`
   - 观察：public transport 再委托给 `Impl::start()`。
3. **启动主体**
   - 位置：`server_transport\src\RemoteControlTransport.cpp:82`
   - 观察：port、listening socket、监听状态、worker 和 accept depth 的初始化顺序。
4. **加载 `AcceptEx()`**
   - 位置：`server_transport\src\RemoteControlTransport.cpp:260`
   - 观察：使用 `WSAIoctl()` 动态取得函数指针。
5. **补充 accept slot**
   - 位置：`server_transport\src\RemoteControlTransport.cpp:321`
   - 观察：把在途 accept 数量补到配置目标。
6. **提交 accept**
   - 位置：`server_transport\src\RemoteControlTransport.cpp:276`
   - 观察：创建 operation、buffer、accepted socket，并提交 `AcceptEx()`。
7. **分发 accept completion**
   - 位置：`server_transport\src\RemoteControlTransport.cpp:417`
   - 观察：worker 根据 `IoOperationType::Accept` 分发 completion。
8. **初始化 accepted socket**
   - 位置：`server_transport\src\RemoteControlTransport.cpp:502`
   - 观察：补位、更新 context、关联 port、注册 connection。
9. **进入 receive 主链**
   - 位置：`server_transport\src\RemoteControlTransport.cpp:335`
   - 观察：新连接进入阶段四的 `postReceive()` 主链。

对应调用链：

```text
RemoteControlServer::start
  → RemoteControlTransport::start
  → RemoteControlTransport::Impl::start
      → loadAcceptEx
      → replenishAccepts
          → postAccept
              → AcceptEx
  → runCompletionWorker
      → handleAcceptCompletion
          → replenishAccepts
          → setsockopt(SO_UPDATE_ACCEPT_CONTEXT)
          → CreateIoCompletionPort(acceptedSocket, existingPort, ...)
          → ConnectionRegistry::add
          → postReceive
```

### 19.1 阅读 `postAccept()` 时只追踪三条线

```text
内存线：unique_ptr → release → completion worker → unique_ptr
socket 线：create → AcceptEx → close 或 transfer to connection
计数线：提交前 +1 → completion 或同步失败时 -1
```

### 19.2 阅读 `handleAcceptCompletion()` 时只追踪一个顺序

```text
取出 socket
  → accept count -1
  → replenish
  → 检查成功
  → update context
  → associate port
  → add connection
  → postReceive
```

项目中的全局 pending I/O 计数、停止标志、空闲超时线程、容量限制和日志不会改变这条 accept 主线，可在相应后续阶段再展开。

---

## 20. 常见错误

| 错误 | 直接后果 | 根因 |
| --- | --- | --- |
| 直接按普通函数名调用 `AcceptEx()`，没有取得函数指针 | 无法按 provider 扩展机制正确调用 | 忘记使用 `WSAIoctl()` 和 `WSAID_ACCEPTEX` |
| 用任意无关 socket 查询函数指针 | provider 匹配关系不清晰 | 没有使用实际 listening socket 查询 |
| `WSAIoctl()` 输出 buffer 传成函数指针值而不是地址 | 查询结果写入非法位置 | 混淆 `m_acceptExFunction` 与 `&m_acceptExFunction` |
| 复用一个 accepted socket 提交多个 accept | operation 相互冲突 | 一个在途 accept 必须独占一个未连接 socket |
| accepted socket 已经 bind 或 connect | `AcceptEx()` 提交失败 | 不满足 accepted socket 的初始状态要求 |
| 两个 operation 共用一个地址 buffer | 地址数据相互覆盖或产生数据竞争 | 忘记 buffer 属于一次 operation |
| 本地或远端地址区没有额外加 16 | `AcceptEx()` 参数不满足要求 | 把地址区长度写成单纯的 `sizeof(sockaddr)` |
| `dwReceiveDataLength == 0` 时不提供 buffer | 调用参数无效 | 忽略地址区域仍然必须存在 |
| 把总 buffer 大小传给每一个地址长度参数 | Windows 看到的布局与实际内存不一致 | 混淆总容量和单个地址区域容量 |
| `AcceptEx()` 返回 `TRUE` 后立即释放 operation | worker 随后访问悬空 `OVERLAPPED*` | 忘记立即完成仍通过 IOCP 交付 packet |
| `FALSE + ERROR_IO_PENDING` 被当作失败 | 关闭仍在使用的 socket 和 operation | 把 pending 误认为最终失败 |
| 同步失败时仍等待 completion | operation、socket 和计数永久滞留 | 其他同步错误不会产生对应 packet |
| 在调用 `AcceptEx()` 后才增加 accept 计数 | 立即 completion 可能先执行减一 | completion 可与提交线程并发 |
| 成功 accept 的 `transferredBytes == 0` 被当作断开 | 刚建立的连接被立即关闭 | 错用了非零长度 receive 的零字节规则 |
| accept 失败 completion 不补位 | 在途 accept 数量逐渐下降 | 忘记失败 operation 也已经消耗槽位 |
| 忘记 `SO_UPDATE_ACCEPT_CONTEXT` | accepted socket 上下文不完整，后续 socket 操作可能失败 | 把连接建立完成误认为全部初始化完成 |
| 把 `SO_UPDATE_ACCEPT_CONTEXT` 的值传成 accepted socket | 更新了错误来源的上下文 | 该选项值必须是 listening socket |
| `optlen` 使用地址 buffer 大小 | `setsockopt()` 参数错误 | 这里传的是一个 `SOCKET` 值 |
| 认为 context 更新会自动关联 IOCP | 后续 receive/send completion 无法进入目标 port | 混淆 Winsock 上下文与 handle 关联 |
| accepted socket 关联失败后仍调用 `postReceive()` | 提交后没有正确 completion 路径 | 未在初始化失败时关闭 socket |
| 把 accept depth 当成最大连接数 | 容量推理错误 | 混淆在途接入请求和已建立连接 |

错误函数来源：

| 失败调用 | 立即读取的错误函数 |
| --- | --- |
| `WSASocketW()` | `WSAGetLastError()` |
| `WSAIoctl()` | `WSAGetLastError()` |
| `AcceptEx()` | `WSAGetLastError()` |
| `setsockopt()` | `WSAGetLastError()` |
| `CreateIoCompletionPort()` | `GetLastError()` |

---

## 21. 阶段练习与验收

按顺序完成每个任务。先独立回答，再使用验收标准和参考答案检查。参数题应说明输入输出方向、当前实参、实际作用和所依赖对象的生命周期。

### 21.1 任务一：画出一次 accept 的对象关系

**练习**

使用以下对象画图，并标出“共享”“独占”“关联”“拥有内存”“保存 handle”“带回指针”：

```text
listening socket
accepted socket
completion port
accept operation / OVERLAPPED
address buffer
completion worker
```

回答：

1. 哪个 socket 在提交前已经关联 port？
2. 哪些对象可以被多个 accept operation 共享？
3. 哪些对象必须每个 operation 独占？
4. accept completion 通过哪个 `OVERLAPPED*` 找回 operation？
5. accepted socket 在什么时候关联 port？

**验收标准**

- [ ] listening socket 被多个 accept operation 共享。
- [ ] listening socket 在 `AcceptEx()` 前已关联 port。
- [ ] 每个 operation 独占 `OVERLAPPED`、accepted socket 和地址 buffer。
- [ ] packet 带回当前 operation 的 `OVERLAPPED*`。
- [ ] accepted socket 在 accept 成功完成后单独关联同一个 port。
- [ ] 不把 socket 与 port 的关联描述为所有权关系。

**参考答案与解释**

```text
listening socket
  ├─ 关联到 ──> completion port
  └─ 被共享 ──> accept operation A / B / C

accept operation A
  ├─ 包含独立 OVERLAPPED A
  ├─ 保存独立 accepted socket A
  └─ 拥有独立 address buffer A

AcceptEx A 完成
  → packet 进入 listening socket 关联的 port
  → packet 带回 OVERLAPPED A*
  → worker 恢复 operation A
  → 初始化 accepted socket A
  → accepted socket A 关联同一个 port
```

### 21.2 任务二：解释 `WSAIoctl()` 的九个实参

**练习**

不看正文，逐个解释：

```cpp
WSAIoctl(
    listenSocket,
    SIO_GET_EXTENSION_FUNCTION_POINTER,
    &acceptExGuid,
    sizeof(acceptExGuid),
    &acceptExFunction,
    sizeof(acceptExFunction),
    &bytesReturned,
    nullptr,
    nullptr);
```

然后回答：

1. `acceptExGuid` 应保存哪个值？
2. 第五个参数为什么有 `&`？
3. 最后两个参数为什么都是 `nullptr`？
4. 成功和失败分别返回什么？
5. 失败后调用哪个错误函数？

**验收标准**

- [ ] 九个实参都能对应到原型参数。
- [ ] 知道输入 buffer 保存 `WSAID_ACCEPTEX`。
- [ ] 知道输出 buffer 是函数指针变量本身的地址。
- [ ] 知道两个 `sizeof` 分别描述输入和输出 buffer。
- [ ] 知道 `bytesReturned` 是实际输出字节数。
- [ ] 知道本次是同步查询，不会产生 accept completion。
- [ ] 失败后使用 `WSAGetLastError()`。

**参考答案与解释**

```text
listenSocket                         → 选择实际 provider
SIO_GET_EXTENSION_FUNCTION_POINTER  → 查询扩展函数入口
&acceptExGuid                        → 输入 WSAID_ACCEPTEX
sizeof(acceptExGuid)                 → 输入 GUID 的大小
&acceptExFunction                    → 输出写入 LPFN_ACCEPTEX 变量
sizeof(acceptExFunction)             → 输出变量容量
&bytesReturned                       → 输出实际写入字节数
nullptr                              → 不使用 OVERLAPPED
nullptr                              → 不使用 completion routine
```

成功返回 `0`；失败返回 `SOCKET_ERROR`。函数指针变量必须保存到后续所有 `postAccept()` 都不再使用它为止。

### 21.3 任务三：计算 buffer 并解释 `AcceptEx()` 参数

**练习**

假设：

```text
sizeof(sockaddr_storage) = 128
dwReceiveDataLength = 0
```

回答：

1. 本地地址区域至少多少 bytes？
2. 远端地址区域至少多少 bytes？
3. 总地址 buffer 至少多少 bytes？
4. 为什么不能只分配 `128 * 2`？
5. 为什么数据长度为 `0` 时仍要提供 `lpOutputBuffer`？

然后逐个解释：

```cpp
acceptExFunction(
    listenSocket,
    acceptSocket,
    addressBuffer.data(),
    0,
    sizeof(sockaddr_storage) + 16,
    sizeof(sockaddr_storage) + 16,
    &bytesReceived,
    operationPointer);
```

**验收标准**

- [ ] 单个地址区域计算为 `128 + 16 = 144` bytes。
- [ ] 总 buffer 计算为 `0 + 144 + 144 = 288` bytes。
- [ ] 知道额外 16 bytes 是每个地址区域分别需要。
- [ ] 知道 output buffer 仍承载两个地址区域。
- [ ] 能解释八个实参。
- [ ] 知道 accepted socket 必须未 bind、未 connect。
- [ ] 知道 `operationPointer` 必须活到最终 completion。

**参考答案与解释**

```text
本地地址区 = 128 + 16 = 144
远端地址区 = 128 + 16 = 144
总 buffer   = 0 + 144 + 144 = 288
```

第四个实参为 `0` 只取消首批业务数据区，不会取消本地和远端地址区。最后一个参数标识这一次 accept；它不能指向栈上即将离开作用域的 `OVERLAPPED`。

### 21.4 任务四：完成提交结果与所有权表

**练习**

分别补全下面三种结果中的提交状态、completion、`unique_ptr` 和 accepted socket 责任：

1. `AcceptEx()` 返回 `TRUE`，不读取错误码。
2. `AcceptEx()` 返回 `FALSE`，错误码为 `ERROR_IO_PENDING`。
3. `AcceptEx()` 返回 `FALSE`，错误码为 `WSAEINVAL`。

再回答：

1. 为什么 `release()` 必须发生在调用前？
2. 为什么 pending accept 计数也要在调用前加一？
3. 同步失败时要回滚哪些资源和状态？
4. 成功 accept completion 的字节数为 `0` 时为什么不能关闭连接？

**验收标准**

- [ ] `TRUE` 和 pending 都由 completion worker 回收 operation。
- [ ] 其他同步错误不会产生对应 packet。
- [ ] 同步失败恢复 `unique_ptr`、计数减一并关闭 accepted socket。
- [ ] 能解释立即 completion 与提交线程并发的可能性。
- [ ] 知道 accept 的零字节来自请求长度为零，不等同于 receive 零字节。

**参考答案与解释**

1. **返回 `TRUE`**
   - 提交状态：成功，且已立即完成。
   - completion：由 IOCP 交付。
   - `unique_ptr`：提交路径不恢复。
   - accepted socket：由 completion handler 继续处理。
2. **返回 `FALSE`，错误码为 `ERROR_IO_PENDING`**
   - 提交状态：成功，operation 仍在进行。
   - completion：最终由 IOCP 交付。
   - `unique_ptr`：提交路径不恢复。
   - accepted socket：由 completion handler 继续处理。
3. **返回 `FALSE`，错误码为 `WSAEINVAL`**
   - 提交状态：失败。
   - completion：不会产生对应 packet。
   - `unique_ptr`：提交路径立即恢复。
   - accepted socket：由提交路径关闭。

### 21.5 任务五：排列 accept completion 顺序

**练习**

把下面步骤按正确顺序排列，并为每一步写一句理由：

```text
postReceive
CreateIoCompletionPort(acceptedSocket, existingPort, ...)
pendingAcceptCount -= 1
SO_UPDATE_ACCEPT_CONTEXT
replenishAccepts
从 operation 取出 acceptedSocket
检查 accept completion 是否成功
创建 ConnectionContext
```

回答：

1. accept 失败时哪些步骤仍要执行？
2. context 更新失败后由谁关闭 socket？
3. 为什么 listening socket 的 IOCP 关联不能替代 accepted socket 的关联？
4. `setsockopt()` 的 `optval` 应指向哪个 socket 值？
5. `CreateIoCompletionPort()` 失败后使用哪个错误函数？

**验收标准**

- [ ] 先接管 socket，再减少计数和补位。
- [ ] 补位发生在耗时更长的新连接初始化之前。
- [ ] 失败 completion 仍会补位。
- [ ] 成功后先更新 context，再进入 receive 主链。
- [ ] accepted socket 单独关联同一个 port。
- [ ] context 或关联失败都关闭当前 accepted socket。
- [ ] `optval` 指向 listening socket 值。
- [ ] IOCP 关联失败使用 `GetLastError()`。

**参考答案与解释**

```text
从 operation 取出 acceptedSocket
  → pendingAcceptCount -= 1
  → replenishAccepts
  → 检查 completion 成功
  → setsockopt(SO_UPDATE_ACCEPT_CONTEXT)
  → CreateIoCompletionPort(acceptedSocket, existingPort, ...)
  → 创建 ConnectionContext
  → postReceive
```

失败 completion 执行前三步后关闭 socket；它不会更新 context、关联 port、创建 connection 或提交 receive。

### 21.6 任务六：推演 8 个 accept slot

**练习**

初始状态：

```text
targetAcceptCount = 8
pendingAcceptCount = 8
```

依次推演：

1. worker A 取得一个成功 accept completion。
2. A 已执行减一，但尚未进入 `replenishAccepts()`。
3. worker B 又取得一个失败 accept completion并执行减一。
4. A 获得 `m_acceptMutex` 并补位。
5. B 随后进入 `replenishAccepts()`。
6. 如果 A 的第二次 `postAccept()` 同步失败，最终计数是多少？

再说明 target `8` 与下面四个概念为什么不同：

```text
worker count
listen backlog
maximum connection count
pending receive count
```

**验收标准**

- [ ] 两个 completion 后计数从 8 变为 6。
- [ ] A 在互斥区中最多补到 8。
- [ ] B 随后看到 8，不再超额提交。
- [ ] 同步失败的 post 不留在 pending 计数中。
- [ ] 能区分接入槽位、线程、内核 backlog、连接容量和 receive operation。

**参考答案与解释**

正常补位：

```text
8
  → A completion：7
  → B completion：6
  → A post 成功：7
  → A 再 post 成功：8
  → B 进入：已经是 8，不提交
```

如果 A 第一次补位成功、第二次同步失败，则最终保持 `7`。失败提交在当前路径回滚计数，后续重试再尝试恢复到 `8`。

### 21.7 任务七：还原项目启动主链

**练习**

不看正文，把以下动作排序：

```text
listen
创建 completion port
创建 completion workers
loadAcceptEx
bind
replenishAccepts
listening socket 关联 port
创建 WSA_FLAG_OVERLAPPED listening socket
```

然后在项目中只读并定位：

- `RemoteControlServer::start()`。
- `RemoteControlTransport::start()`。
- `RemoteControlTransport::Impl::start()`。
- `loadAcceptEx()`。
- `replenishAccepts()`。
- `postAccept()`。
- `runCompletionWorker()` 的 `Accept` 分支。
- `handleAcceptCompletion()`。
- `postReceive()`。

为每个函数写一句“输入状态 → 输出状态”。

**验收标准**

- [ ] 先创建 port 和 listening socket，再 `bind()`、`listen()`。
- [ ] `loadAcceptEx()` 在 listening socket 可用后执行。
- [ ] 提交 accept 前 listening socket 已关联 port。
- [ ] workers 在项目中先于初始 accept 投递创建。
- [ ] 启动成功前检查在途 accept 数量达到配置值。
- [ ] 能从 accept completion 连续追到 `postReceive()`。
- [ ] 不把协议解析、发送队列或安全停机加入当前启动主链。

**参考答案与解释**

```text
创建 completion port
  → 创建 WSA_FLAG_OVERLAPPED listening socket
  → bind
  → listen
  → loadAcceptEx
  → listening socket 关联 port
  → 创建 completion workers
  → replenishAccepts
```

启动结束时的关键状态是：port 有效、listening socket 正在监听且已关联、函数指针有效、worker 已等待、accept depth 达到目标。

### 21.8 最终综合验收

**练习**

闭卷复述下面完整链路，并在每个箭头处说明对象所有权或资源责任发生了什么变化：

```text
start
  → loadAcceptEx
  → 创建 accept operation
  → 创建 accepted socket
  → 准备 address buffer
  → pending count +1
  → release unique_ptr
  → AcceptEx
  → completion packet
  → worker 恢复 unique_ptr
  → pending count -1
  → replenish accepts
  → SO_UPDATE_ACCEPT_CONTEXT
  → accepted socket 关联 port
  → 创建 connection
  → postReceive
```

必须额外回答：

1. `WSAIoctl()` 的九个实参分别是什么？
2. `AcceptEx()` 的八个实参分别是什么？
3. 地址 buffer 为什么是两个“地址结构大小加 16”？
4. 三种提交结果分别由谁回收 operation？
5. accept 成功时零字节为什么正常？
6. accept 失败 completion 为什么仍需补位？
7. `SO_UPDATE_ACCEPT_CONTEXT` 的五个实参分别是什么？
8. accepted socket 为什么必须另行关联 IOCP？
9. 默认 8 个 accept slot 表示什么、不表示什么？
10. 从项目哪个函数进入阶段六？

**验收标准**

- [ ] 能完整说明 listening socket、accepted socket、operation、buffer 和 port 的关系。
- [ ] 能逐个解释 `WSAIoctl()`、`AcceptEx()` 和本节 `setsockopt()` 调用的全部参数。
- [ ] 能正确处理立即完成、pending、同步失败和失败 completion。
- [ ] operation 内存与 socket handle 都只有一条明确回收路径。
- [ ] counter 更新顺序能抵抗立即 completion。
- [ ] context 更新和 IOCP 关联没有混为一件事。
- [ ] 成功路径最终只进入阶段四的 `postReceive()`。
- [ ] 不依赖 packet 协议、状态机、发送队列、取消或安全停机知识完成复述。

**参考答案与解释**

完整答案至少包含以下八层：

1. `WSAIoctl()` 使用 listening socket 和 `WSAID_ACCEPTEX` 取得 provider 对应的 `LPFN_ACCEPTEX`。
2. 每个 accept slot 创建独立 operation、未连接 accepted socket 和地址 buffer；listening socket由它们共享。
3. 每个地址区域至少为最大地址结构大小加 16；数据长度为 `0` 时总 buffer 只包含两个地址区域。
4. 提交前先增加计数并释放 `unique_ptr`；`TRUE` 和 pending 都交给 worker，其他同步错误由提交路径回滚。
5. listening socket 上的 accept completion 进入 port；worker 按阶段四规则恢复 operation，即使 completion 失败也不能遗漏回收。
6. handler 先接管 socket、计数减一并补位；accept 的零字节是请求长度为零的结果，不表示断开。
7. 成功 socket先设置 `SO_UPDATE_ACCEPT_CONTEXT`，再单独关联同一个 completion port；任一步失败都关闭它。
8. connection context 创建后接管 socket，并由 `postReceive()` 开始下一阶段的数据处理链。

全部任务通过后，阶段五才算完成。

---

## 22. 下一阶段衔接

阶段五结束时，服务器已经能够持续异步接入连接：

```text
AcceptEx completion
  → accepted socket 初始化完成
  → accepted socket 关联 IOCP
  → ConnectionContext 创建
  → postReceive
```

此时尚未回答：第一次 receive 完成后，收到的字节怎样成为一个可执行的业务请求。

阶段六沿着最短业务链继续：

```text
postReceive
  → WSARecv completion
  → handleReceiveCompletion
  → processReceivedPackets
  → Packet::tryParse
  → handleInitialPacket(TestConnection)
  → 发送响应
  → 正常关闭连接
```

阶段五结论在阶段六中的用途：

| 阶段五已经保证 | 阶段六可以直接开始 |
| --- | --- |
| accepted socket 的 context 已更新 | 可以把它视为正常 connected socket |
| accepted socket 已关联 completion port | 第一次 receive completion 会回到同一组 worker |
| `ConnectionContext` 已创建 | 可以保存接收字节、连接阶段和业务状态 |
| 第一次 `postReceive()` 已提交 | 可以从 `handleReceiveCompletion()` 开始追踪业务数据 |
| accept slot 已先补位 | 处理当前连接时，服务器仍能继续接收其他连接 |

进入阶段六前，应能够准确回答：

> 一个 accept completion 到达后，为什么要先补充 accept slot，再依次更新 socket context、关联 completion port，并提交第一次 receive？

---

## 23. 官方资料

阅读时重点核对函数原型、每个参数、返回值、buffer 尺寸、错误函数和对象生命周期。

- [AcceptEx function](https://learn.microsoft.com/en-us/windows/win32/api/mswsock/nf-mswsock-acceptex)
- [WSAIoctl function](https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-wsaioctl)
- [Provider-Specific Extension Mechanism](https://learn.microsoft.com/en-us/windows/win32/winsock/provider-specific-extension-mechanism-2)
- [setsockopt function](https://learn.microsoft.com/en-us/windows/win32/api/winsock/nf-winsock-setsockopt)
- [SOL_SOCKET Socket Options](https://learn.microsoft.com/en-us/windows/win32/winsock/sol-socket-socket-options)
- [CreateIoCompletionPort function](https://learn.microsoft.com/en-us/windows/win32/api/ioapiset/nf-ioapiset-createiocompletionport)
- [GetQueuedCompletionStatus function](https://learn.microsoft.com/en-us/windows/win32/api/ioapiset/nf-ioapiset-getqueuedcompletionstatus)
