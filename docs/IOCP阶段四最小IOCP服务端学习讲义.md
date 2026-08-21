# IOCP 阶段四：最小 IOCP 服务端学习讲义

> 前置知识：阶段三的 Overlapped I/O、operation 生命周期、`OVERLAPPED`、`WSABUF`、`WSARecv()` 和 `WSASend()`。
> 贯穿项目：`D:\CodeRepository\claude\remote_control`。
> 学习范围：使用 completion port 统一接收 receive/send 完成通知；先理解单 worker，再扩展到两个 worker。同步 `accept()` 视为已经完成，不引入 `AcceptEx()`。

## 1. 最小 IOCP 学习主线

阶段三解决的是：

> 一次 Overlapped operation 在最终完成前，相关对象为什么必须继续存活？

阶段四继续解决：

> 多个 socket 的 operation 完成后，怎样统一进入一个队列，并由任意 completion worker 处理？

阶段三已经确定的四个结论在这里直接继续使用：

- `WSARecv()`、`WSASend()` 的函数原型和三种提交结果含义不变；与完成通知有关的实参会改为 IOCP 用法。
- operation 仍然必须拥有稳定的 `OVERLAPPED`、真实 buffer 和 connection 引用。
- receive 仍然只处理实际完成范围，零字节语义不变。
- send 仍然通过 `sendOffset` 处理部分完成。

阶段四新增的是 completion port、completion worker，以及 operation 所有权从提交线程转移到 worker 的过程。

常用术语：

| 术语 | 含义 |
| --- | --- |
| completion port | Windows 内核对象。它保存 completion packet，并协调等待这些 packet 的 worker。 |
| handle 关联 | 把一个支持 Overlapped I/O 的 handle（本阶段是 `SOCKET`）绑定到某个 completion port。 |
| completion packet | 某次 I/O 最终完成后进入 completion port 队列的通知记录。 |
| completion worker | 循环调用 `GetQueuedCompletionStatus()`，从 completion port 取出 packet 的线程。 |
| completion key | 关联 handle 时指定的整数值；该 handle 的普通 I/O completion 会带回这个值。 |
| operation pointer | 提交 I/O 时使用的 `OVERLAPPED*`；completion 到达时通过它找回具体 operation。 |
| concurrency value | completion port 允许同时运行的 completion worker 上限，不等于应用创建的 worker 总数。 |
| control packet | 应用通过 `PostQueuedCompletionStatus()` 人工投递的 packet，例如 worker 退出通知。 |

学习主线：

```text
阶段三：每次 operation 等待自己的 event
  → 阶段四：多个 socket 共享一个 completion port
  → 创建空 completion port
  → 把一个已连接 socket 关联到 port
  → 在堆上创建并提交 receive operation
  → completion worker 取出 packet
  → 通过 OVERLAPPED* 找回并回收 operation
  → 处理 receive/send completion
  → 扩展为两个 worker
  → 使用人工 packet 让 worker 退出
```

建议分五个学习单元推进：

1. **更换通知方式（第 4～5 节）**
   - 解决的问题：event 和 completion port 分别通知谁。
   - 学完自检：能画出 socket、port、packet、operation 和 worker 的关系。
2. **创建并关联（第 6～8 节）**
   - 解决的问题：如何创建 port、关联 socket、区分 key 与 operation pointer。
   - 学完自检：能解释 `CreateIoCompletionPort()` 两种调用模式的每个参数。
3. **提交并转移所有权（第 9～10 节）**
   - 解决的问题：operation 在提交线程和 completion worker 之间如何交接。
   - 学完自检：能判断三种提交结果下由谁回收 operation。
4. **取包并处理（第 11～15 节）**
   - 解决的问题：如何读取成功、失败、字节数并继续 receive/send。
   - 学完自检：能独立写出最小 completion worker 的关键分支。
5. **扩展与映射（第 16～21 节）**
   - 解决的问题：两个 worker、最小退出、项目映射和综合练习。
   - 学完自检：能闭卷复述一条完整 IOCP echo 链路。

---

## 2. 知识范围

### 2.1 核心内容

- `CreateIoCompletionPort()` 创建 completion port。
- `CreateIoCompletionPort()` 把 socket 关联到已有 port。
- `GetQueuedCompletionStatus()` 取得 completion。
- `PostQueuedCompletionStatus()` 投递人工控制 packet。
- `CloseHandle()` 关闭 completion port。
- `GetLastError()` 取得 Win32 API 错误。
- 阶段三的 `WSARecv()`、`WSASend()` 和 operation 生命周期。
- completion key 与 `OVERLAPPED*` 的不同职责。
- 成功 completion、失败 completion 和无 packet 错误的区别。
- operation 所有权从提交路径转移到 completion 路径。
- 单 worker 扩展为两个 worker。
- 把阶段三的 receive 零字节和 send 部分完成结论接入 completion handler。

### 2.2 后续内容

以下内容不作为阶段四的前置知识：

| 符号或主题 | 后续阶段 |
| --- | --- |
| `AcceptEx()`、预投递 accept、`SO_UPDATE_ACCEPT_CONTEXT` | 阶段五 |
| 连接状态机、有序发送队列、背压 | 阶段七 |
| 阻塞任务池与业务任务卸载 | 阶段八 |
| pending I/O 计数、`CancelIoEx()`、并发关闭、生产级安全停机 | 阶段九 |
| `GetQueuedCompletionStatusEx()` 和批量取包优化 | 完成最小模型后再扩展 |

本阶段的退出示例有一个明确前置条件：所有已提交 operation 的 completion 都已经被 worker 处理并回收。如何在并发服务器中可靠满足这个条件，留到阶段九。

---

## 3. 学习完成标准

完成本阶段后，应能够：

1. 解释 Overlapped I/O 与 IOCP 的分工。
2. 使用 `CreateIoCompletionPort()` 创建 port 并关联一个 socket。
3. 解释 completion key 属于 handle，`OVERLAPPED*` 属于一次 operation。
4. 解释为什么 IOCP 模式下通常不再为每次 operation 创建 event。
5. 根据阶段三的三种提交结果，判断 IOCP 模式下由谁回收 operation。
6. 解释为什么立即完成也不能在提交路径直接回收 operation。
7. 正确解释 `GetQueuedCompletionStatus()` 的四种结果组合。
8. 在成功或失败 completion 中都找回并回收 operation。
9. 在 completion handler 中正确复用 receive 零字节和 send 部分完成结论。
10. 解释两个 worker 为什么不固定属于某一个 socket。
11. 使用一个人工 packet 唤醒一个 worker，并解释为什么两个 worker 需要两个退出 packet。
12. 看懂项目 `postReceive()`、`postSend()` 和 `runCompletionWorker()` 的核心所有权链。
13. 解释为什么多 worker 的 completion 处理顺序不能直接作为业务顺序。

建议投入 8～12 小时。

---

## 4. 从 event 通知迁移到 completion port

阶段三的 event 模型：

```text
提交一个 operation
  → operation 完成
  → Windows 将该 operation 的 event 设为 signaled
  → 等待这个 event 的线程醒来
  → WSAGetOverlappedResult 取得最终结果
```

阶段四的 IOCP 模型：

```text
多个 socket 关联到同一个 completion port
  → 在任意已关联 socket 上提交 operation
  → operation 完成
  → Windows 向 port 队列放入 completion packet
  → 任意一个 completion worker 取出 packet
  → worker 取得结果并找回 operation
```

两种模型的对比：

| 对比项 | event 模型 | IOCP 模型 |
| --- | --- | --- |
| 通知目标 | 某个 operation 自己的 event | 多个 handle 共享的 completion port |
| 等待函数 | `WSAWaitForMultipleEvents()` | `GetQueuedCompletionStatus()` |
| 最终字节数 | `WSAGetOverlappedResult()` | `GetQueuedCompletionStatus()` 的输出参数 |
| operation 标识 | 等待路径已经知道对应 `OVERLAPPED` | packet 带回提交时的 `OVERLAPPED*` |
| 适合规模 | 少量 operation、教学与简单流程 | 多连接、多 operation、固定 worker 集合 |

IOCP 不负责提交 receive 或 send。真正提交 I/O 的仍然是：

- `WSARecv()`。
- `WSASend()`。

IOCP 改变的是“最终完成结果从哪里取得”。

### 4.1 IOCP 模式不再使用每次 operation 的 event

本阶段的 `OVERLAPPED` 保持零初始化：

```cpp
OVERLAPPED overlapped{};
```

因此 `overlapped.hEvent == nullptr`。普通 IOCP 路径：

1. 不调用 `WSACreateEvent()`。
2. 不调用 `WSAWaitForMultipleEvents()`。
3. 不在正常 completion 路径调用 `WSAGetOverlappedResult()`。
4. 由 `GetQueuedCompletionStatus()` 返回最终状态、字节数、key 和 `OVERLAPPED*`。

> 同一次 operation 只选择一种主要完成通知路径。不要既按 event 路径处理一次，又按 IOCP packet 再处理一次。

---

## 5. IOCP 中的核心对象

先只考虑一个已经连接的 socket、一个 receive operation 和一个 worker：

```text
connected SOCKET
  └─ 关联到 ──> completion port

IoOperation
  ├─ 继承 OVERLAPPED，标识本次 receive
  ├─ 拥有 receive buffer
  └─ 保存 ConnectionContext 的 shared_ptr

WSARecv
  └─ 在 connected SOCKET 上提交 IoOperation

operation 最终完成
  └─ Windows 向 completion port 放入 packet

completion worker
  └─ 从 packet 取得原来的 OVERLAPPED*
       └─ 转回 IoOperation* 并接管回收责任
```

逐项解释：

1. socket 与 completion port 是“关联”关系；port 不拥有也不会自动关闭 socket。
2. `IoOperation` 表示一次独立 I/O，并拥有本次 I/O 使用的真实 buffer。
3. `WSARecv()` 只收到 `OVERLAPPED*` 和 buffer 地址，不理解 C++ 所有权。
4. Windows 在 completion 中带回原来的 `OVERLAPPED*`，不会替应用释放 operation。
5. worker 根据应用约定，把这个指针还原为 `IoOperation*`，并恢复 C++ 所有权。

### 5.1 worker 最终要取得哪些信息

`GetQueuedCompletionStatus()` 使 worker 观察到：

| 信息 | 来源 | 作用 |
| --- | --- | --- |
| completion 是否成功 | 函数返回值 | 区分成功 I/O 与失败 I/O |
| 错误码 | 失败后立即调用 `GetLastError()` | 说明失败原因 |
| `transferredBytes` | 输出参数 | 当前 operation 实际传输字节数 |
| `completionKey` | 输出参数 | 标识关联 handle 时保存的 handle 级上下文 |
| `OVERLAPPED*` | 输出参数 | 找回本次 operation |

completion packet 不是应用代码中需要定义的 C++ 类。把它理解为 Windows 放入队列的一组完成信息即可。

---

## 6. 创建一个空 completion port

关键代码：

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

`reportWin32Error(_error)` 是示意错误报告函数；参数 `_error` 是刚刚保存的 Win32 错误码。

### 6.1 `CreateIoCompletionPort()` 参数说明

```cpp
HANDLE CreateIoCompletionPort(
    HANDLE FileHandle,
    HANDLE ExistingCompletionPort,
    ULONG_PTR CompletionKey,
    DWORD NumberOfConcurrentThreads);
```

创建空 port 时：

| 参数 | 示例值 | 作用 |
| --- | --- | --- |
| `FileHandle` | `INVALID_HANDLE_VALUE` | 只创建 completion port，不在这次调用中关联普通 handle。 |
| `ExistingCompletionPort` | `nullptr` | 当前没有已有 port，要求创建新的 port。 |
| `CompletionKey` | `0` | 创建空 port 时该参数被忽略。 |
| `NumberOfConcurrentThreads` | `0` | 使用系统默认并发值，即系统处理器数量。它不是要创建的 worker 数量。 |

返回值：

- 成功：新的 completion port `HANDLE`。
- 失败：`nullptr`，立即调用 `GetLastError()`。

### 6.2 `GetLastError()`

```cpp
DWORD GetLastError();
```

参数：无。

返回当前线程最近一次 Win32 API 错误码。它与阶段三使用的 `WSAGetLastError()` 不同：

| 失败的函数 | 读取错误的函数 |
| --- | --- |
| `CreateIoCompletionPort()` | `GetLastError()` |
| `GetQueuedCompletionStatus()` | `GetLastError()` |
| `PostQueuedCompletionStatus()` | `GetLastError()` |
| `CloseHandle()` | `GetLastError()` |
| `WSARecv()`、`WSASend()` | `WSAGetLastError()` |

错误码必须在失败调用之后立即保存，不能先调用其他可能改变线程错误状态的 Windows API。

### 6.3 并发值不等于 worker 数量

`NumberOfConcurrentThreads` 限制的是：Windows 最多允许多少个与该 port 关联的 worker 同时处于运行状态。

它不负责创建线程。应用仍要自己创建 `std::thread`：

```text
NumberOfConcurrentThreads
  → completion port 的调度上限

workerCount
  → 应用实际创建的 worker 线程数量
```

本阶段创建两个 worker，但 port 创建时使用 `0`，先使用系统默认并发值，避免过早调优。

---

## 7. 把一个已连接 socket 关联到 port

假设 `connectedSocket`：

- 已经连接。
- 支持 Overlapped I/O。
- 尚未关联到其他 completion port。

关键代码：

```cpp
HANDLE const associatedPort{CreateIoCompletionPort(
    reinterpret_cast<HANDLE>(connectedSocket),
    completionPort,
    0,
    0)};

if (associatedPort == nullptr)
{
    DWORD const error{GetLastError()};
    reportWin32Error(error);
}
```

### 7.1 关联模式的参数

1. **`FileHandle`**
   - 示例值：`reinterpret_cast<HANDLE>(connectedSocket)`。
   - 作用：指定要关联的 socket handle。类型转换只用于适配 API 类型，不转移所有权。
2. **`ExistingCompletionPort`**
   - 示例值：`completionPort`。
   - 作用：指定已经创建的目标 completion port。
3. **`CompletionKey`**
   - 示例值：`0`。
   - 作用：为这个 socket 保存 handle 级 key；当前示例不通过 key 查找连接。
4. **`NumberOfConcurrentThreads`**
   - 示例值：`0`。
   - 作用：关联到已有 port 时该参数会被忽略。

成功时返回传入的同一个 `completionPort`。失败返回 `nullptr`。

### 7.2 关联后的规则

1. 一个 handle 只能关联到一个 completion port。
2. 关联关系一直持续到该 handle 被关闭。
3. port 不会取得 socket 所有权。
4. socket 仍然使用 `closesocket()` 关闭，不能使用 `CloseHandle()`。
5. 关联本身不会自动提交 receive 或 send。
6. 只有关联之后提交的 Overlapped I/O，才会按当前模型向该 port 产生 completion。

如果监听 socket 由 `WSASocketW(..., WSA_FLAG_OVERLAPPED)` 创建，同步 `accept()` 返回的连接 socket 具有监听 socket 的相关属性。第一版可以继续同步 accept，再把每个已连接 socket 关联到 completion port。

---

## 8. completion key 与 operation pointer

这两个值最容易混淆。

### 8.1 completion key 属于 handle

关联 socket 时指定：

```cpp
CreateIoCompletionPort(
    reinterpret_cast<HANDLE>(connectedSocket),
    completionPort,
    socketCompletionKey,
    0);
```

同一个 socket 上的普通 completion 都会带回这个 `socketCompletionKey`。

### 8.2 `OVERLAPPED*` 属于一次 operation

每次提交时指定：

```cpp
WSARecv(
    connectedSocket,
    &operation->nativeBuffer,
    1,
    nullptr,
    &flags,
    operation,
    nullptr);
```

该 receive 完成后，`GetQueuedCompletionStatus()` 的 `lpOverlapped` 输出会得到这次提交使用的指针。

对比：

| 信息 | 粒度 | 典型用途 |
| --- | --- | --- |
| completion key | 每个关联 handle | 找到 socket 级上下文或区分控制用途 |
| `OVERLAPPED*` | 每次 operation | 找到 operation 类型、buffer 和关联 connection |

项目把普通 socket 的 completion key 设置为 `0`，连接和 operation 类型保存在 `IoOperation` 中：

```text
completionKey = 0
  → 不通过 key 查找连接

OVERLAPPED* → IoOperation*
  → operation.type 区分 Receive / Send
  → operation.connection 保持 ConnectionContext 存活
```

人工退出 packet 使用一个保留 key：

```cpp
constexpr ULONG_PTR SocketCompletionKey{0};
constexpr ULONG_PTR StopCompletionKey{1};
```

`StopCompletionKey` 只用于 `PostQueuedCompletionStatus()`，不用于普通 socket 关联。

---

## 9. IOCP operation 的对象与所有权

阶段三第 20 节已经分析过项目 `IoOperation` 的成员、构造函数和内部 buffer 关系。阶段四继续使用同一个对象模型；completion port 不要求 operation 再增加一套 buffer 字段。

### 9.1 阶段三对象模型在 IOCP 中的新用途

| 阶段三已经掌握的成员 | 在阶段四中的新用途 |
| --- | --- |
| `OVERLAPPED` 基类 | completion packet 带回它的地址，worker 再由该地址找回 `IoOperation`。 |
| `type` | worker 取得 completion 后区分 receive 和 send。 |
| `connection` | operation 跨线程等待 completion 时，继续保持 `ConnectionContext` 存活。 |
| `storage`、`sendBytes` | 提交线程返回后仍然拥有真实 I/O buffer。 |
| `sendOffset`、`nativeBuffer` | send 部分完成后更新剩余范围并重新提交。 |

receive operation 继续使用阶段三介绍过的三个构造实参：operation 类型、connection 和 buffer 大小。阶段四真正新增的问题不在对象内部，而在对象外部：提交函数返回后，由谁拥有并最终释放这个 operation？

### 9.2 为什么 operation 放在堆上

提交后，发起函数会返回，而 operation 必须继续存活。最小所有权链：

```text
提交前
  → std::unique_ptr<IoOperation> 拥有 operation

成功提交后
  → completion 路径负责未来回收

completion 被 worker 取出
  → worker 用 OVERLAPPED* 恢复 unique_ptr

处理结束
  → unique_ptr 析构并释放 operation、buffer 和 connection 引用
```

阶段三关于“在途期间地址、`OVERLAPPED` 和真实 buffer 必须稳定”的要求全部保持不变。后文的 `std::move(operation)` 只移动 `unique_ptr` 的所有权，不会移动堆上的 `IoOperation` 对象，因此 operation 地址仍然稳定。

---

## 10. 向已关联 socket 提交 receive

receive 构造函数已经让 `nativeBuffer` 指向 `storage`，因此提交函数只需要管理 operation 所有权并调用 `WSARecv()`：

```cpp
[[nodiscard]] bool postReceive(
    std::shared_ptr<ConnectionContext> const& _connection)
{
    auto operation{std::make_unique<IoOperation>(
        IoOperationType::Receive,
        _connection,
        8192)};

    IoOperation* const operationPointer{operation.release()};

    DWORD flags{0};
    int const result{WSARecv(
        _connection->socket,
        &operationPointer->nativeBuffer,
        1,
        nullptr,
        &flags,
        operationPointer,
        nullptr)};

    if (result == SOCKET_ERROR)
    {
        int const error{WSAGetLastError()};
        if (error != WSA_IO_PENDING)
        {
            operation.reset(operationPointer);
            reportWinsockError(error);
            return false;
        }
    }

    return true;
}
```

### 10.1 IOCP 模式下的 `WSARecv()` 参数

七个参数的基础含义已经在阶段三第 9.1 节学习。本阶段只关注与 IOCP 直接相关的变化：

| 参数 | 当前值 | 阶段四需要新增理解的内容 |
| --- | --- | --- |
| `s` | `_connection->socket` | 除了满足阶段三的 connected、Overlapped 条件，还必须先关联到当前 completion port。 |
| `lpBuffers`、`dwBufferCount`、`lpFlags` | 原来的 receive buffer、`1`、`&flags` | buffer 生命周期和 flags 语义与阶段三相同。 |
| `lpNumberOfBytesRecvd` | `nullptr` | 最终完成量改由 `GetQueuedCompletionStatus()` 输出。 |
| `lpOverlapped` | `operationPointer` | 指向堆上的 operation；提交成功后由 completion 路径负责未来回收。 |
| `lpCompletionRoutine` | `nullptr` | 不使用 completion routine，最终通知进入 completion port。 |

`flags` 仍然可以是局部变量：`WSARecv()` 返回前会读取输入值；延迟完成时不会再写回这个变量。本阶段使用普通 TCP receive，也不需要额外的完成 flags。

### 10.2 自定义函数参数

| 参数 | 作用 | 生命周期 |
| --- | --- | --- |
| `_connection` | 提供已关联 socket，并让新 operation 保存连接强引用。 | 当前函数通过引用使用；operation 内部复制 `shared_ptr`，因此 completion 到达前连接上下文仍存活。 |

`IoOperationType::Receive`、`_connection` 和 `8192` 的含义与阶段三第 20.2 节相同。新增点是 operation 现在由 `unique_ptr` 创建在堆上，以便把所有权交给 completion worker。

返回值：

- `true`：receive 已成功提交，未来由 completion 路径回收 operation。
- `false`：发生同步投递失败，当前函数已经重新接管并释放 operation。

### 10.3 把阶段三的三种结果转换为 IOCP 所有权

| `WSARecv()` 结果 | 阶段三已经得到的结论 | 阶段四新增的所有权结论 |
| --- | --- | --- |
| 返回 `0` | operation 已立即完成 | packet 仍会进入 port，由 completion worker 回收 |
| `SOCKET_ERROR + WSA_IO_PENDING` | operation 已成功提交 | completion worker 在未来回收 |
| `SOCKET_ERROR + 其他错误` | operation 未成功提交 | 不会有 packet，当前提交函数恢复 `unique_ptr` |

> IOCP 模式下，返回 `0` 只表示 I/O 立即完成，不表示提交路径可以直接处理并释放 operation。

默认情况下，即使 operation 立即完成，关联到 completion port 的 handle 仍会产生 completion packet。提交路径如果处理一次，worker 再处理一次，就会造成重复处理或重复释放。

### 10.4 为什么在调用 `WSARecv()` 前先 `release()`

两个 worker 已经运行时，立即完成的 packet 可能很快被其他线程取出。所有权交接必须在 worker 有机会看见该指针之前完成：

```text
unique_ptr 拥有 operation
  → release，约定 completion 路径负责回收
  → 调用 WSARecv
      ├─ 立即完成：worker 可以马上接管
      ├─ pending：worker 将来接管
      └─ 同步失败：不会有 packet，提交路径 reset 后重新接管
```

如果先调用 `WSARecv()`，返回后才 `release()`，立即 completion 可能与提交线程同时认为自己拥有 operation。

同步投递失败时，Winsock 保证不会再产生该 operation 的 completion，因此 `operation.reset(operationPointer)` 可以安全恢复所有权。

---

## 11. 从 completion port 取出一个 packet

关键调用：

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

DWORD const completionError{
    success == FALSE ? GetLastError() : ERROR_SUCCESS};
```

`GetLastError()` 紧跟在失败返回之后执行，先保存错误，再调用日志或业务函数。

这里真正由 `GetQueuedCompletionStatus()` 写入的只有三个变量：`transferredBytes`、`completionKey` 和 `overlapped`。

`completionError` 是调用结束后由应用自己保存的错误快照，不是第四个输出变量。成功时使用值为 `0` 的 `ERROR_SUCCESS` 占位，后续成功分支不会读取它。

### 11.1 `GetQueuedCompletionStatus()` 参数说明

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
| `CompletionPort` | `completionPort` | 要等待和取包的 completion port。 |
| `lpNumberOfBytesTransferred` | `&transferredBytes` | 普通 I/O packet 输出实际传输字节数；人工 packet 输出投递者指定的 bytes 值。 |
| `lpCompletionKey` | `&completionKey` | 输出关联 handle 时保存的 key，或人工 packet 指定的 key。 |
| `lpOverlapped` | `&overlapped` | 输出提交 operation 时使用的 `OVERLAPPED*`。它是“指针的输出地址”，因此参数类型是二级指针。 |
| `dwMilliseconds` | `INFINITE` | 无限等待，直到有 packet、port 被关闭或调用发生错误。 |

返回值：

- 非零：成功取出一个成功 I/O packet，或取出应用人工投递的 packet。
- `FALSE`：可能取出了失败 I/O packet，也可能根本没有取到 packet；必须结合 `overlapped` 判断。

### 11.2 为什么 `overlapped` 必须先初始化为 `nullptr`

当函数返回 `FALSE` 且没有取到 packet 时，`overlapped == nullptr` 是关键判断依据。

同时要记住：

> `FALSE` 且 `overlapped == nullptr` 时，`transferredBytes` 和 `completionKey` 没有可用含义，不要读取它们。

---

## 12. `GetQueuedCompletionStatus()` 的四种结果

下表采用本阶段约定：普通 I/O 总是使用非空 `OVERLAPPED*`，人工 control packet 总是使用空 `overlapped`。

1. **非零返回值，`overlapped` 非空**
   - 含义：取到普通 I/O 的成功 completion。
   - 下一步：恢复 operation 所有权，再按 receive/send 类型处理。
2. **返回 `FALSE`，`overlapped` 非空**
   - 含义：取到普通 I/O 的失败 completion。
   - 下一步：立即保存 `GetLastError()`，仍要恢复并回收 operation。
3. **非零返回值，`overlapped` 为空**
   - 含义：取到应用人工投递的 control packet。
   - 下一步：根据 `completionKey` 处理控制命令。
4. **返回 `FALSE`，`overlapped` 为空**
   - 含义：没有取到 packet；可能是超时、port 被关闭或调用错误。
   - 下一步：立即保存 `GetLastError()`，不要读取 bytes/key。

### 12.1 最重要的失败分支

```text
GetQueuedCompletionStatus 返回 FALSE
  ├─ overlapped != nullptr
  │    → 已经取到“失败 I/O”的 packet
  │    → operation 仍然存在
  │    → 必须回收 operation
  │
  └─ overlapped == nullptr
       → 没有普通 operation 可以回收
       → bytes 和 key 不可用
       → 按 port 等待错误处理
```

不能把所有 `FALSE` 都当成“worker 直接退出”。否则失败完成对应的 operation、buffer 和 connection 引用会泄漏。

### 12.2 有限超时时的补充

本阶段核心 worker 使用 `INFINITE`。如果以后传入有限毫秒数：

- 超时返回 `FALSE`。
- `overlapped == nullptr`。
- `GetLastError() == WAIT_TIMEOUT`。

有限超时并不会取消任何在途 I/O。

---

## 13. 最小 completion worker

先只分发 receive 和 send：

```cpp
constexpr ULONG_PTR StopCompletionKey{1};

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

        DWORD const completionError{
            success == FALSE ? GetLastError() : ERROR_SUCCESS};

        if (overlapped == nullptr)
        {
            if (success != FALSE && completionKey == StopCompletionKey)
            {
                return;
            }
            if (success != FALSE)
            {
                reportUnexpectedControlPacket(completionKey);
                continue;
            }

            reportCompletionPortError(completionError);
            return;
        }

        auto operation{std::unique_ptr<IoOperation>{
            static_cast<IoOperation*>(overlapped)}};

        if (success == FALSE)
        {
            handleIoFailure(std::move(operation), completionError);
            continue;
        }

        switch (operation->type)
        {
            case IoOperationType::Receive:
                handleReceiveCompletion(
                    std::move(operation),
                    transferredBytes);
                break;

            case IoOperationType::Send:
                handleSendCompletion(
                    std::move(operation),
                    transferredBytes);
                break;
        }
    }
}
```

### 13.1 自定义函数参数

1. **`runCompletionWorker(_completionPort)`**
   - `_completionPort`：worker 反复等待的 completion port；整个循环使用同一个 port。
2. **`reportUnexpectedControlPacket(_completionKey)`**
   - `_completionKey`：未识别的人工 key。
   - 作用：报告控制协议错误，不读取空 `overlapped`。
3. **`reportCompletionPortError(_error)`**
   - `_error`：`GetQueuedCompletionStatus()` 没有取到 packet 时保存的 Win32 错误。
   - 作用：记录 port 等待失败。
4. **`handleIoFailure(_operation, _error)`**
   - `_operation`：失败 operation 的唯一所有权。
   - `_error`：本次 I/O 的完成错误。
   - 作用：处理连接错误；函数返回后 operation 必须被释放。
5. **`handleReceiveCompletion(_operation, _transferredBytes)`**
   - `_operation`：成功完成的 receive operation。
   - `_transferredBytes`：本次实际接收字节数。
   - 作用：处理有效接收数据。
6. **`handleSendCompletion(_operation, _transferredBytes)`**
   - `_operation`：成功完成的 send operation。
   - `_transferredBytes`：本次实际发送字节数。
   - 作用：推进部分发送进度。

### 13.2 为什么可以转回 `IoOperation*`

提交时传入的原始指针来自一个真实的 `IoOperation`。下面第二行只发生指针类型转换，不会创建或移动对象：

```cpp
IoOperation* const operationPointer{operation.release()};
OVERLAPPED* const submittedOverlapped{operationPointer};
```

`IoOperation` 公开继承 `OVERLAPPED`，因此 `IoOperation*` 可以转换为它内部的 `OVERLAPPED` 基类指针。

前面的 `postReceive()` 直接把 `operationPointer` 传给 `lpOverlapped`，函数调用会完成同样的转换。completion 到达后，Windows 带回的正是这个基类指针，所以可以执行：

```cpp
auto* operationPointer{static_cast<IoOperation*>(overlapped)};
```

这个向下转换成立有两个前提：

1. 原始指针确实来自一个仍然存活的 `IoOperation`。
2. 当前 packet 是普通 I/O completion，而不是 `overlapped == nullptr` 的 control packet。

不能把任意 `OVERLAPPED*` 都强制解释为 `IoOperation*`。worker 必须先排除空指针，再遵守“普通 operation 都由 `IoOperation` 表示”的应用约定。

### 13.3 为什么先恢复 `unique_ptr`

`overlapped` 非空时，它来自某个真实 `IoOperation`。立即恢复：

```cpp
auto operation{std::unique_ptr<IoOperation>{
    static_cast<IoOperation*>(overlapped)}};
```

从这一行开始：

- worker 路径重新拥有 operation。
- 成功、失败、`continue` 或函数返回都不会遗漏释放。
- operation 内部的 buffer 和 `connection` 强引用也由同一个 `unique_ptr` 管理。

一个 completion packet 只会被一个 worker 取出，因此普通 completion 只会恢复一次 `unique_ptr`。

---

## 14. 处理 receive completion

worker 只在 `success != FALSE` 时进入成功处理函数。echo 数据必须先复制到新的 send operation，之后才能释放 receive operation：

```cpp
void handleReceiveCompletion(
    std::unique_ptr<IoOperation> _operation,
    DWORD _transferredBytes)
{
    std::shared_ptr<ConnectionContext> const connection{
        _operation->connection};

    if (_transferredBytes == 0)
    {
        closeMinimalConnection(connection);
        return;
    }

    auto sendOperation{makeEchoSendOperation(
        connection,
        _operation->storage.data(),
        _transferredBytes)};

    _operation.reset();

    if (!postSend(std::move(sendOperation)))
    {
        closeMinimalConnection(connection);
    }
}
```

### 14.1 参数说明

| 参数 | 作用 |
| --- | --- |
| `_operation` | 已成功完成的 receive operation。按值接收 `unique_ptr`，使当前函数明确拥有并最终释放它。 |
| `_transferredBytes` | 本次 receive completion 的实际字节数。成功时不会超过提交的 `nativeBuffer.len`，只处理 `[0, _transferredBytes)`。 |

辅助函数：

1. **`closeMinimalConnection(_connection)`**
   - `_connection`：当前连接的 `shared_ptr`。
   - 作用：在“每个连接一次只保留一个 operation”的前提下关闭连接；并发关闭留到后续阶段。
2. **`makeEchoSendOperation(_connection, _data, _size)`**
   - `_connection`：当前连接。
   - `_data`：有效数据的首地址。
   - `_size`：需要复制的字节数。
   - 作用：只复制 `[0, _size)`，创建一个自行拥有 echo 数据的 send operation。
3. **`postSend(_operation)`**
   - `_operation`：send operation 的唯一所有权。
   - 作用：提交 echo send；成功后由 completion 路径回收。

`makeEchoSendOperation()` 沿用阶段三的 send buffer 规则：只复制 `[0, _size)`，并让新 operation 自己拥有复制后的字节。阶段四新增的重点是复制完成后显式释放 receive operation，再把 send operation 的所有权交给新的 completion 路径。

### 14.2 阶段三 receive 数据语义保持不变

阶段三第 13 节的三个结论直接适用：只处理 `[0, _transferredBytes)`；非零长度 TCP receive 成功完成 0 bytes 表示对端正常关闭；一次 receive completion 不是一个完整 Packet。阶段四只改变 `_transferredBytes` 的来源——它现在由 GQCS 提供。

失败 completion 不进入本函数，而是在 worker 的 `success == FALSE` 分支处理。

### 14.3 为什么当前链路一次只有一个 operation 在途

下一次 receive 只在完整 send 结束后提交：

```text
receive A 提交
  → receive A completion
  → send A 提交
  → 处理 send A 的所有部分完成
  → 提交 receive B
```

因此当前最小 echo 流程不会同时给同一连接提交两个 operation。

---

## 15. 处理 send 与部分发送

### 15.1 提交 send

`IoOperation` 自己拥有完整 `sendBytes`：

```cpp
[[nodiscard]] bool postSend(
    std::unique_ptr<IoOperation> _operation)
{
    if (!_operation)
    {
        return false;
    }

    static_cast<OVERLAPPED&>(*_operation) = {};
    _operation->refreshSendBuffer();
    if (_operation->nativeBuffer.len == 0)
    {
        return false;
    }

    std::shared_ptr<ConnectionContext> const connection{
        _operation->connection};
    IoOperation* const operationPointer{_operation.release()};

    int const result{WSASend(
        connection->socket,
        &operationPointer->nativeBuffer,
        1,
        nullptr,
        0,
        operationPointer,
        nullptr)};

    if (result == SOCKET_ERROR)
    {
        int const error{WSAGetLastError()};
        if (error != WSA_IO_PENDING)
        {
            _operation.reset(operationPointer);
            reportWinsockError(error);
            return false;
        }
    }

    return true;
}
```

### 15.2 IOCP 模式下的 `WSASend()` 参数

七个参数的基础含义已经在阶段三第 17.1 节学习。本阶段只关注与 IOCP 直接相关的变化：

| 参数 | 当前值 | 阶段四需要新增理解的内容 |
| --- | --- | --- |
| `s` | `connection->socket` | socket 必须已经关联到当前 completion port。 |
| `lpBuffers`、`dwBufferCount`、`dwFlags` | 剩余发送范围、`1`、`0` | send buffer 生命周期和 flags 语义与阶段三相同。 |
| `lpNumberOfBytesSent` | `nullptr` | 最终本次完成量改由 GQCS 输出。 |
| `lpOverlapped` | `operationPointer` | 提交成功后由 completion 路径负责回收这个堆 operation。 |
| `lpCompletionRoutine` | `nullptr` | 不使用 completion routine，最终通知进入 completion port。 |

自定义函数参数：

| 参数 | 作用 |
| --- | --- |
| `_operation` | 拥有完整 send 数据和当前 `sendOffset`。函数成功提交后把所有权交给 completion 路径；同步失败时重新接管。 |

`static_cast<OVERLAPPED&>(*_operation) = {}` 只清零 `OVERLAPPED` 基类，不清除 `sendBytes`、`sendOffset`、`connection` 和 `type`。

阶段三第 19.1 节已经解释过复用前为什么要重置；IOCP 模式没有 event，因此不需要 `WSAResetEvent()`。

### 15.3 推进部分发送

```cpp
void handleSendCompletion(
    std::unique_ptr<IoOperation> _operation,
    DWORD _transferredBytes)
{
    std::shared_ptr<ConnectionContext> const connection{
        _operation->connection};

    if (_operation->sendOffset > _operation->sendBytes.size())
    {
        reportInvalidSendProgress();
        closeMinimalConnection(connection);
        return;
    }

    std::size_t const remainingBytes{
        _operation->sendBytes.size() - _operation->sendOffset};
    std::size_t const completedBytes{
        static_cast<std::size_t>(_transferredBytes)};

    if (completedBytes == 0 || completedBytes > remainingBytes)
    {
        reportInvalidSendProgress();
        closeMinimalConnection(connection);
        return;
    }

    _operation->sendOffset += completedBytes;
    if (_operation->sendOffset < _operation->sendBytes.size())
    {
        if (!postSend(std::move(_operation)))
        {
            closeMinimalConnection(connection);
        }
        return;
    }

    _operation.reset();
    if (!postReceive(connection))
    {
        closeMinimalConnection(connection);
    }
}
```

参数：

| 参数 | 作用 |
| --- | --- |
| `_operation` | 已成功完成一次 send 的 operation，仍拥有完整业务数据和累计进度。 |
| `_transferredBytes` | 当前这一次 send completion 消费的字节数，不是累计值。 |

辅助函数：

1. **`reportInvalidSendProgress()`**
   - 参数：无。
   - 作用：报告零进展或超出剩余范围的异常完成结果。
2. **`postSend(_operation)`**
   - `_operation`：尚未发送完的 operation 唯一所有权。
   - 作用：刷新剩余范围并重新提交；同步失败时返回 `false`。
3. **`postReceive(_connection)`**
   - `_connection`：当前连接的 `shared_ptr`。
   - 作用：完整 send 结束后提交下一次 receive；同步失败时返回 `false`。
4. **`closeMinimalConnection(_connection)`**
   - `_connection`：当前连接的 `shared_ptr`。
   - 作用：在进度异常或续投同步失败时关闭当前最小模型中的连接。

阶段三第 19 节已经完成 `sendOffset` 的算术推演。阶段四新增的是：每次部分完成后，同一个 operation 会再次转交给 completion 路径；完整发送后先释放 send operation，再提交下一次 receive。

部分 send 或下一次 receive 如果同步投递失败，不能忽略返回值，否则连接会保留在“没有 operation 在途、也不会再收到 completion”的停滞状态。

---

## 16. 从一个 worker 扩展到两个 worker

先确认单 worker 可以正确处理 completion，再创建两个：

```cpp
std::size_t constexpr WorkerCount{2};

std::vector<std::thread> workers;
workers.reserve(WorkerCount);

for (std::size_t index{0}; index < WorkerCount; ++index)
{
    workers.emplace_back(
        runCompletionWorker,
        completionPort);
}
```

变量作用：

| 变量 | 作用 |
| --- | --- |
| `WorkerCount` | 应用实际创建的 completion worker 数量。 |
| `workers` | 拥有两个 `std::thread` 对象，后续用于 `join()`。 |
| `completionPort` | 两个 worker 共同等待的同一个 port。 |

`workers.emplace_back(runCompletionWorker, completionPort)` 的两个实参：

| 实参 | 作用 |
| --- | --- |
| `runCompletionWorker` | 新线程执行的入口函数。 |
| `completionPort` | 传给入口函数参数 `_completionPort`；两个线程得到的是同一个 port handle 值。 |

循环变量 `index` 只负责创建两次线程，并没有传给 worker，因此它不是连接编号，也不会形成 worker 与连接的绑定关系。

### 16.1 worker 不固定属于某条连接

```text
socket A completion ─┐
socket B completion ─┼─> 同一个 completion port
socket C completion ─┘
                         ├─ worker 0 取下一包
                         └─ worker 1 取下一包
```

哪个 worker 取到哪个 completion 由调度决定：

- worker 0 可能先处理 socket A，随后处理 socket C。
- worker 1 可能处理 socket B，随后又处理 socket A。
- 连接身份必须来自 completion key 或 operation 内部的 `connection`，不能来自“当前 worker 编号”。

### 16.2 当前最小模型避免额外并发知识

为了不提前依赖阶段七的连接状态机和有序发送队列，最小 echo 链路对每条连接采用更严格的串行流程：

```text
receive
  → receive completion
  → send
  → 所有部分 send completion
  → 下一次 receive
```

因此同一连接同一时刻最多一个 operation 在途。两个 worker 仍然可以并行处理不同连接。

之后可以扩展为每连接最多一个 receive 和一个 send 同时在途，但共享连接状态的同步属于后续阶段。

### 16.3 completion 处理顺序不是业务顺序

两个 worker 可以同时处理不同 packet：

```text
worker 0 取到 operation A
worker 1 取到 operation B
  → 两个 handler 可能并行执行
  → 先完成业务处理的不一定是先取包的那个
```

一个 completion 只说明“这一项 I/O 已经结束”，不说明它在业务协议中应该先于其他 operation 生效。不同连接之间本来就没有统一业务顺序；同一连接如果允许多个 send 同时在途，还需要额外的有序发送设计。

当前最小 echo 链一次只推进一个 connection operation，因此用串行链路避开该问题，而不是依赖 completion 的到达或处理顺序。

---

## 17. 使用人工 packet 让 worker 退出

### 17.1 `PostQueuedCompletionStatus()`

关键调用：

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

函数声明：

```cpp
BOOL PostQueuedCompletionStatus(
    HANDLE CompletionPort,
    DWORD dwNumberOfBytesTransferred,
    ULONG_PTR dwCompletionKey,
    LPOVERLAPPED lpOverlapped);
```

参数：

| 参数 | 示例值 | 作用 |
| --- | --- | --- |
| `CompletionPort` | `completionPort` | 要投递 control packet 的 port。 |
| `dwNumberOfBytesTransferred` | `0` | worker 取包时得到的 bytes 值；退出 packet 不使用它。 |
| `dwCompletionKey` | `StopCompletionKey` | worker 用于识别退出命令的保留 key。 |
| `lpOverlapped` | `nullptr` | 表明它不是普通 I/O operation。 |

返回值：

- 非零：packet 已成功放入队列。
- `FALSE`：投递失败，立即调用 `GetLastError()`。

该函数不会验证三个自定义值。`lpOverlapped` 甚至不必指向真实 `OVERLAPPED`；本阶段约定退出 packet 必须使用 `nullptr`。

### 17.2 两个 worker 需要两个退出 packet

一个 packet 只会被一个等待中的 worker 取出：

```cpp
for (std::size_t index{0}; index < workers.size(); ++index)
{
    BOOL const posted{PostQueuedCompletionStatus(
        completionPort,
        0,
        StopCompletionKey,
        nullptr)};

    if (posted == FALSE)
    {
        DWORD const error{GetLastError()};
        reportWin32Error(error);
        break;
    }
}
```

如果只投递一个：

```text
worker 0 取到 StopCompletionKey 并退出
worker 1 仍阻塞在 GetQueuedCompletionStatus
主线程 join worker 1 时无法结束
```

投递失败时，不能假设对应 worker 已经醒来，也不能盲目进入 `join()`。生产级失败恢复属于安全停机主题。

### 17.3 `CloseHandle()`

```cpp
BOOL CloseHandle(HANDLE hObject);
```

| 参数 | 作用 |
| --- | --- |
| `hObject` | 要关闭的 completion port handle。示例传入 `completionPort`。 |

返回值：

- 非零：关闭成功。
- `FALSE`：关闭失败，立即调用 `GetLastError()`。

completion port 使用 `CloseHandle()`，socket 使用 `closesocket()`。两者不能混用。

### 17.4 本阶段的最小收尾顺序

只有在满足以下前置条件时执行：

> 不再提交新 I/O；所有已提交 operation 的 completion 都已被 worker 处理并回收；所有关联 socket 都已使用 `closesocket()` 关闭。

然后：

```text
为每个 worker 投递一个 StopCompletionKey packet
  → 每个 worker 取到一个退出 packet
  → join 所有 worker
  → CloseHandle(completionPort)
```

不要把“直接关闭 port 让 worker 报错返回”作为正常退出流程。port 被关闭时，等待中的 `GetQueuedCompletionStatus()` 会走 `FALSE + overlapped == nullptr` 的错误路径。

---

## 18. 最小 IOCP echo 链路

第一版保留同步 `accept()`，只把已连接 socket 的 receive/send 改为 IOCP：

```text
创建 completion port
  → 创建两个 completion worker
  → 同步 accept 得到 connected socket
  → 将 connected socket 关联到 completion port
  → postReceive
  → receive completion 进入 port
  → 某个 worker 取出 completion
  → 恢复 receive operation 所有权
  → 把有效字节复制到 send operation
  → postSend
  → send completion 进入 port
  → worker 推进 sendOffset
      ├─ 仍有剩余：再次 postSend
      └─ 全部完成：postReceive
```

这条链路故意一次只推进一个 connection operation，因此不需要提前引入发送队列和连接状态机。

### 18.1 各时刻由谁拥有 operation

| 时刻 | 所有者 |
| --- | --- |
| 创建 receive/send operation 后 | 当前 `unique_ptr` |
| `release()` 后且提交成功 | completion 路径的所有权约定 |
| 同步投递失败 | 提交函数重新恢复的 `unique_ptr` |
| worker 取得非空 `OVERLAPPED*` 后 | worker 恢复的 `unique_ptr` |
| completion handler 处理期间 | handler 的 `unique_ptr` 参数 |
| handler 返回且未重新提交 | `unique_ptr` 析构，operation 被释放 |
| 部分发送重新提交 | `postSend()` 再次把所有权交给 completion 路径 |

### 18.2 每条连接的循环

```text
Receive
  → 收到 0 bytes：连接结束
  → 收到 N bytes：只使用 [0, N)
  → 创建拥有这 N bytes 的 Send operation
  → 处理全部部分发送
  → 再提交下一次 Receive
```

TCP 仍然没有消息边界。如果目标不是纯 echo，receive completion 得到的字节仍要进入协议累计缓冲区。

---

## 19. 映射到项目

只阅读与阶段四直接相关的成员和函数：

```text
server_transport/internal/RemoteControlTransportImpl.h
  → IoOperation
  → m_completionPort
  → m_completionThreads
  → postReceive()
  → postSend()
  → runCompletionWorker()
  → handleReceiveCompletion()
  → handleSendCompletion()

server_transport/src/RemoteControlTransport.cpp
  → completion port 创建
  → completion worker 创建
  → postReceive()
  → postSend()
  → runCompletionWorker()
  → receive/send completion handler
```

### 19.1 项目 receive 所有权链

```text
postReceive
  → make_unique<IoOperation>
  → release 得到 IoOperation*
  → WSARecv
      ├─ 立即完成或 pending：completion worker 未来接管
      └─ 同步失败：reset 恢复 unique_ptr

runCompletionWorker
  → GetQueuedCompletionStatus
  → OVERLAPPED* 转回 IoOperation*
  → unique_ptr 恢复所有权
  → handleReceiveCompletion
      → 处理有效字节
      → 再次 postReceive
```

### 19.2 项目 send 所有权链

```text
postSend(unique_ptr<IoOperation>)
  → refreshSendBuffer
  → release
  → WSASend
  → completion worker 恢复 unique_ptr
  → handleSendCompletion
      ├─ 部分完成：更新 sendOffset，再次 postSend
      └─ 全部完成：释放当前 send operation，后续发送策略留到阶段七
```

### 19.3 当前只观察这些事实

1. `m_completionPort` 是所有 completion worker 共享的 port。
2. 普通 socket completion key 使用 `0`。
3. `IoOperation::connection` 保存连接强引用。
4. `postReceive()` 和 `postSend()` 在同步失败时恢复 operation 所有权。
5. `runCompletionWorker()` 即使收到失败 completion，也会先恢复 operation。
6. `handleSendCompletion()` 使用 `sendOffset` 处理部分发送。

当前最小 echo 示例采用更严格的 `receive → send → receive` 串行链；项目则分别维持 receive 和 send 链，同一连接可以各有一个 receive 和一个 send 在途。阶段四只观察两条链各自的 operation 所有权，不展开项目中的发送队列、背压和连接状态机。

项目中的 `Accept` 分支、pending 计数和停止状态也分别属于后续阶段；当前不依赖它们解释 receive/send completion 主链。

---

## 20. 常见错误

| 错误 | 症状 | 根因 |
| --- | --- | --- |
| 创建 port 后忘记关联 connected socket | receive/send 已提交但 worker 收不到预期 packet | socket 不属于该 completion port |
| 把 IOCP 当成提交 I/O 的 API | 不知道 `WSARecv()`、`WSASend()` 放在哪里 | 混淆“提交 operation”和“取得 completion” |
| IOCP operation 仍创建并等待 event | 同一次 I/O 出现两套处理路径 | 没有从 event 模型切换到 port 模型 |
| `WSARecv()` 返回 `0` 后直接释放 operation | worker 随后访问悬空 `OVERLAPPED*` | 忘记立即完成仍会产生 completion packet |
| 调用 `WSARecv()` 后才转移所有权 | 偶发重复所有权或重复释放 | 立即 completion 可能被其他 worker 很快取出 |
| 同步投递失败后仍等待 packet | operation 永远得不到 completion | 其他同步错误表示没有成功提交 |
| GQCS 返回 `FALSE` 就直接退出 | operation 泄漏、connection 无法释放 | `overlapped` 非空时其实取到了失败 I/O packet |
| `FALSE + overlapped == nullptr` 时读取 key/bytes | 控制流随机或误判 Stop key | 没有 packet 时两个输出值不可用 |
| GQCS 失败后调用 `WSAGetLastError()` | 得到错误来源不明确的值 | GQCS 是 Win32 API，应调用 `GetLastError()` |
| 把 completion key 当作 operation pointer | 无法区分同一 socket 上的 receive/send | key 是 handle 级，`OVERLAPPED*` 才是 operation 级 |
| worker 收到 `OVERLAPPED*` 后不恢复 RAII 所有权 | operation 和 buffer 泄漏 | 原生指针没有重新交给 `unique_ptr` |
| 假设一个 worker 固定服务一条连接 | 多 worker 时连接上下文错乱 | 任意 worker 都可能取得该连接的下一次 completion |
| 把 completion 处理顺序当作业务顺序 | 响应次序不稳定 | 多 worker handler 可以并行执行 |
| 假设一次 send 完成全部数据 | echo 数据被截断 | 忽略 `transferredBytes` 和 `sendOffset` |
| 忽略部分 send 或下一次 receive 的同步投递失败 | 连接仍存在，但再也没有 completion | 续投失败后既未关闭连接，也没有新的 operation 在途 |
| 两个 worker 只投一个退出 packet | 一个 worker 退出，另一个永久等待 | 一个 packet 只能被一个 worker 取走 |
| 用 `CloseHandle()` 关闭 socket | 资源管理错误 | socket 必须使用 `closesocket()` |
| worker 未退出就关闭 port | GQCS 进入错误路径，收尾顺序混乱 | port 生命周期短于 worker |

---

## 21. 阶段练习与验收

按编号完成练习，先根据验收标准自行检查，再查看“参考答案与解释”。API 参数题重在理解当前值、输入输出方向、实际作用和生命周期，不要求死记函数原型。

### 21.1 任务一：画核心对象关系图

**练习**

以阶段三的 `IoOperation → OVERLAPPED / buffer / connection` 关系为起点，再加入下面这些 IOCP 对象：

```text
connected SOCKET
completion port
IoOperation / OVERLAPPED
真实 buffer
completion packet
completion worker
```

使用“关联到”“拥有”“提交”“带回指针”“取出”“恢复所有权”说明关系。

**验收标准**

- [ ] 能指出 socket 与 port 是关联关系，不是所有权关系。
- [ ] 能指出 buffer 由 `IoOperation` 拥有。
- [ ] 能指出普通 packet 为什么能够找回 operation。
- [ ] 能指出 worker 从哪里取得 completion。
- [ ] 能解释 Windows 不负责释放 `IoOperation`。

**参考答案与解释**

```text
connected SOCKET
  └─ 关联到 ──> completion port

IoOperation
  ├─ 包含 ──> OVERLAPPED
  └─ 拥有 ──> 真实 buffer

WSARecv / WSASend
  └─ 在 socket 上提交 ──> IoOperation

operation 完成
  └─ 产生 ──> completion packet
                  ├─ 进入 completion port
                  └─ 带回原来的 OVERLAPPED*

completion worker
  └─ 取出 packet
       └─ OVERLAPPED* 转回 IoOperation*
            └─ 恢复 unique_ptr 所有权
```

Windows 只保存并带回原生指针。operation、buffer 和连接引用的 C++ 生命周期仍由应用负责。

### 21.2 任务二：解释 `CreateIoCompletionPort()` 的两种调用

**练习**

逐个解释下面八个实参：

```cpp
CreateIoCompletionPort(
    INVALID_HANDLE_VALUE,
    nullptr,
    0,
    0);

CreateIoCompletionPort(
    reinterpret_cast<HANDLE>(connectedSocket),
    completionPort,
    0,
    0);
```

然后回答：

1. 两次调用的返回值分别是什么？
2. 第二次调用为什么不创建新的 port？
3. `NumberOfConcurrentThreads` 为什么不是 worker 数量？
4. 失败后调用哪个错误函数？

**验收标准**

- [ ] 能解释两次调用中每个参数的当前值。
- [ ] 知道创建空 port 时 key 被忽略。
- [ ] 知道关联已有 port 时并发参数被忽略。
- [ ] 知道成功关联返回同一个 `completionPort`。
- [ ] 知道一个 handle 只能关联一个 port。
- [ ] 使用 `GetLastError()` 而不是 `WSAGetLastError()`。

**参考答案与解释**

1. **创建空 port**
   - 第一个参数：`INVALID_HANDLE_VALUE`，表示不关联普通 handle。
   - 第二个参数：`nullptr`，表示创建新的 completion port。
   - 第三个参数：`0`，在创建模式下被忽略。
   - 第四个参数：`0`，使用系统默认并发值。
2. **关联 socket**
   - 第一个参数：connected socket 的 handle 表示。
   - 第二个参数：已有的 `completionPort`。
   - 第三个参数：`0`，作为 socket 的 handle 级 key。
   - 第四个参数：`0`，在关联模式下被忽略。

第一次成功返回新 port；第二次成功返回传入的同一个 port。

`NumberOfConcurrentThreads` 是 completion port 的调度并发上限，不会创建 worker；worker 数量由应用创建多少个 `std::thread` 决定。两次调用失败都立即使用 `GetLastError()` 保存 Win32 错误码。

### 21.3 任务三：把提交结果转换为 IOCP 所有权

**练习**

阶段三已经解释过下面三种结果的 API 含义：

- 返回 `0`。
- `SOCKET_ERROR + WSA_IO_PENDING`。
- `SOCKET_ERROR + WSAECONNRESET`。

本题只分析阶段四新增的所有权问题：

1. 每种结果下，提交函数和 completion worker 谁负责最终释放 operation？
2. 哪些结果允许 worker 取得同一个 `OVERLAPPED*`？
3. 为什么必须在调用 `WSARecv()` 前执行 `release()`？
4. `std::move(operation)` 是否会改变堆上 `IoOperation` 的地址？

**验收标准**

- [ ] 返回 `0` 和 `WSA_IO_PENDING` 都由 completion worker 回收。
- [ ] 其他同步错误由提交函数恢复并释放 `unique_ptr`。
- [ ] 能解释立即 completion 与所有权转移的竞态。
- [ ] 知道移动 `unique_ptr` 只转移所有权，不会移动堆上的 operation。

**参考答案与解释**

| `WSARecv()` 结果 | completion 路径 | 最终回收者 |
| --- | --- | --- |
| 返回 `0` | packet 可能立刻被 worker 取出 | completion worker |
| `WSA_IO_PENDING` | operation 完成后产生 packet | completion worker |
| 其他同步错误 | 不会产生 packet | 提交函数恢复的 `unique_ptr` |

`release()` 必须先于提交，因为立即 completion 可能在提交函数返回前就被另一个 worker 取出。

`std::move(operation)` 只把同一个堆对象的所有权交给另一个 `unique_ptr`，不会改变 `IoOperation` 的地址，因此 Windows 保存的 `OVERLAPPED*` 仍然有效。

### 21.4 任务四：完成 GQCS 结果矩阵

**练习**

分别补全下面四种组合中的 packet、operation 和错误处理：

1. 返回值非零，`overlapped` 非空。
2. 返回 `FALSE`，`overlapped` 非空。
3. 返回值非零，`overlapped` 为空。
4. 返回 `FALSE`，`overlapped` 为空。

并回答：哪一种组合中不能读取 `transferredBytes` 和 `completionKey`？

**验收标准**

- [ ] `FALSE + 非空` 被识别为失败 I/O completion。
- [ ] `非零 + 空` 被识别为人工 control packet。
- [ ] `FALSE + 空` 被识别为没有取到 packet。
- [ ] 所有 `FALSE` 都立即保存 `GetLastError()`。
- [ ] 知道 `FALSE + 空` 时 bytes/key 不可用。

**参考答案与解释**

1. **返回值非零，`overlapped` 非空**
   - 取到成功 I/O packet。
   - 恢复并处理 operation。
   - 不调用 `GetLastError()`。
2. **返回 `FALSE`，`overlapped` 非空**
   - 取到失败 I/O packet。
   - 仍要恢复并回收 operation。
   - 立即保存 `GetLastError()`。
3. **返回值非零，`overlapped` 为空**
   - 取到人工 packet。
   - 根据 key 处理，不恢复 operation。
   - 不调用 `GetLastError()`。
4. **返回 `FALSE`，`overlapped` 为空**
   - 没有取到 packet。
   - 不读取 bytes/key，也没有 operation 可以恢复。
   - 立即保存 `GetLastError()`。

### 21.5 任务五：写出最小 worker 的关键分支

**练习**

不照抄完整函数，独立写出：

1. 五个 `GetQueuedCompletionStatus()` 参数。
2. 失败后立即保存 `GetLastError()`。
3. `overlapped == nullptr` 的 control/error 分支。
4. 非空 `overlapped` 转回 `IoOperation*`。
5. 使用 `unique_ptr` 恢复所有权。
6. 失败 completion 与成功 receive/send 分发。
7. 说明这个 `static_cast` 为什么成立，以及什么指针不能这样转换。
8. 区分 GQCS 的三个输出变量与应用自己保存的 `completionError`。

**验收标准**

- [ ] `overlapped` 调用前初始化为 `nullptr`。
- [ ] 错误码在任何其他 Windows API 前保存。
- [ ] Stop packet 使用 `overlapped == nullptr` 和保留 key。
- [ ] `FALSE + 非空` 不直接退出。
- [ ] operation 只恢复一次 `unique_ptr`。
- [ ] receive/send handler 按值接收 `unique_ptr`。
- [ ] 只把来源于 `IoOperation` 的非空 `OVERLAPPED*` 转回 `IoOperation*`。
- [ ] 知道 `completionError` 不是 GQCS 的输出参数。

**参考答案与解释**

正确顺序：

```text
准备三个输出变量
  → GQCS
  → 立即保存失败错误
  → 先判断 overlapped 是否为空
      ├─ 空：control packet 或 port 错误
      └─ 非空：恢复 unique_ptr
            ├─ success == FALSE：处理失败并释放
            └─ success != FALSE：按 type 分发
```

先恢复 RAII 所有权后，任何 `continue` 或返回路径都不会遗失 operation。

向下转换之所以安全，是因为提交时传入的指针来自 `IoOperation` 的 `OVERLAPPED` 基类；空 control packet 和来源不明的 `OVERLAPPED*` 都不能这样转换。GQCS 只有 bytes、key 和 `OVERLAPPED*` 三个输出，`completionError` 是失败后立即取得的本地错误快照。

五个 GQCS 实参依次是：当前 worker 等待的 port、bytes 输出地址、key 输出地址、`OVERLAPPED*` 输出地址和等待时间 `INFINITE`。

### 21.6 任务六：把数据语义接入 completion handler

**练习**

阶段三已经练习过 receive 有效范围、零字节语义和部分发送算术。本题只写它们在 completion handler 中的处理顺序：

1. GQCS 提供 `_transferredBytes` 后，receive handler 怎样区分关闭与有效数据？
2. send handler 怎样使用当前 completion 的 `_transferredBytes` 推进 `sendOffset`？
3. 仍有剩余数据时，重新提交前保留什么、重置什么、更新什么？
4. 完整发送后，send operation 与下一次 receive 按什么顺序处理？
5. 为什么不调用 `WSAResetEvent()`？
6. 重新提交同步失败时为什么必须结束当前最小连接？

**验收标准**

- [ ] 正确识别对端正常关闭。
- [ ] 不把 buffer 容量当成完成字节数。
- [ ] `sendOffset` 使用累计完成量。
- [ ] operation 继续拥有完整 `sendBytes`。
- [ ] 重新提交前只清零 `OVERLAPPED` 基类并刷新 `WSABUF`。
- [ ] 知道 IOCP operation 没有 event 需要重置。
- [ ] 知道续投同步失败后要结束当前最小连接，不能让连接停在无 operation 状态。

**参考答案与解释**

```text
receive completion
  ├─ 0 bytes：关闭当前连接
  └─ N bytes：只复制 [0, N)，再提交 send

send completion
  → sendOffset += transferredBytes
  ├─ 仍有剩余：清零 OVERLAPPED 基类，刷新 WSABUF，重新 postSend
  └─ 全部完成：释放 send operation，再 postReceive
```

本阶段的 `OVERLAPPED::hEvent` 始终为 `nullptr`，完成通知来自 completion port，因此没有 event 可以交给 `WSAResetEvent()` 重置。

如果部分 send 或下一次 receive 同步投递失败，不会再有对应 completion。当前最小模型应关闭该连接，避免留下一个没有任何在途 operation 的停滞连接。

### 21.7 任务七：两个 worker 的最小退出

**练习**

假设：

- 有两个 worker。
- 不再提交新 I/O。
- 所有普通 completion 都已经处理并回收。
- 所有关联 socket 已关闭。

回答：

1. 需要投递几个 Stop packet？
2. 每个 packet 的 bytes、key 和 `overlapped` 应是什么？
3. `join()` 与 `CloseHandle(completionPort)` 谁先执行？
4. 为什么不能根据 worker 编号查找 connection？
5. 为什么 completion handler 的执行先后不能直接当作业务顺序？

**验收标准**

- [ ] 两个 worker 对应两个 Stop packet。
- [ ] 使用 `0`、`StopCompletionKey`、`nullptr`。
- [ ] 先让 worker 退出并 `join()`，最后关闭 port。
- [ ] 知道一个 packet 只被一个 worker 取出。
- [ ] 知道 worker 与连接没有固定绑定。
- [ ] 知道多个 worker 的 handler 可以并行执行。
- [ ] 不把本练习扩展成取消和生产级安全停机。

**参考答案与解释**

```text
PostQueuedCompletionStatus(port, 0, StopCompletionKey, nullptr)
PostQueuedCompletionStatus(port, 0, StopCompletionKey, nullptr)
  → 两个 worker 各取一个
  → 两个 worker 返回
  → join 两个 worker
  → CloseHandle(port)
```

worker 从共享 port 竞争下一个 packet，connection 来自 operation，不来自线程编号。packet 被取出的先后也不等于 handler 完成业务处理的先后。

### 21.8 最终综合验收

**练习**

闭卷复述下面完整链路：

```text
创建 port
  → 关联 connected socket
  → 创建 worker
  → 创建 receive operation
  → 转移所有权并 WSARecv
  → completion packet
  → GQCS
  → 结果矩阵
  → 恢复 unique_ptr
  → receive/send completion
  → 部分发送
  → 下一次 receive
  → 最小 worker 退出
```

然后只读项目以下函数，指出每一步对应位置：

- `postReceive()`。
- `postSend()`。
- `runCompletionWorker()`。
- `handleReceiveCompletion()`。
- `handleSendCompletion()`。

**验收标准**

- [ ] 能解释 Overlapped I/O 负责提交，IOCP 负责汇聚完成通知。
- [ ] 能解释 port、socket、worker 和 operation 的关系。
- [ ] 能解释 key 与 `OVERLAPPED*` 的粒度差异。
- [ ] 能解释立即完成为什么仍由 worker 回收。
- [ ] 能完整复述 GQCS 四种结果。
- [ ] 能说明成功和失败 completion 都要回收 operation。
- [ ] 能处理 receive 零字节和 send 部分完成。
- [ ] 能说明部分 send 或下一次 receive 同步投递失败后如何收尾。
- [ ] 能解释两个 worker 没有连接亲和性。
- [ ] 能解释 completion 处理顺序为什么不能直接作为业务顺序。
- [ ] 复述过程不依赖 `AcceptEx()`、发送队列、pending 计数、取消或安全停机知识。

**参考答案与解释**

完整复述至少应包含下面七层关系：

1. `CreateIoCompletionPort()` 先创建 port，再把 connected socket 关联到它；port 不拥有 socket。
2. `WSARecv()` 和 `WSASend()` 负责提交 operation，operation 自己拥有 `OVERLAPPED`、buffer 和 connection 强引用。
3. 返回 `0` 或 `WSA_IO_PENDING` 都表示提交成功，operation 交给 completion 路径；其他同步错误由提交函数恢复并释放所有权。
4. GQCS 通过返回值和 `overlapped` 是否为空区分成功 I/O、失败 I/O、人工 packet 与无 packet 错误。
5. 普通 packet 的 `OVERLAPPED*` 转回 `IoOperation*` 后立即恢复 `unique_ptr`；成功和失败 completion 都不能遗漏回收。
6. receive 使用实际完成字节范围；零字节结束连接；send 使用 `sendOffset` 处理所有部分完成，然后再提交下一次 receive。
7. 两个 worker 共享同一个 port且不绑定连接；普通 operation 全部回收后，为每个 worker 投递一个 Stop packet，`join()` 后再关闭 port。

全部任务通过后，阶段四才算完成。

---

## 22. 下一阶段衔接

当前连接建立方式仍然是同步 `accept()`：

```text
同步 accept
  → 得到 connected socket
  → 关联 completion port
  → postReceive
```

阶段五把连接建立本身也变成 Overlapped operation：

```text
预先创建 accept socket
  → AcceptEx
  → accept completion 进入同一个 port
  → 完成 accepted socket 初始化
  → 将 accepted socket 关联到 port
  → postReceive
```

阶段五将新增：

- 动态取得 `AcceptEx()` 函数指针。
- accept operation 与地址 buffer。
- `SO_UPDATE_ACCEPT_CONTEXT`。
- accepted socket 关联 completion port。
- 预投递多个 accept。

这些内容建立在本阶段已经掌握的 port、packet、worker 和 operation 所有权之上。

---

## 23. 官方资料

阅读时重点关注：每个参数的当前作用、失败后使用哪个错误函数、是否取到了 packet，以及 operation 由谁回收。

- [I/O Completion Ports](https://learn.microsoft.com/en-us/windows/win32/fileio/i-o-completion-ports)
- [CreateIoCompletionPort](https://learn.microsoft.com/en-us/windows/win32/api/ioapiset/nf-ioapiset-createiocompletionport)
- [GetQueuedCompletionStatus](https://learn.microsoft.com/en-us/windows/win32/api/ioapiset/nf-ioapiset-getqueuedcompletionstatus)
- [PostQueuedCompletionStatus](https://learn.microsoft.com/en-us/windows/win32/api/ioapiset/nf-ioapiset-postqueuedcompletionstatus)
- [CloseHandle](https://learn.microsoft.com/en-us/windows/win32/api/handleapi/nf-handleapi-closehandle)
- [WSARecv](https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-wsarecv)
- [WSASend](https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-wsasend)

进入阶段五前，必须能够准确回答：

> `WSARecv()` 返回后，为什么提交路径有时必须放弃 operation 所有权、有时又必须立即恢复？`GetQueuedCompletionStatus()` 返回 `FALSE` 时，如何判断是否仍有一个 `IoOperation` 必须回收？
