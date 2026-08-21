# IOCP 阶段九：取消、关闭与安全停机学习讲义

> 前置知识：阶段七的连接状态机与 closing winner（唯一关闭胜出者），阶段八的有界任务池、增量文件传输和业务任务生命周期。
> 贯穿项目：`D:\CodeRepository\claude\remote_control`。
> 学习范围：单连接关闭、异步 I/O 取消、业务任务停止、pending I/O 排空、completion worker 退出和 completion port 释放。
> 客户端线程模型与外围 Windows API 放在阶段十。

---

## 1. 阶段九学习主线

前八个阶段主要回答“服务端怎样持续接收和处理工作”。阶段九回答相反的问题：

> 当连接、任务和 I/O 可能同时仍在进行时，怎样保证系统停止后没有新工作进入，也没有旧对象被过早释放？

完整主线：

```text
单连接关闭：
closeConnection
  → 争夺 Closing
  → 清理等待发送和文件状态
  → 从 registry 移除
  → 阻止该 socket 上的新投递
  → CancelIoEx
  → shutdown
  → closesocket
  → 标记 Closed

整体停机：
stop
  → 原子地进入 stopping
  → 关闭 listening socket
  → 关闭 registry 中的活动连接
  → 停止三个业务任务池
  → 停止 timeout monitor
  → completion worker 继续消费取消结果
  → 等待 pending I/O 归零
  → 每个 completion worker 一个退出 packet
  → join completion workers
  → CloseHandle(completionPort)
```

建议分五个学习单元推进：

1. **建立停机心智模型（第 4～6 节）**
   - 解决的问题：逻辑状态、socket handle、operation 和业务任务为什么不会同时结束。
   - 学完自检：能解释“已关闭 socket”与“已回收 operation”的区别。
2. **掌握单连接关闭（第 7～15 节）**
   - 解决的问题：closing winner 怎样按顺序清理一条连接。
   - 学完自检：能逐项解释 `CancelIoEx()`、`shutdown()` 和 `closesocket()` 的职责。
3. **建立 pending I/O 账本（第 16～20 节）**
   - 解决的问题：怎样保证每次成功投递最终都由一次 completion 对账。
   - 学完自检：能推演立即完成、pending、同步失败和取消完成四条路径。
4. **完成整体安全停机（第 21～27 节）**
   - 解决的问题：任务池、timeout thread、completion worker 和 port 应按什么顺序退出。
   - 学完自检：能说明为什么 pending 归零必须早于 worker 退出和 port 关闭。
5. **竞态推演与综合验收（第 28～33 节）**
   - 解决的问题：怎样用不变量验证任意交错顺序。
   - 学完自检：能独立推演连接到达、I/O 投递、任务执行与 `stop()` 并发的结果。

---

## 2. 知识范围

### 2.1 本阶段核心内容

- `closeConnection()` 的一次性清理资格。
- `ConnectionPhase::Closing` 与 `ConnectionPhase::Closed` 的停机语义。
- `CancelIoEx()` 对 Overlapped I/O 的取消请求。
- `shutdown()` 与 `closesocket()` 的不同职责。
- 取消后仍会返回 completion 的规则。
- `IoOperation` 与 `ConnectionContext` 在取消期间的生命周期。
- `m_stopping` 对新 accept、receive、send 和业务任务的准入控制。
- `tryBeginOperation()` 与 `finishOperation()` 的全局 pending 账本。
- `condition_variable` 等待 pending I/O 归零。
- `TaskPool::stop()`、`CancelSynchronousIo()` 与 worker join。
- timeout monitor 的唤醒与 join。
- `PostQueuedCompletionStatus()` 投递 completion worker 退出 packet。
- completion port 的最终释放时机。
- 取得 completion port 后的启动失败、显式停止和 destructor 复用同一条收尾路径。

### 2.2 直接沿用的前置结论

以下内容已经在前面阶段建立，本阶段只使用结论：

1. `tryBeginClosing()` 的 CAS 细节沿用阶段七。
2. 单连接最多一个 receive 和一个 send 在途的规则沿用阶段七。
3. 发送队列 FIFO、背压和 final send 语义沿用阶段七。
4. `TaskPool::submit()`、`runWorker()` 和队列容量沿用阶段八。
5. 文件分批生产和屏幕帧合并沿用阶段八。
6. `GetQueuedCompletionStatus()` 的四种结果沿用阶段四。

本阶段不重新推导这些机制，而是研究它们在停止过程中怎样收敛。

### 2.3 本阶段不展开的内容

- 客户端 Qt worker 的完整退出顺序。
- 服务注册、托盘、剪贴板、输入注入等外围 Windows API。
- 进程崩溃恢复和跨进程 watchdog。
- 分布式服务的优雅下线协议。
- TLS close-notify 等加密协议收尾。

---

## 3. 学习完成标准

完成本阶段后，应能做到：

1. 区分逻辑关闭、native socket 关闭、operation 回收和 transport 完全停止。
2. 解释为什么 `CancelIoEx()` 只是发出取消请求，不负责等待完成。
3. 解释为什么取消后的 packet 仍必须由 completion worker 消费。
4. 说明 `IoOperation::connection` 怎样防止取消期间出现悬空 `ConnectionContext`。
5. 逐项解释 `closeConnection(_connection, _reason)` 的参数和执行顺序。
6. 说明 `socketMutex` 怎样消除“正在投递”与“正在关闭”之间的竞态。
7. 说明 `acceptMutex` 怎样协调 `AcceptEx()`、accept context 更新和 listening socket 关闭。
8. 说明 `m_stopping` 与 pending mutex 为什么必须共同控制新 I/O 准入。
9. 对四种投递结果正确增减 pending I/O 计数。
10. 解释 condition variable 为什么等待 predicate，而不是只等待一次通知。
11. 解释任务池停止为什么要拒绝新任务、清空等待任务、取消同步 I/O 并 join。
12. 说明 timeout monitor 为什么要先通知再 join。
13. 说明退出 packet 为什么必须在 pending I/O 归零后投递。
14. 说明一个 completion worker 为什么需要一个退出 packet。
15. 说明 completion port 为什么最后关闭。
16. 能推演 `postReceive()`、`closeConnection()` 和 `stop()` 的并发交错。
17. 能阅读 `TransportLifecycleTests.cpp` 并解释它实际覆盖的竞态。

建议投入 10～15 小时。

---

## 4. 安全停机不是“把对象析构掉”

同步程序中，资源释放常被想象成一条简单链：

```text
函数返回
  → 局部对象析构
  → handle 关闭
```

IOCP 服务端中，同一时刻可能同时存在：

```text
一个 completion worker 正在处理 receive completion
一个文件 worker 正在同步读取文件
一个 timeout thread 正准备关闭超时连接
一个线程正调用 WSASend
Windows 仍保存若干 OVERLAPPED* 地址
另一个线程开始执行 stop
```

因此不能从“某个 C++ 对象准备析构”直接跳到“所有线程和内核都不再使用它”。

安全停机必须解决四个问题：

1. **停止准入**：不允许新 accept、receive、send 和业务 task 继续进入。
2. **发出取消**：让已经阻塞或 pending 的工作尽快返回。
3. **消费结果**：接收所有已经登记的 completion，并回收 operation。
4. **最后释放基础设施**：join worker 后才能关闭 completion port，并让 transport 生命周期拥有者继续析构。

本文所说的“排空”，是指先禁止新工作进入，再让已经登记的工作逐一完成对账，直到对应计数归零。

错误顺序通常不会每次都失败。它更可能表现为：

- 偶发对象释放后仍被访问（use-after-free）。
- 偶发重复关闭。
- `stop()` 偶发永久等待。
- worker 偶发访问已经关闭的 port。
- 压力测试运行多次后才出现崩溃。

阶段九的目标不是记住一串 API，而是能证明停机顺序为什么成立。

---

## 5. 四种生命周期必须分开

### 5.1 连接逻辑状态

```text
活动 phase
  → Closing
  → Closed
```

它回答：

> 应用是否还允许这条连接产生新业务和新 I/O？

`Closing` 表示某个线程已经取得清理资格。`Closed` 表示该线程已经完成项目定义的连接清理步骤。

### 5.2 native socket handle

```text
有效 SOCKET
  → CancelIoEx / shutdown / closesocket
  → INVALID_SOCKET
```

它回答：

> 应用是否还能使用这个 handle 提交新的 socket 操作？

handle 关闭后，应用不能再使用它，但此前提交的 operation 仍可能等待 completion 交付。

### 5.3 `IoOperation` 对象

```text
unique_ptr 创建 operation
  → release 后把 OVERLAPPED* 交给 Windows
  → completion worker 收回指针
  → 恢复 unique_ptr
  → handler 返回后析构，或在运行期再次投递
```

它回答：

> Windows 保存的 `OVERLAPPED*` 和 buffer 何时可以释放？

答案不是“socket 关闭时”，而是“最终 completion 已经被取出并恢复所有权后”。

运行期间，send handler 可能把同一个 operation 再次投递；停机开始后，stopping/terminal gate 会拒绝续投，所以最终 completion 才会走向析构。

### 5.4 transport 与 worker 基础设施

```text
任务池线程
timeout thread
completion worker
completion port
transport Impl
```

它回答：

> 谁负责把剩余工作推进到可回收状态？

只要还有已登记的 Overlapped operation，就必须保留能够消费 completion 的 worker 和 port。

### 5.5 最容易混淆的结论

```text
ConnectionPhase::Closed
  ≠ 所有 IoOperation 已经析构
  ≠ pending I/O 已经归零
  ≠ completion worker 已经退出
  ≠ transport 已经停止
```

`Closed` 只描述单连接的逻辑与 handle 清理已经完成。全局排空发生在后续停机步骤中。

---

## 6. 安全停机依赖三个“闸门”

### 6.1 连接状态闸门

```cpp
if (_connection->state.isTerminal())
{
    return false;
}
```

作用：拒绝一条已进入 `Closing` 或 `Closed` 的连接继续产生工作。

它只控制单连接，不能阻止其他活动连接或新的 accept。

### 6.2 transport 停止闸门

```cpp
if (this->m_stopping.load())
{
    return false;
}
```

作用：服务级停止开始后，拒绝整个 transport 的新工作。

它覆盖：

- accept 补位。
- receive/send 投递。
- accepted socket 的注册准入；检查与插入还必须和 stop snapshot 协调，详见第 23.5 节。
- 文件、Shell 和屏幕任务入口。
- timeout monitor 的下一轮工作。

### 6.3 pending I/O 记账闸门

```cpp
if (!this->tryBeginOperation())
{
    return false;
}
```

作用：把“是否仍允许新 I/O”和“本次 I/O 是否已经记入排空账本”合并成一个原子业务步骤。

仅检查 `m_stopping` 再单独递增计数是不够的：

```text
线程 A：看到 stopping == false
线程 B：设置 stopping = true，看到 pending == 0
线程 B：准备退出 completion worker
线程 A：pending++ 并提交 WSARecv
```

项目使用同一把 `m_pendingIoOperationMutex` 串行化停止标志切换与 operation 登记，消除这个窗口。

---

## 7. 单连接关闭入口 `closeConnection()`

项目入口：

```cpp
void closeConnection(
    std::shared_ptr<ConnectionContext> const& _connection,
    ConnectionCloseReason _reason);
```

### 7.1 `_connection` 参数

- 类型：`std::shared_ptr<ConnectionContext> const&`。
- 作用：指定要关闭的连接上下文。
- 为什么使用 `shared_ptr`：当前关闭函数执行期间连接必须存活；其他在途 operation 也可能持有同一个上下文。
- 为什么按 const 引用传入：函数不转移调用者的 `shared_ptr`，只共享对象生命周期。
- 空指针：函数立即返回，不执行任何清理。

### 7.2 `_reason` 参数

- 类型：`ConnectionCloseReason`。
- 作用：记录第一个成功关闭者观察到的终止原因。
- 常见值：`RequestComplete`、`PeerDisconnected`、`IoFailure`、`IdleTimeout`、`Backpressure`、`ServerShutdown`。
- 默认值：声明中默认为 `InternalFailure`。
- 关键规则：只有 closing winner 可以写入最终 reason。

### 7.3 为什么函数必须幂等

同一连接可能同时被多个路径关闭：

```text
receive completion 失败
send completion 失败
idle timeout 到达
背压拒绝
业务请求完成
server stop
```

这些路径都调用同一个入口。幂等不是“重复执行全部清理也没关系”，而是：

> 多个线程都可以请求关闭，但只有一个线程执行清理，其余线程在状态竞争失败后返回。

---

## 8. closing winner 决定唯一清理者

关键代码：

```cpp
ConnectionPhase previousPhase{ConnectionPhase::AwaitingRequest};
if (!_connection->state.tryBeginClosing(&previousPhase))
{
    return;
}
_connection->closeReason.store(_reason);
```

### 8.1 `tryBeginClosing(_previousPhase)` 参数

阶段七已经推导 CAS，本节只回顾输出语义：

- `_previousPhase`：可为空的输出指针。
- winner：函数把关闭前的活动 phase 写入该地址，并返回 `true`。
- loser：返回 `false`，不能使用输出值继续释放 quota。

当前调用传入 `&previousPhase`，因为 registry 移除连接时需要知道它原来是否占用 `ScreenStream` 或 `ControlStream` quota。

### 8.2 为什么先争夺状态，再写 reason

假设 timeout 与 receive failure 同时发生：

```text
线程 A：原因 = IdleTimeout
线程 B：原因 = IoFailure
```

如果两者先写 reason 再竞争关闭，loser 可能覆盖 winner 的诊断结果。

正确顺序：

```text
先 CAS 争夺 Closing
  → winner 写 closeReason
  → loser 立即返回
```

### 8.3 `previousPhase` 的两个用途

1. 从 registry 移除时释放正确的角色 quota。
2. 关闭日志记录连接结束前的业务角色。

它不是“当前 phase”。CAS 成功后当前 phase 已经是 `Closing`。

---

## 9. 先释放等待中的应用状态

winner 首先处理还没有进入内核的应用级工作。

### 9.1 清空等待发送

```cpp
{
    std::lock_guard<std::mutex> const lock{_connection->sendMutex};
    _connection->sendQueue.clear();
    _connection->queuedSendBytes = 0;
}
```

这一步释放：

- `sendQueue` 中尚未成为 operation 的 waiting items。
- waiting items 对应的 retained byte 计数。

这一步不会释放：

- 当前已经提交的 send operation。
- 当前 operation 内部的 `sendBytes`。
- Windows 保存的 `OVERLAPPED*`。

当前 operation 只能由 completion worker 在最终 completion 到达后回收。

### 9.2 为什么必须持有 `sendMutex`

阶段七已经规定：

- `sendQueue`。
- `queuedSendBytes`。
- `sendPending`。
- `closeAfterSend`。

属于同一个发送临界区。

关闭路径虽然只修改其中两个字段，也必须取得同一把锁，避免与 producer 或 send completion 同时修改 queue 和计数。

### 9.3 清除增量文件状态

```cpp
{
    std::lock_guard<std::mutex> const lock{_connection->fileTransferMutex};
    _connection->fileTransfer.reset();
}
```

这一步可能释放：

- `FileTransferState`。
- 打开的 `std::ifstream`。
- `QDirIterator`。
- 尚未生产的文件传输进度。

如果某个文件 task 已经复制了 `shared_ptr<FileTransferState>` 到局部变量，该 task 仍可暂时保持状态存活。task 自己还必须检查 `m_stopping` 和 connection terminal state。

### 9.4 为什么先清应用状态，再关 socket

关闭状态已经是 terminal，后续 producer 会被拒绝。尽早清空等待工作可以：

1. 释放不再需要的内存和文件资源。
2. 防止关闭过程继续保留大队列。
3. 让后续 completion 只做失败收尾，不再交接 waiting item。

---

## 10. 从 `ConnectionRegistry` 移除

关键调用：

```cpp
this->m_connectionRegistry.remove(_connection, previousPhase);
```

### 10.1 `_connection` 参数

- 作用：指定要从 registry map 删除的连接。
- registry 会验证 map 中的对象与传入对象是同一个 `shared_ptr` 目标。
- 删除 map entry 只减少一个强引用，不保证对象立即析构。

### 10.2 `_previousPhase` 参数

- 作用：指出连接进入 `Closing` 前的业务角色。
- 如果是 `ScreenStream`：释放一个 screen quota。
- 如果是 `ControlStream`：释放一个 control quota。
- 其他 phase：不修改这两个 stream counter。

### 10.3 为什么在 socket 设为 `INVALID_SOCKET` 前移除

registry 使用 socket 值作为 map key：

```cpp
auto const iterator{
    this->m_connectionsBySocket.find(_connection->socket)};
```

如果先执行：

```cpp
_connection->socket = INVALID_SOCKET;
```

registry 将无法用原 key 找到 entry，连接和 quota 可能残留。

因此项目顺序是：

```text
保存 previousPhase
  → registry.remove(connection, previousPhase)
  → 关闭 native socket
  → socket = INVALID_SOCKET
```

### 10.4 registry 移除不等于对象析构

取消期间至少可能还有三种强引用：

1. `stop()` 获取的 connection snapshot。
2. 当前正在执行的 handler 或 task 局部变量。
3. receive/send `IoOperation::connection`。

因此从 registry 移除后，completion handler 仍能安全读取 connection state，并发现它已经 terminal。

---

## 11. `socketMutex` 消除投递与关闭竞态

关闭 socket 的关键临界区：

```cpp
{
    std::lock_guard<std::mutex> const lock{_connection->socketMutex};
    if (_connection->socket != INVALID_SOCKET)
    {
        CancelIoEx(reinterpret_cast<HANDLE>(_connection->socket), nullptr);
        shutdown(_connection->socket, SD_BOTH);
        closesocket(_connection->socket);
        _connection->socket = INVALID_SOCKET;
    }
}
```

`postReceive()` 和 `postSend()` 也在同一把 `socketMutex` 下完成：

```text
检查 stopping / terminal / socket
  → 登记 pending operation
  → 调用 WSARecv 或 WSASend
  → 处理同步投递失败
```

### 11.1 竞态 A：投递先取得锁

```text
postReceive 取得 socketMutex
  → tryBeginOperation 成功，pending++
  → WSARecv 成功提交
  → 释放 socketMutex
closeConnection 随后取得 socketMutex
  → CancelIoEx 取消刚才的 operation
```

结果：operation 已经正确记账，取消 completion 会把它归还。

### 11.2 竞态 B：关闭先取得锁

```text
closeConnection 取得 socketMutex
  → 取消并关闭 socket
  → socket = INVALID_SOCKET
  → 释放 socketMutex
postReceive 随后取得 socketMutex
  → 发现 terminal 或 INVALID_SOCKET
  → 不登记、不提交
```

结果：不会在已关闭 handle 上启动新 I/O。

### 11.3 为什么只用 atomic state 不够

下面的顺序仍可能发生：

```text
线程 A 检查 state 仍活动
线程 B 把 state 改为 Closing 并关闭 socket
线程 A 调用 WSARecv
```

atomic state 只能安全保存 phase，不能让“检查 socket 条件”和“提交 native I/O”成为一个不可分割的临界区。

`socketMutex` 才是 socket handle 使用权的最终串行化边界。

Winsock 也明确要求不能在同一个 socket 上并发执行 `closesocket()` 与其他 Winsock 调用。项目用 `socketMutex` 把 `WSARecv()`、`WSASend()` 和关闭路径排成互斥顺序。

---

## 12. `CancelIoEx()`：请求取消 Overlapped I/O

函数原型：

```cpp
BOOL CancelIoEx(
    HANDLE hFile,
    LPOVERLAPPED lpOverlapped);
```

### 12.1 `hFile` 参数

- 作用：指定包含目标 I/O 的 handle。
- 项目值：`reinterpret_cast<HANDLE>(_connection->socket)` 或 listening socket 的 handle 表示。
- 类型转换：只适配 Win32 API 形参类型，不改变 socket 所有权。
- 要求：调用时 handle 必须仍然有效。

### 12.2 `lpOverlapped` 参数

- 传具体 `OVERLAPPED*`：只请求取消与该 operation 对应的 I/O。
- 传 `nullptr`：请求取消当前进程在该 handle 上尚未完成的全部 I/O，不限制最初由哪个线程提交。
- 项目值：`nullptr`，因为关闭连接时不再保留该 socket 上的任何 operation。

### 12.3 返回值

- 非零：取消请求调用成功。
- `FALSE`：调用失败，使用 `GetLastError()` 取得 Win32 错误。
- 没有可取消请求时可能得到 `ERROR_NOT_FOUND`。

项目关闭路径不依赖返回值决定 operation 生命周期。无论取消是否抢在自然完成前成功，已经登记的 operation 都必须等待其最终 completion。

### 12.4 `CancelIoEx()` 不保证什么

它不保证：

1. 函数返回时 operation 已经停止。
2. completion 已经进入 port。
3. completion worker 已经处理 packet。
4. `IoOperation` 已经可以释放。
5. 所有 I/O 都一定以“取消”结束；某个 I/O 可能已经自然完成。

可以把它理解为：

```text
应用向内核发出取消请求
  → 内核决定 operation 最终结果
  → completion port 仍交付最终 packet
  → worker 回收 operation
```

### 12.5 取消请求后的三种最终状态

即使 `CancelIoEx()` 成功返回，operation 最终也可能是：

1. **正常完成**：取消请求到达得太晚，I/O 已经自然完成。
2. **被取消**：completion error 通常为 `ERROR_OPERATION_ABORTED`。
3. **以其他错误失败**：completion 携带对应错误。

因此项目不根据 `CancelIoEx()` 的返回值直接释放 operation，而是统一等待 completion worker 取得最终状态。

---

## 13. `shutdown()`：关闭 socket 的通信方向

函数原型：

```cpp
int shutdown(
    SOCKET s,
    int how);
```

### 13.1 `s` 参数

- 作用：指定要停止通信的 connected socket。
- 项目值：`_connection->socket`。
- listening socket 不使用这里的 connected-stream 收尾；项目直接取消 accept 并 `closesocket()`。

### 13.2 `how` 参数

Winsock 常用值：

- `SD_RECEIVE`：停止接收方向。
- `SD_SEND`：停止发送方向。
- `SD_BOTH`：同时停止接收和发送方向。

项目传入 `SD_BOTH`，因为连接已经进入 terminal state，不再保留任何通信方向。

### 13.3 返回值

- `0`：调用成功。
- `SOCKET_ERROR`：调用失败，使用 `WSAGetLastError()` 取得 Winsock 错误。

### 13.4 `shutdown()` 不释放 socket handle

`shutdown()` 改变通信能力，但不会释放 socket 资源。

因此不能只调用：

```cpp
shutdown(socketHandle, SD_BOTH);
```

还必须执行：

```cpp
closesocket(socketHandle);
```

本项目不是在实现“发送完剩余数据后的协议级优雅半关闭”。连接已经由 closing winner 判定终止，因此这里的目标是阻止继续通信并进入资源回收。

---

## 14. `closesocket()`：释放应用持有的 socket handle

函数原型：

```cpp
int closesocket(SOCKET s);
```

### 14.1 `s` 参数

- 作用：指定要关闭的 socket handle。
- 项目值：connection socket 或 listening socket。
- 调用后：应用不能再使用原 handle 提交 I/O。

### 14.2 返回值

- `0`：关闭调用成功。
- `SOCKET_ERROR`：关闭失败，使用 `WSAGetLastError()`。

关闭路径通常已经无法恢复这条连接，因此项目继续把成员设为无效值，确保应用不再重复使用 handle。

### 14.3 为什么随后设置 `INVALID_SOCKET`

```cpp
closesocket(_connection->socket);
_connection->socket = INVALID_SOCKET;
```

作用：

1. 明确应用层不再拥有有效 handle。
2. 后续重复关闭请求可以安全跳过 native close。
3. 后续投递入口会在检查阶段拒绝。

Windows 可能在 `closesocket()` 调用后很快复用同一个数值作为其他 socket descriptor。把成员立即改成 `INVALID_SOCKET`，并用 `socketMutex` 禁止并发 Winsock 调用，可以避免把旧数值误认为仍属于原连接。

### 14.4 `CloseHandle()` 不能替代 `closesocket()`

- completion port 使用 `CloseHandle()`。
- socket 使用 `closesocket()`。

两者资源类型不同，不能因为 `CancelIoEx()` 接收 `HANDLE` 就把 socket 的关闭函数也改成 `CloseHandle()`。

### 14.5 `closesocket()` 也会取消在途 socket I/O

关闭 overlapped socket 时，仍 pending 的 `WSARecv()` 和 `WSASend()` 也会被取消，并继续触发对应的 completion port 动作。

同样要注意：`closesocket()` 返回不表示这些 completion 已经全部交付。`WSAOVERLAPPED`、buffer 和 operation 仍必须存活到最终 completion 被消费。

---

## 15. 三个 API 的职责不能合并

项目顺序：

```cpp
CancelIoEx(reinterpret_cast<HANDLE>(_connection->socket), nullptr);
shutdown(_connection->socket, SD_BOTH);
closesocket(_connection->socket);
```

分别回答三个问题：

1. **`CancelIoEx()`**
   - 问题：已经提交但尚未最终完成的 Overlapped I/O 怎么办？
   - 动作：请求取消。
2. **`shutdown()`**
   - 问题：connected socket 是否还允许继续收发？
   - 动作：关闭两个通信方向。
3. **`closesocket()`**
   - 问题：应用持有的 socket handle 何时释放？
   - 动作：关闭 handle。

### 15.1 既然 close 也会取消，为什么仍先调用 `CancelIoEx()`

`closesocket()` 本身确实会启动 pending socket I/O 的取消。项目仍显式调用 `CancelIoEx()`，是为了在释放 handle 前清楚表达“先请求取消该 handle 上全部 Overlapped I/O”的意图。

无论取消由哪一个调用触发，生命周期结论都相同：

```text
API 返回
  ≠ operation completion 已被消费
```

真正决定何时可释放 operation 的仍是 completion 与 pending 账本，不是某个取消 API 的同步返回。

调用全部完成后，closing winner 执行：

```cpp
_connection->state.markClosed();
```

`markClosed()` 没有参数。它只尝试完成 `Closing → Closed`，不等待 pending operation，也不释放 completion port。

---

## 16. 取消后为什么仍会收到 completion

Overlapped I/O 一旦成功提交，Windows 已经记住：

- socket handle。
- `OVERLAPPED*`。
- buffer 描述。
- completion port 关联。

关闭路径不能直接撤销这些地址关系并释放内存。

下面是一种可能的取消时间线：

```text
postReceive
  → pending count +1
  → Windows 保存 IoOperation 中的 OVERLAPPED*
  → connection 开始关闭
  → CancelIoEx
  → closesocket
  → connection 标记 Closed
  → Windows 产生失败 completion
  → GQCS 返回 FALSE，但 overlapped 非空
  → finishOperation，pending count -1
  → 恢复 unique_ptr<IoOperation>
  → receive handler 发现失败或 terminal
  → closeConnection 再次调用，但成为 loser
  → handler 返回，IoOperation 析构
```

这不是线程间的固定执行顺序。取消 completion 也可能在 `closeConnection()` 尚未执行到 `markClosed()` 时到达；只要连接已经进入 `Closing`，handler 的再次关闭请求就会成为 loser。

### 16.1 `FALSE + overlapped != nullptr` 的含义

阶段四已经学习：

```text
GetQueuedCompletionStatus 返回 FALSE
  + overlapped 非空
  = 已经取到一个失败 I/O completion
```

取消就是这类失败 completion 的常见来源之一。

不能把所有 `FALSE` 都当成“worker 应立即退出”。

### 16.2 operation 怎样保持 connection 存活

receive/send operation 内部保存：

```cpp
std::shared_ptr<ConnectionContext> connection;
```

因此即使：

```text
registry 已移除 connection
  + stop() 的 snapshot 已离开当前循环项
```

只要 canceled operation 尚未被 completion worker 回收，connection context 仍然存活。

### 16.3 Windows 不拥有 C++ 对象

Windows 只保存传入的地址，不理解：

- `std::unique_ptr`。
- `std::shared_ptr`。
- `QByteArray`。
- `ConnectionContext`。

让这些对象活到 completion 的责任仍属于应用。

---

## 17. 三种取消 completion 的收尾

### 17.1 canceled accept

completion worker 先执行全局对账：

```cpp
this->finishOperation();
```

然后 accept handler：

```cpp
this->m_pendingAcceptOperationCount.fetch_sub(1);
if (!_success || this->m_stopping.load())
{
    closesocket(acceptedSocket);
    return;
}
```

结果：

1. 全局 pending I/O 计数减少。
2. accept depth 专用计数减少。
3. 不再补充 accept slot。
4. operation 中尚未转交的 accepted socket 被关闭。

### 17.2 canceled receive

receive handler 入口：

```cpp
if (!_success || _transferredBytes == 0 ||
    connection->state.isTerminal())
{
    this->closeConnection(
        connection,
        _success && _transferredBytes == 0
            ? ConnectionCloseReason::PeerDisconnected
            : ConnectionCloseReason::IoFailure);
    return;
}
```

连接通常已经由 server stop 的 closing winner 标记为 `Closed`。再次调用 `closeConnection()` 时，`tryBeginClosing()` 返回 `false`，不会重复清队列、移除 registry 或关闭 socket。

### 17.3 canceled send

send handler 使用同一类 completion gate：

```cpp
if (!_success || _transferredBytes == 0 ||
    connection->state.isTerminal())
{
    this->closeConnection(
        connection,
        _success && _transferredBytes == 0
            ? ConnectionCloseReason::PeerDisconnected
            : ConnectionCloseReason::IoFailure);
    return;
}
```

因为 terminal 分支在 offset、queue 和下一项交接之前，所以 canceled send 不会：

- 继续部分发送。
- 取出下一条 waiting item。
- 触发下一批文件生产。
- 触发下一帧屏幕任务。

### 17.4 关闭 reason 为什么不会被改成 `IoFailure`

server stop winner 已经写入：

```text
closeReason = ServerShutdown
```

取消 completion 后的 handler 虽然以 `IoFailure` 再次请求关闭，但它是 loser，不能覆盖 reason。

---

## 18. pending I/O 是一套严格账本

项目全局计数：

```cpp
std::atomic_int m_pendingIoOperationCount{0};
```

它统计所有已经登记、但 completion 路径尚未最终对账的 operation：

- `AcceptEx()`。
- `WSARecv()`。
- `WSASend()`。

### 18.1 账本规则

每个 operation 必须满足：

```text
成功取得投递资格
  → 恰好 +1

同步投递失败
  → 当前提交路径恰好 -1

成功提交
  → completion worker 取到最终 packet 后恰好 -1
```

### 18.2 “立即完成”仍不在提交路径减一

在本项目当前 completion 配置中，即使 `WSARecv()`、`WSASend()` 或 `AcceptEx()` 立即成功，仍会通过 IOCP 接收 completion packet。

因此：

```text
立即成功
  → count 保持 +1
  → worker 取得 completion
  → finishOperation()
```

不能因为 API 已立即完成就在提交路径 `finishOperation()`，否则 packet 到达时会再次减一。

### 18.3 pending accept count 不是全局排空计数

项目还有：

```cpp
std::atomic_int m_pendingAcceptOperationCount{0};
```

它用于：

- 维持 configured accept depth。
- 启动后检查预投递数量。
- accept completion 后决定是否补位。

整体停机等待的是 `m_pendingIoOperationCount`，因为它同时覆盖 accept、receive 和 send。

### 18.4 计数过高与过低的后果

**漏减一次：**

```text
真实 operation 已全部结束
  → pending 仍大于 0
  → stop() 永久等待
```

**多减一次：**

```text
pending 提前变成 0
  → stop() 退出 completion worker
  → 仍有 packet 尚未消费
  → operation 泄漏或悬空访问
```

因此 pending count 不是统计信息，而是决定资源释放时机的正确性协议。

---

## 19. `tryBeginOperation()`：登记新 I/O

项目实现：

```cpp
bool RemoteControlTransport::Impl::tryBeginOperation() noexcept
{
    std::lock_guard<std::mutex> const lock{
        this->m_pendingIoOperationMutex};
    if (this->m_stopping.load())
    {
        return false;
    }
    this->m_pendingIoOperationCount.fetch_add(1);
    return true;
}
```

### 19.1 参数与返回值

`tryBeginOperation()` 没有参数。

返回值：

- `true`：transport 仍接受新 I/O，并且 pending count 已经增加一次。
- `false`：停机已经开始，调用方不能提交 operation，也不能再减少计数。

`m_pendingIoOperationCount.fetch_add(1)` 的参数 `1` 表示原子地增加一。函数会返回增加前的旧值，但当前路径只需要完成登记，不使用该返回值。

### 19.2 为什么检查和递增必须在同一把 mutex 下

要保证的业务原子性：

```text
要么：stop 先进入 stopping，当前 operation 不登记
要么：operation 先完成登记，stop 将来必须等待它对账
```

不能出现：

```text
operation 已经提交
  + stop 没有在 pending count 中看到它
```

### 19.3 `std::atomic_bool::exchange()` 为什么仍需要 mutex

`m_stopping` 本身是 atomic，能防止 data race。但本阶段需要保护的不是单个 bool，而是这个跨字段不变量：

```text
stopping == true
  → 从此以后 pending count 不再增加
```

atomic 只能保证一次 load/store/exchange 原子，不能自动把另一个 atomic counter 的检查和修改组合成事务。

### 19.4 投递函数怎样使用它

receive 路径：

```cpp
if (!this->tryBeginOperation())
{
    return false;
}

IoOperation* const operationPointer{operation.release()};
int const result{WSARecv(..., operationPointer, nullptr)};
```

调用成功返回 `true` 后，当前路径就欠全局账本一次 `finishOperation()`。

### 19.5 同步失败怎样归还登记

```cpp
if (result == SOCKET_ERROR &&
    WSAGetLastError() != WSA_IO_PENDING)
{
    operation.reset(operationPointer);
    this->finishOperation();
}
```

同步失败表示 operation 没有进入未来 completion 路径，所以提交线程必须：

1. 恢复 `unique_ptr`。
2. 调用 `finishOperation()`。
3. 处理连接错误。

---

## 20. `finishOperation()`：完成一次对账

项目实现：

```cpp
void RemoteControlTransport::Impl::finishOperation() noexcept
{
    if (this->m_pendingIoOperationCount.fetch_sub(1) == 1)
    {
        std::lock_guard<std::mutex> const lock{
            this->m_pendingIoOperationMutex};
        this->m_pendingIoOperationCondition.notify_all();
    }
}
```

### 20.1 参数与返回值

`finishOperation()` 没有参数，也没有返回值。

调用前提：当前路径确实拥有一笔尚未归还的 operation 登记。

### 20.2 `fetch_sub(1)` 返回什么

`fetch_sub(1)` 的参数 `1` 表示原子地减一。

返回值是减一之前的旧值：

```text
旧值 5 → 新值 4 → 返回 5
旧值 1 → 新值 0 → 返回 1
```

因此条件：

```cpp
fetch_sub(1) == 1
```

只在计数发生 `1 → 0` 转换时成立。

### 20.3 为什么只在归零时通知

`stop()` 等待的 predicate 是：

```text
pending count == 0
```

从 8 变成 7 或从 2 变成 1 都不能让停机继续。只有最后一笔 operation 对账后才需要唤醒等待者。

### 20.4 `notify_all()` 参数与作用

`notify_all()` 没有参数。

作用：唤醒当前等待该 condition variable 的所有线程，让它们重新取得 mutex 并检查 predicate。

项目通常只有一个 `stop()` winner 在等待，但使用 `notify_all()` 可以直接表达“归零条件已经改变”。

### 20.5 为什么归零通知前还要取得 pending mutex

考虑等待线程：

```text
持有 pending mutex
  → 检查 count 是否为 0
  → 如果不是，准备释放 mutex 并睡眠
```

如果归零线程完全不与这把 mutex 同步，通知可能恰好落在“检查完成但尚未真正睡眠”的窗口中。

项目在 `1 → 0` 后取得同一把 mutex，再通知：

```text
等待线程要么仍持锁，归零线程等它进入 wait
要么等待线程已进入 wait，归零线程可以通知
```

从而避免停机线程错过最后一次唤醒。

### 20.6 completion worker 在何时调用

```cpp
if (overlapped)
{
    this->finishOperation();
    auto operation{std::unique_ptr<IoOperation>{
        static_cast<IoOperation*>(overlapped)}};
}
```

先对账，再进入 handler 的原因：

1. packet 已经从 completion port 取出，内核等待已经结束。
2. operation 所有权已经回到 worker 路径。
3. handler 只负责业务收尾或续投；如果续投成功，会重新登记一笔新 operation。
4. 停机期间 `m_stopping == true`，续投会被拒绝，因此计数最终单调收敛到零。

### 20.7 pending 归零不等于 handler 已经返回

worker 在取出 packet 后先调用 `finishOperation()`，随后才恢复 `unique_ptr` 并执行 handler。

因此最后一笔计数归零时，某个 completion worker 仍可能正在处理已经取出的 operation：

```text
packet 已从 port 取出
  → pending 归零
  → handler 仍持有 IoOperation
  → handler 返回后 operation 才析构
```

这不会破坏停机顺序，因为 `stop()` 只在 pending 归零后投递退出 packet，随后还会 join 所有 completion workers，最后才关闭 port 和离开停机 winner 调用。

---

## 21. `stop()` 首先建立全局停止事实

关键入口：

```cpp
void RemoteControlTransport::Impl::stop()
{
    {
        std::lock_guard<std::mutex> const lock{
            this->m_pendingIoOperationMutex};
        if (this->m_stopping.exchange(true))
        {
            return;
        }
    }
}
```

### 21.1 `stop()` 参数与返回值

`stop()` 没有参数，也没有返回值。

第一次把 `m_stopping` 从 false 改成 true 的调用是同步停止 winner。该调用返回前，项目会完成任务池 join、timeout thread join、completion worker join 和 port 关闭。

已经观察到 `m_stopping == true` 的重复调用会立即返回，不会等待 winner 完成后续步骤。

### 21.2 `exchange(true)` 的参数

- 参数 `true`：把 `m_stopping` 设置为 true。
- 返回值：修改前的旧值。

两种结果：

```text
旧值 false
  → 当前线程第一次开始停机
  → exchange 返回 false
  → 继续执行

旧值 true
  → 其他调用已经开始或完成停机
  → exchange 返回 true
  → 当前调用立即返回
```

### 21.3 为什么 exchange 放在 pending mutex 下

这一步与 `tryBeginOperation()` 使用同一把 mutex，形成停机分界线：

```text
分界线之前登记成功的 operation
  → 必须出现在 pending count 中

分界线之后尝试登记的 operation
  → 必须被拒绝
```

### 21.4 为什么 `stop()` 可以重复调用

项目中至少有三类调用者：

1. 显式调用 `RemoteControlTransport::stop()`。
2. `Impl` destructor。
3. `start()` 已取得 completion port 后，后续初始化失败触发的清理路径。

只有第一次调用执行收尾，后续调用读取到旧值 true 后返回。

### 21.5 同一个 `Impl` 不支持停止后重启

`start()` 会拒绝：

```cpp
this->m_stopping.load() == true
```

因此停止是当前 transport instance 的单向生命周期转换：

```text
未启动 / 启动中
  → 运行
  → stopping
  → stopped
```

需要再次运行时，应创建新的 transport instance。

### 21.6 重复 `stop()` 不是停机完成屏障

如果两个外部线程同时调用 `stop()`：

```text
线程 A：exchange 得到 false，成为停机 winner，并继续执行
线程 B：exchange 得到 true，立即返回
```

线程 B 的返回不能证明线程 A 已经完成 join 和 port close。

因此应由单一生命周期拥有者串行管理 transport：一个拥有者线程发起并等待首次 stop，其他线程只向它请求停止，不能把重复调用的返回当作“服务已经完全停止”。

---

## 22. 先关闭 listening socket

进入 stopping 后，项目先公开“已不再监听”：

```cpp
this->m_listeningPort.store(0);
this->m_idleTimeoutCondition.notify_all();
```

然后在 `m_acceptMutex` 下取消和关闭 listening socket：

```cpp
{
    std::lock_guard<std::mutex> const lock{this->m_acceptMutex};
    if (this->m_listenSocket != INVALID_SOCKET)
    {
        CancelIoEx(
            reinterpret_cast<HANDLE>(this->m_listenSocket),
            nullptr);
        closesocket(this->m_listenSocket);
        this->m_listenSocket = INVALID_SOCKET;
    }
}
```

### 22.1 为什么先停止新连接入口

如果先关闭当前连接，却继续接受新连接：

```text
stop 正在遍历旧 connection snapshot
  → 新 accept completion 注册一个新 connection
  → 它不在旧 snapshot 中
  → 停机可能漏掉该连接
```

因此必须先让：

- `m_stopping == true`。
- accept 补位停止。
- listening socket 取消并关闭。

然后再 snapshot 活动连接。这是阻止新连接进入的必要步骤，但还不是完整证明：已经完成 accept、尚未注册到 registry 的 socket 仍需要第 23.5 节所述的同步边界或失败回滚。

### 22.2 `m_acceptMutex` 保护哪些竞争

它串行化：

1. `replenishAccepts()` 提交新的 `AcceptEx()`。
2. accept completion 使用 listening socket 执行 `SO_UPDATE_ACCEPT_CONTEXT`。
3. `stop()` 取消并关闭 listening socket。

### 22.3 正在提交 `AcceptEx()` 时 stop 到达

如果 accept 路径先持有 mutex：

```text
accept 完成登记和提交
  → 释放 acceptMutex
  → stop 取得 acceptMutex
  → CancelIoEx 取消已登记 accept
```

如果 stop 先建立 stopping：

```text
postAccept 的 tryBeginOperation 失败
或 replenishAccepts 的 while 条件失败
  → 不再提交
```

### 22.4 context 更新与 listener 关闭

`SO_UPDATE_ACCEPT_CONTEXT` 需要仍有效的 listening socket 值。

handler 在同一把 `m_acceptMutex` 下再次检查：

```text
stopping == false
  + listenSocket != INVALID_SOCKET
```

所以它要么在 stop 关闭 listener 前完成 context 更新，要么在 stop 后放弃初始化并关闭 accepted socket。

这里证明的只是 listening socket 与 `SO_UPDATE_ACCEPT_CONTEXT` 不会并发使用；后续 `registry.add()` 是否会越过 stop snapshot，需要单独验证。

---

## 23. 用 snapshot 关闭所有活动连接

项目代码：

```cpp
std::vector<std::shared_ptr<ConnectionContext>> const connections{
    this->m_connectionRegistry.snapshot()};

for (std::shared_ptr<ConnectionContext> const& connection : connections)
{
    this->closeConnection(
        connection,
        ConnectionCloseReason::ServerShutdown);
}
```

### 23.1 `snapshot()` 参数与返回值

`snapshot()` 没有参数。

返回值：

```cpp
std::vector<std::shared_ptr<ConnectionContext>>
```

每个元素都是一个稳定强引用。registry mutex 只在复制 map 内容时持有，函数返回后释放。

### 23.2 为什么不持有 registry mutex 逐个关闭

`closeConnection()` 内部会调用：

```cpp
m_connectionRegistry.remove(...)
```

如果 snapshot 之后仍持有 registry mutex，再调用需要同一把 mutex 的 `remove()`，会造成自锁或复杂锁顺序。

正确结构：

```text
短时间持有 registry mutex
  → 复制 shared_ptr
  → 释放 registry mutex
  → 在锁外逐个 closeConnection
```

### 23.3 snapshot 期间连接已经被其他线程关闭

可能出现：

```text
snapshot 包含 connection A
  → timeout thread 先关闭 A
  → stop 随后 closeConnection(A, ServerShutdown)
```

第二次调用在 `tryBeginClosing()` 处成为 loser，安全返回。

### 23.4 为什么统一使用 `ServerShutdown`

stop 只对它真正赢得的连接写入 `ServerShutdown`。

已经由其他原因进入 `Closing` 的连接保留原 reason，例如：

- `RequestComplete`。
- `PeerDisconnected`。
- `IdleTimeout`。
- `Backpressure`。

这保证诊断结果反映第一个终止决定，而不是最后一个到达的线程。

### 23.5 代码审查重点：accepted socket 注册与 snapshot 的边界

stop 的目标是：建立 stopping 之后，不再有新 connection 出现在 snapshot 之外。

accepted socket 注册路径包含：

```cpp
std::shared_ptr<ConnectionContext> const connection{
    this->m_stopping.load()
        ? std::shared_ptr<ConnectionContext>{}
        : this->m_connectionRegistry.add(acceptedSocket)};
```

`m_stopping.load()` 的原子读取只能保证这一次读取没有数据竞争（data race），不能把“检查 stopping”“registry.add”和 stop 的 `snapshot()` 自动合并为一个事务。第 29.7 节将具体推演这个交错；这里先记住需要证明的边界。

能够证明安全的设计至少需要满足一种：

1. 注册前检查与插入操作和 stop snapshot 使用同一个生命周期 mutex。
2. stop 在持有同一个注册同步边界时完成 snapshot。
3. add 后再次发现 stopping 或首次 `postReceive()` 失败时，立即调用统一关闭入口。

当前源码阅读时，不能只凭一次 `m_stopping.load()` 就断言“snapshot 后绝不会注册新连接”。

后续停机收敛过程以这项注册同步或回滚条件已经满足为前提；检查当前源码时，仍需把它保留为独立审查项。

---

## 24. 停止三个业务任务池

项目顺序：

```cpp
this->m_screenCaptureTaskPool.stop();
this->m_fileTaskPool.stop();
this->m_shellCommandTaskPool.stop();
```

关闭所有活动连接后，每条 connection 已经 terminal。此时停止任务池，等待业务 worker 不再访问 transport。

### 24.1 `TaskPool::stop()` 参数与返回值

`TaskPool::stop()` 没有参数，也没有返回值。

对首次调用者而言，它是幂等的同步入口：

```text
设置 pool stopping
  → 清空尚未开始的 waiting tasks
  → 唤醒 worker
  → 请求取消 worker 的同步 I/O
  → join 全部 worker
  → 清空 thread 容器
```

如果两个线程并发调用同一个 task pool 的 `stop()`，观察到 `m_stopping == true` 的调用仍会立即返回，不会替首次调用者等待 join。项目依赖 transport 的停机 winner 串行停止三个 task pool。

### 24.2 停止准入并清空等待任务

```cpp
{
    std::lock_guard<std::mutex> const lock{this->m_mutex};
    if (this->m_stopping)
    {
        return;
    }
    this->m_stopping = true;
    this->m_tasks.clear();
}
this->m_condition.notify_all();
```

效果：

1. 新 `submit()` 返回 `false`。
2. 尚在 deque 中的 tasks 被销毁。
3. 正在等待的 worker 被唤醒并退出。
4. 已经被 worker 取出的 task 不会由 `clear()` 撤回。

### 24.3 已经开始的 task 怎么办

运行中的 task 可能：

- 正在读文件。
- 正在枚举目录。
- 正在截图和编码。
- 正在执行 host Shell 操作。

`TaskPool::stop()` 不能销毁其栈帧。项目采用三层收敛：

1. 全局 `m_stopping` 让 task 在入口或循环中主动返回。
2. connection terminal state 让 task 放弃发送结果。
3. `CancelSynchronousIo()` 尝试打断阻塞的同步 I/O。

### 24.4 `CancelSynchronousIo()` 原型

```cpp
BOOL CancelSynchronousIo(HANDLE hThread);
```

#### `hThread` 参数

- 作用：指定发起同步 I/O 的目标线程。
- 项目值：`thread.native_handle()`。
- 当前目标：任务池中的业务 worker thread。
- 它不是文件 handle，也不是 completion port handle。

`thread.native_handle()` 没有参数。在 MSVC/Windows 环境中，它返回底层 thread handle，供 Windows API 使用；调用不会转移 `std::thread` 的所有权。

`CancelSynchronousIo()` 要求该 handle 具有 `THREAD_TERMINATE` 访问权限。项目使用仍处于 joinable 状态的 worker native handle。

#### 返回值

- 非零：取消请求调用成功。
- `FALSE`：调用失败，使用 `GetLastError()` 取得错误。
- 如果线程当前没有可取消的同步 I/O，失败不代表线程无法正常退出。

项目把它作为尽力而为的唤醒手段，忽略返回值，最终仍以 `join()` 为准。该函数本身也不等待同步 I/O 真正结束。

### 24.5 它与 `CancelIoEx()` 的区别

**`CancelIoEx()`：**

- 目标是 file/socket handle。
- 处理 Overlapped I/O。
- 取消结果通过 operation completion 收敛。

**`CancelSynchronousIo()`：**

- 目标是 thread handle。
- 尝试取消该线程发起的同步 I/O。
- 不会产生项目的 IOCP `IoOperation` completion。
- task 仍必须从调用栈返回，thread 才能 join。

### 24.6 `join()` 参数与作用

`std::thread::join()` 没有参数。

作用：阻塞当前 stop 调用，直到目标 worker thread 已经返回。

只有 join 完成后，才能证明：

- worker 不再执行捕获 `this` 的 task。
- worker 不再访问 task deque 和 condition variable。
- worker 不再调用 transport 的 enqueue 或 close 入口。

### 24.7 协作取消仍然必要

例如递归删除每轮检查：

```cpp
if (_stopping.load())
{
    return false;
}
```

并在遍历中继续检查。

`CancelSynchronousIo()` 不能替代这种检查，因为：

- 纯 CPU 工作不会被它取消。
- 某些库操作不一定处于可取消同步 I/O。
- task 在一次系统调用返回后仍可能继续下一轮业务逻辑。

---

## 25. 唤醒并 join timeout monitor

timeout monitor 使用可中断等待：

```cpp
bool const stopping{this->m_idleTimeoutCondition.wait_for(
    lock,
    std::chrono::seconds{1},
    [this] { return this->m_stopping.load(); })};
```

stop 在进入 stopping 后调用：

```cpp
this->m_idleTimeoutCondition.notify_all();
```

随后：

```cpp
if (this->m_idleTimeoutThread.joinable())
{
    this->m_idleTimeoutThread.join();
}
```

`joinable()` 没有参数；thread object 当前仍关联一个尚未 detach 或 join 的线程时返回 `true`。只有返回 `true` 时才能调用 `join()`。

### 25.1 `wait_for(lock, duration, predicate)` 的三个参数

1. **`lock`**
   - 类型：`std::unique_lock<std::mutex>`。
   - 作用：等待时临时释放 `m_idleTimeoutMutex`，返回前重新取得。
2. **`std::chrono::seconds{1}`**
   - 作用：指定本轮最长等待一秒。
   - 到期时即使没有通知，也会返回并执行 timeout 扫描。
3. **predicate**
   - 当前值：`[this] { return this->m_stopping.load(); }`。
   - 作用：停机开始后立即满足条件，使 monitor 不再进入下一轮扫描。

返回值为 `bool`：predicate 已满足时为 `true`，仅因 duration 到期时为 `false`。

### 25.2 为什么不能只等待一秒

如果不通知，stop 可能每次都额外等待最多一个 timeout interval。

notify 让 monitor 立即重新检查：

```text
m_stopping == true
  → wait_for 返回
  → monitor thread 退出
```

### 25.3 monitor 退出前可能正在关闭连接

如果 monitor 已经离开 wait 并取得 snapshot，它可能与 stop 同时调用 `closeConnection()`。

阶段七的 closing winner 保证只有一个线程清理；stop 最终 `join()` 保证 monitor 不再访问 registry 和 transport。

### 25.4 为什么要在 pending 排空前 join

timeout monitor 本身不消费 completion。停机已经主动关闭全部 snapshot 连接，因此不再需要它产生新的关闭请求。

先 join 可以减少仍在访问 transport 的线程集合，再进入最终 I/O 排空。

---

## 26. completion worker 必须继续排空

业务 worker 和 timeout thread 已经退出后，completion worker 仍然运行。

这是安全停机中最关键的阶段：

```text
所有新 I/O 已被禁止
所有 socket 已取消或关闭
  → 旧 operation 的 completion 正在陆续返回
  → completion worker 持续 GQCS
  → 每个 packet 调用 finishOperation
  → pending count 最终归零
```

### 26.1 等待代码

```cpp
std::unique_lock<std::mutex> lock{
    this->m_pendingIoOperationMutex};

this->m_pendingIoOperationCondition.wait(
    lock,
    [this] {
        return this->m_pendingIoOperationCount.load() == 0;
    });

lock.unlock();
```

### 26.2 `wait(lock, predicate)` 的两个参数

1. **`lock`**
   - 类型：`std::unique_lock<std::mutex>`。
   - 作用：让 condition variable 在阻塞时释放 pending mutex，在唤醒后重新取得。
   - 为什么不是 `lock_guard`：等待操作必须临时 unlock/relock。
2. **predicate**
   - 类型：可调用对象。
   - 当前条件：`m_pendingIoOperationCount == 0`。
   - 作用：处理提前满足和 spurious wakeup；每次唤醒都重新检查真实条件。

### 26.3 `wait()` 的行为

可以理解为：

```text
持有 mutex 检查 predicate
  → true：立即返回
  → false：原子地释放 mutex 并等待
  → 被通知后重新取得 mutex
  → 再次检查 predicate
```

### 26.4 为什么 worker 不能先退出

如果 pending count 为 3 时就退出 worker：

```text
3 个 canceled operation 的 packet 进入 port
  → 没有 worker 调用 GQCS
  → 没有人恢复 unique_ptr
  → 没有人 finishOperation
  → operation 和 connection 引用无法释放
```

### 26.5 为什么 port 不能先关闭

如果先 `CloseHandle(completionPort)`：

1. pending operation 失去正常 completion 消费基础设施。
2. worker 的 GQCS 进入 port 错误路径。
3. pending count 可能永远无法归零。
4. `OVERLAPPED*` 对应对象无法按原协议回收。

因此必须保证：

```text
pending count == 0
  → 才允许 completion worker 退出
  → worker 全部 join
  → 才允许关闭 port
```

---

## 27. 退出 completion worker 并关闭 port

pending count 归零后，项目为每个 completion thread 投递一个 control packet。

### 27.1 `PostQueuedCompletionStatus()` 原型

```cpp
BOOL PostQueuedCompletionStatus(
    HANDLE CompletionPort,
    DWORD dwNumberOfBytesTransferred,
    ULONG_PTR dwCompletionKey,
    LPOVERLAPPED lpOverlapped);
```

### 27.2 四个参数

1. **`CompletionPort`**
   - 项目值：`this->m_completionPort`。
   - 作用：指定接收人工 packet 的 completion port。
2. **`dwNumberOfBytesTransferred`**
   - 项目值：`0`。
   - 作用：人工 packet 携带的 bytes 字段；退出命令不需要数据量。
3. **`dwCompletionKey`**
   - 项目值：`StopCompletionKey`。
   - 作用：标识这是 worker 退出命令，而不是普通 socket completion。
4. **`lpOverlapped`**
   - 项目值：`nullptr`。
   - 作用：明确这不是需要恢复 `IoOperation` 的普通 I/O packet。

### 27.3 返回值

- 非零：人工 packet 已成功进入 completion port。
- `FALSE`：投递失败，使用 `GetLastError()` 取得 Win32 错误。

### 27.4 为什么一个 worker 需要一个 packet

一个 packet 只能由一个 `GetQueuedCompletionStatus()` 调用取走。

项目按 thread 数循环：

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

有 N 个 worker，就投递 N 个退出 packet。

### 27.5 worker 怎样识别

```cpp
if (!overlapped)
{
    if (completionKey == StopCompletionKey ||
        this->m_stopping.load())
    {
        return;
    }
    continue;
}
```

这里不会恢复 `IoOperation`，因为 `overlapped == nullptr`。

### 27.6 为什么只在 pending 归零后投递

如果过早投递，某个 worker 可能在 canceled operation packet 到达前退出。

即使还有其他 worker，系统也不再容易证明剩余并发数足以排空所有 packet。最强且最简单的不变量是：

> 退出 packet 出现时，已经不存在任何尚未对账的普通 I/O operation。

### 27.7 投递失败时的 fallback

如果某次人工 packet 投递失败，项目执行：

```cpp
CloseHandle(this->m_completionPort);
```

关闭 port 会唤醒没有收到退出 packet 的 worker；GQCS 返回错误且 `overlapped == nullptr`，worker 看到 `m_stopping == true` 后返回。

这个 fallback 仍然安全的前提是：

```text
pending I/O 已经归零
```

此时关闭 port 不会丢失普通 operation completion。

### 27.8 join 与最终 `CloseHandle()`

正常路径：

```text
每个 worker 取得退出 packet
  → runCompletionWorker 返回
  → stop join 全部 completion thread
  → CloseHandle(completionPort)
  → completionPort = nullptr
```

`CloseHandle()` 的参数是要关闭的 Win32 handle。这里传 completion port，而不是 socket。

只有 join 后关闭 port，才能保证没有 worker 正在或即将再次调用 GQCS。

### 27.9 部分启动失败怎样复用同一条路径

`start()` 成功创建 completion port 后，如果创建 socket、绑定、监听、关联 port、查询端口或预投递 accept 失败，就会调用 `stop()`。

如果 completion port 本身创建失败，函数直接返回 `false`。此时还没有 port、listener 或 Overlapped operation 需要排空，并且 `m_stopping` 尚未变成 true。

构造阶段已经启动的三个 task pool 仍由后续显式 `stop()` 或 destructor 收尾。

此时各资源可能只初始化了一部分：

- listening socket 可能仍为 `INVALID_SOCKET`。
- timeout thread 可能尚未创建。
- completion thread vector 可能为空或只有部分 worker。
- completion port 可能为空。
- pending operation 可能为零，也可能已有部分 accept。

停机代码通过 handle validity、`joinable()`、thread vector size 和 `if (m_completionPort)` 分支只清理真实存在的资源。幂等入口让 destructor 后续再次调用时可以直接返回。

---

## 28. `stop()` 的完整时间线

把项目代码压缩为十个步骤：

### 28.1 步骤一：建立 stopping 分界线

```text
持有 pending mutex
  → m_stopping.exchange(true)
  → 重复 stop 立即返回
  → 释放 pending mutex
```

保护目标：不允许新的 operation 在排空账本之外进入系统。

### 28.2 步骤二：公开停止并唤醒 monitor

```text
listeningPort = 0
  → notify idle timeout condition
```

保护目标：外部不再把 transport 视为监听中，monitor 不再等待完整 timeout interval。

### 28.3 步骤三：取消 listening socket

```text
取得 acceptMutex
  → CancelIoEx(listenSocket, nullptr)
  → closesocket(listenSocket)
  → listenSocket = INVALID_SOCKET
```

保护目标：停止新连接入口，并与 accept 提交/context 更新串行化。

### 28.4 步骤四：关闭活动连接

```text
registry.snapshot()
  → 对每个 connection 调用 closeConnection(ServerShutdown)
```

保护目标：让 snapshot 中的已注册连接 terminal，清除等待业务状态，取消 receive/send。要把“snapshot 中”扩展为“全部连接”，还必须满足第 23.5 节的注册同步或回滚条件。

### 28.5 步骤五：停止业务任务池

```text
screen pool stop
  → file pool stop
  → shell pool stop
```

保护目标：拒绝新 task，丢弃 waiting task，让 running task 返回并 join worker。

### 28.6 步骤六：join timeout monitor

```text
timeout condition 已通知
  → join idle timeout thread
```

保护目标：不再有 monitor 访问 registry 或发起关闭。

### 28.7 步骤七：等待 pending I/O 归零

```text
completion workers 保持运行
  → canceled completions 持续返回
  → finishOperation 持续减计数
  → predicate 达到 pending == 0
```

保护目标：所有已登记 `OVERLAPPED*` 都已经回到应用并完成对账。

completion worker 从 socket 开始取消时就在并发消费 packet；步骤七只是等待这一过程收敛，并不是到这里才开始处理 completion。

### 28.8 步骤八：投递 worker 退出 packet

```text
completion thread 数量 = N
  → 投递 N 个 StopCompletionKey packet
```

保护目标：让每个阻塞在 GQCS 的 worker 都有正常退出事件。

### 28.9 步骤九：join completion workers

```text
每个 worker 返回
  → join
  → clear thread vector
```

保护目标：确认再也没有线程访问 completion port。

### 28.10 步骤十：关闭 completion port

```text
CloseHandle(completionPort)
  → completionPort = nullptr
  → 记录 transport.stopped
```

保护目标：释放最后的内核调度基础设施。

### 28.11 一句话记忆

```text
先关入口
  → 再关生产者
  → 保留消费者排空旧结果
  → 最后退出消费者并关闭队列本身
```

这里的“生产者”包括 socket 投递线程和业务任务池；“消费者”是 completion worker；“队列本身”是 completion port。

---

## 29. 七个关键竞态推演

### 29.1 `postReceive()` 与 `stop()`

涉及两把锁：

```text
postReceive：socketMutex → pending mutex
stop 起始：pending mutex，只设置 stopping 后立即释放
closeConnection：socketMutex
```

只看“是否越过 stopping 分界线”时，结果分成两类：

1. receive 在 stopping 分界线前登记；native 提交成功时，随后由正常或取消 completion 对账。
2. stopping 先建立，receive 登记失败，不调用 `WSARecv()`。

第一类中如果 `WSARecv()` 同步失败，提交线程会立即调用 `finishOperation()` 回滚，不等待未来 completion。无论 native 结果怎样，都不允许出现“receive 已成功提交但没有进入 pending count”。

### 29.2 为什么 stop 不能持有 pending mutex 再等待 socket mutex

如果改成：

```text
stop 持有 pending mutex
  → closeConnection 等待 socketMutex
```

同时另一线程：

```text
postReceive 持有 socketMutex
  → tryBeginOperation 等待 pending mutex
```

两者形成锁环：

```text
stop：pending → 等 socket
post：socket → 等 pending
```

项目在设置 stopping 后立即释放 pending mutex，再进入 accept/socket 清理，避免这个 deadlock。

### 29.3 accept completion 与 listening socket 关闭

两条路径都使用 `m_acceptMutex`：

```text
handler：检查 stopping 和 listenSocket
  → SO_UPDATE_ACCEPT_CONTEXT

stop：CancelIoEx listener
  → closesocket listener
```

如果 handler 先完成 context 更新，stop 随后关闭 listener；如果 stop 先执行，handler 放弃 accepted socket 初始化并关闭它。

### 29.4 timeout 与 server stop 同时关闭连接

```text
timeout：closeConnection(connection, IdleTimeout)
stop：closeConnection(connection, ServerShutdown)
```

只有一个 `tryBeginClosing()` 成功：

- winner 写 reason、移除 registry、关闭 socket。
- loser 立即返回。

最终 reason 取决于谁先赢得状态转换，而不是谁最后写日志。

### 29.5 canceled completion 与已关闭 connection

```text
stop winner 已把 connection 标记 Closed
  → canceled receive completion 到达
  → operation 仍持有 connection shared_ptr
  → handler 看到 terminal
  → 再次 close 成为 loser
  → operation 析构，引用减少
```

这说明状态幂等性和共享所有权必须同时存在：

- state 防止重复清理。
- 共享所有权防止失败 handler 访问悬空对象。

### 29.6 `TaskPool::stop()` 与正在运行的 task

三种 task 状态：

1. **仍在 waiting deque**
   - `m_tasks.clear()` 直接销毁。
2. **已被 worker 取出但刚准备执行**
   - task 可能进入函数体，应在入口检查 `m_stopping` 或 terminal state。
3. **已经阻塞在同步 I/O**
   - `CancelSynchronousIo()` 尝试唤醒。
   - task 仍必须返回，worker 才能 join。

因此“清空任务队列”不等于“任务池已经停止”。真正的完成点是全部 worker join。

### 29.7 accepted socket 晚于 stop snapshot 注册

危险交错：

```text
handler 已完成 accept context 更新
  → handler 读取 stopping == false
  → stop 设置 stopping = true
  → stop 关闭 listener 并取得 connection snapshot
  → handler 才把 accepted socket 加入 registry
  → postReceive 因 stopping 返回 false
```

这里没有新的 pending receive 帮助后续触发清理，snapshot 也没有包含该 connection。

所以“停止 listener”与“对旧 snapshot 逐个 close”之间，还需要证明 accepted socket 注册已经越过一个共同同步点，或让晚到的注册路径自己执行关闭回滚。

---

## 30. 安全停机必须保持的不变量

### 30.1 单连接关闭不变量

```text
只有 tryBeginClosing winner
  才能写 closeReason、移除 registry、关闭 socket 和 markClosed
```

### 30.2 socket 投递不变量

```text
检查 terminal / stopping / socket validity
  与 native I/O 提交
  必须受同一个 socketMutex 串行化
```

### 30.3 新 I/O 准入不变量

```text
m_stopping 变成 true 之后
  m_pendingIoOperationCount 不再增加
```

### 30.4 operation 对账不变量

```text
每次 tryBeginOperation 成功
  恰好对应一次 finishOperation
```

### 30.5 operation 生命周期不变量

```text
只要 Windows 仍可能返回某个 OVERLAPPED*
  对应 IoOperation 和 buffer 就必须存活
```

### 30.6 connection 生命周期不变量

```text
只要某个 receive/send operation 尚未回收
  operation 内的 shared_ptr 就保持 ConnectionContext 存活
```

### 30.7 completion 基础设施不变量

```text
pending I/O > 0
  → completion port 必须有效
  → 至少有 completion worker 继续消费 packet
```

### 30.8 worker 退出不变量

```text
只有 pending I/O == 0
  才能投递正常退出 packet
```

### 30.9 port 释放不变量

```text
只有 completion workers 全部 join
  才能 CloseHandle(completionPort)
```

### 30.10 任务池不变量

```text
TaskPool::stop 首次调用返回
  → waiting tasks 已销毁
  → worker threads 已 join
  → 不再有 task 访问 pool 或 transport
```

### 30.11 accepted socket 注册不变量

```text
stopping 分界线之后
  → accepted socket 要么不会进入 registry
  → 要么进入后立即由明确路径移除并关闭
```

一次独立的原子读取不能单独证明这个跨对象不变量。

---

## 31. 常见错误与直接后果

### 31.1 把取消当成完成

错误：

```text
CancelIoEx 返回
  → 立即 delete IoOperation
```

后果：completion 到达时 `OVERLAPPED*` 已悬空。

### 31.2 关闭 socket 后清空 pending count

错误：直接把 counter 设为 0。

后果：账本与真实 operation 数量失去对应，worker 可能提前退出。

### 31.3 同步投递失败漏调 `finishOperation()`

后果：该 operation 不会产生 completion，pending count 永远多一，`stop()` 永久等待。

### 31.4 立即成功时在提交路径减计数

后果：后续 IOCP packet 再减一次，counter 过早归零或变成负值。

### 31.5 completion worker 看见 `FALSE` 就退出

后果：取消和其他失败 operation 无法恢复 `unique_ptr`，连接引用和 buffer 泄漏。

### 31.6 pending 未归零就投递退出 packet

后果：worker 可能先消费控制 packet 并退出，普通 canceled completion 留在 port 中。

### 31.7 worker 未 join 就关闭 completion port

后果：GQCS 进入错误路径，worker 与 port 生命周期发生竞态。

### 31.8 只投递一个退出 packet

后果：一个 worker 退出，其余 worker 永久阻塞在 GQCS。

### 31.9 持有 pending mutex 进入 connection close

后果：可能与 `socketMutex → pending mutex` 的投递路径形成 deadlock。

### 31.10 registry 移除前把 socket 改为无效值

后果：registry 无法按原 socket key 找到连接，entry 和 stream quota 残留。

### 31.11 认为 `Closed` 表示 operation 全部回收

后果：过早释放 completion worker、port 或 transport 生命周期拥有者。

### 31.12 任务池只清 queue，不 join worker

后果：运行中的 task 仍可能访问已经析构的 transport。

### 31.13 把 `CancelSynchronousIo()` 当成 thread terminate

后果：忽略 task 自身退出逻辑，`join()` 仍可能等待很久或永久阻塞。

### 31.14 从 transport 管理的 worker 内调用同步 `stop()`

后果：stop 可能尝试 join 当前 completion、业务或 timeout worker，形成 self-join 错误或 deadlock。

项目 worker 路径只请求单连接关闭或向外部生命周期拥有者报告结果；transport 级 `stop()` 应由拥有服务生命周期的外部线程调用。

### 31.15 把重复 `stop()` 返回当作停机已经完成

后果：loser 调用可能在线程、operation 和 port 仍由 winner 收尾时继续析构外围对象。

应由单一生命周期拥有者等待首次停机调用返回；重复调用只提供幂等保护，不提供停机完成屏障。

### 31.16 使用 `CloseHandle()` 关闭 socket

后果：混淆 Win32 handle 与 Winsock socket 的释放协议。

- socket：`closesocket()`。
- completion port：`CloseHandle()`。

---

## 32. 映射到项目源码

以下入口都相对于源项目根目录：

> `D:\CodeRepository\claude\remote_control`

### 32.1 推荐阅读顺序

1. **连接与停机字段**
   - 位置：`server_transport/internal/RemoteControlTransportImpl.h:245`
   - 观察：`socketMutex`、发送状态、文件状态和 connection state。
2. **停机相关接口与全局字段**
   - 位置：`server_transport/internal/RemoteControlTransportImpl.h:396`
   - 观察：`stop()`、`closeConnection()`、pending counter、condition variable 和 worker containers。
3. **状态机 closing winner**
   - 位置：`server_transport/src/RemoteControlTransportRuntime.cpp:86`
   - 观察：`tryBeginClosing()` 与 `markClosed()`；只复习输出语义。
4. **registry 移除与 snapshot**
   - 位置：`server_transport/src/RemoteControlTransportRuntime.cpp:288`
   - 观察：按原 socket key 移除、释放 role quota、复制稳定 `shared_ptr`。
5. **业务任务池停止**
   - 位置：`server_transport/src/RemoteControlTransportRuntime.cpp:168`
   - 观察：拒绝新任务、清 queue、`CancelSynchronousIo()` 和两轮 thread 遍历。
6. **整体 `stop()`**
   - 位置：`server_transport/src/RemoteControlTransport.cpp:173`
   - 观察：从 stopping 分界线到 completion port 关闭的完整顺序。
7. **accept 登记与同步失败回滚**
   - 位置：`server_transport/src/RemoteControlTransport.cpp:276`
   - 观察：全局 pending count 与 accept depth count 怎样同时变化。
8. **receive/send 投递锁边界**
   - 位置：`server_transport/src/RemoteControlTransport.cpp:335`
   - 观察：`socketMutex`、`tryBeginOperation()`、同步失败 `finishOperation()`。
9. **completion worker 对账**
   - 位置：`server_transport/src/RemoteControlTransport.cpp:417`
   - 观察：非空 `overlapped` 先 `finishOperation()`，再恢复 `unique_ptr`。
10. **取消后的三个 handler**
    - 位置：`server_transport/src/RemoteControlTransport.cpp:502`
    - 观察：accept、receive、send 在 stopping 或 terminal 状态下怎样收尾。
11. **单连接关闭与 pending helper**
    - 位置：`server_transport/src/RemoteControlTransport.cpp:727`
    - 观察：closing winner、三把 connection lock、三个关闭 API 和 pending 归零通知。
12. **业务 task 的协作停止**
    - 位置：`server_transport/src/RemoteControlTransportFileTransfer.cpp:40`
    - 观察：递归操作怎样在入口和循环中检查 `m_stopping`。
13. **并发停机生命周期测试**
    - 位置：`tests/TransportLifecycleTests.cpp:17`
    - 观察：40 次 server 生命周期、连接到达竞态和未完成 receive。
14. **错误隔离与停止后的干净收尾**
    - 位置：`tests/TransportResilienceTests.cpp:106`
    - 观察：连接级故障不影响后续连接，压力请求结束后显式 `server.stop()`。

### 32.2 阅读时追踪七条线

```text
准入线：
m_stopping false → true → 所有新工作被拒绝

连接线：
active phase → Closing → registry remove → socket close → Closed

operation 线：
tryBeginOperation → native submit → completion / sync failure → finishOperation

所有权线：
unique_ptr release → Windows 保存地址 → worker 恢复 unique_ptr

任务线：
waiting task 清除 → running task 协作返回 → task worker join

线程线：
timeout join → pending 排空 → completion worker stop packet → completion join

handle 线：
listening socket → connection sockets → completion port
```

---

## 33. 阶段练习与验收

按顺序完成。练习只要求阅读关键代码并进行纸面推演，不需要在项目中新增或构建示例代码。

### 33.1 任务一：区分四种“结束”

**练习**

分别说明下面四句话能证明什么，不能证明什么：

1. `connection->state.phase() == Closed`。
2. `connection->socket == INVALID_SOCKET`。
3. `m_pendingIoOperationCount == 0`。
4. 首次进入停机的 `stop()` winner 已经返回。

**验收标准**

- [ ] 不把 `Closed` 等同于 operation 全部析构。
- [ ] 知道 `INVALID_SOCKET` 只表示应用不再使用 native handle。
- [ ] 知道 pending 为零表示已登记 Overlapped operation 全部对账。
- [ ] 知道 pending 为零时，最后一个 handler 仍可能尚未返回。
- [ ] 知道 `stop()` 返回还包含 task、timeout 和 completion worker join。

**参考答案与解释**

1. **phase 为 `Closed`**
   - 能证明：单连接 closing winner 已完成项目定义的逻辑清理和 socket close。
   - 不能证明：canceled operation 已全部回到 worker。
2. **socket 为 `INVALID_SOCKET`**
   - 能证明：应用不应再使用该成员提交 socket I/O。
   - 不能证明：Windows 不会再交付旧 operation 的 completion。
3. **pending count 为 0**
   - 能证明：所有成功登记的 accept/receive/send operation 都已经对账。
   - 不能单独证明：最后一个 completion handler 已返回、operation 已析构、业务 task 或 worker thread 已 join。
4. **停机 winner 的 `stop()` 返回**
   - 能证明：本项目同步停机链已经完成，业务 worker、timeout thread 和 completion workers 已 join，port 已关闭。

重复 `stop()` loser 的提前返回不具备这项证明力。

### 33.2 任务二：排列单连接关闭顺序

**练习**

把下列动作按项目顺序排列，并为每一步写出前置条件：

```text
markClosed
registry.remove
tryBeginClosing
清空 sendQueue
fileTransfer.reset
CancelIoEx
shutdown
closesocket
socket = INVALID_SOCKET
写 closeReason
```

再回答：为什么 `registry.remove` 必须早于 `socket = INVALID_SOCKET`？

**验收标准**

- [ ] `tryBeginClosing` 位于所有有副作用清理之前。
- [ ] 只有 winner 写 reason。
- [ ] 应用级 waiting state 在 native close 前释放。
- [ ] registry 使用原 socket key 移除。
- [ ] `CancelIoEx → shutdown → closesocket` 顺序正确。
- [ ] `markClosed` 位于当前连接清理末尾。

**参考答案与解释**

```text
tryBeginClosing
  → 写 closeReason
  → 清空 sendQueue
  → fileTransfer.reset
  → registry.remove
  → 取得 socketMutex
  → CancelIoEx
  → shutdown
  → closesocket
  → socket = INVALID_SOCKET
  → markClosed
```

registry map 以原 socket 值为 key。先改成 `INVALID_SOCKET` 会导致 `remove()` 找不到 entry，也无法正确释放角色 quota。

### 33.3 任务三：解释三个关闭 API 的参数

**练习**

不看讲义，解释以下调用中的每个实参：

```cpp
CancelIoEx(reinterpret_cast<HANDLE>(socketHandle), nullptr);
shutdown(socketHandle, SD_BOTH);
closesocket(socketHandle);
```

并回答：哪一个调用负责等待 canceled completion？

**验收标准**

- [ ] 能解释 socket 到 `HANDLE` 的类型适配不转移所有权。
- [ ] 知道 `nullptr` 表示请求取消该 handle 上全部 pending I/O。
- [ ] 知道 `SD_BOTH` 停止两个通信方向。
- [ ] 知道只有 `closesocket()` 关闭 socket handle。
- [ ] 知道三个 API 都不负责消费 IOCP packet。

**参考答案与解释**

- `CancelIoEx` 第一个实参指定目标 socket 的 handle 表示；第二个实参为空，表示不限定某一个 `OVERLAPPED`。
- `shutdown` 第一个实参是 connected socket；第二个实参 `SD_BOTH` 同时关闭 receive/send 方向。
- `closesocket` 的唯一实参是要释放的 socket handle。
- 等待和消费 canceled completion 的是仍在运行的 completion worker，不是这三个 API。

### 33.4 任务四：完成 pending 账本

**练习**

初始：

```text
global pending = 0
pending accept = 0
```

依次发生：

1. 一个 `AcceptEx()` 登记并成功 pending。
2. 一个 receive 登记并成功 pending。
3. 一个 send 完成登记，但 `WSASend()` 同步返回其他错误。
4. stop 调用 `CancelIoEx()`，尚无 completion 到达。
5. accept 取消 completion 被 worker 取出。
6. receive 取消 completion 被 worker 取出。

写出每一步后的两个计数。

**验收标准**

- [ ] accept 提交后两个计数都增加。
- [ ] receive 只增加 global pending。
- [ ] send 同步失败先增加再立即归还，净变化为零。
- [ ] `CancelIoEx()` 返回时计数不变化。
- [ ] accept completion 同时减少 global 和 accept count。
- [ ] 最终 global pending 与 accept count 都为零。

**参考答案与解释**

1. accept 成功提交：global `1`，accept `1`。
2. receive 成功提交：global `2`，accept `1`。
3. send 同步失败完成回滚：global 先到 `3`，随后回到 `2`；accept 仍为 `1`。
4. `CancelIoEx()` 刚返回：global `2`，accept `1`。
5. accept completion：global `1`，accept `0`。
6. receive completion：global `0`，accept `0`。

取消调用不直接修改账本；只有提交失败回滚或 completion 对账修改 global pending。

### 33.5 任务五：推演 `postReceive()` 与 stop

**练习**

分别推演两个顺序：

```text
路径 A：postReceive 先取得 socketMutex
路径 B：stop 先建立 m_stopping = true
```

为集中练习停机分界线，路径 A 假设 `WSARecv()` 成功提交；同步失败回滚已经在任务四练习。

每条路径回答：

1. `tryBeginOperation()` 返回什么？
2. pending count 是否增加？
3. 是否调用 `WSARecv()`？
4. 如果已提交，最终由谁回收 operation？

**验收标准**

- [ ] 路径 A 的 operation 被计数并可由 stop 取消。
- [ ] 路径 B 在登记阶段被拒绝。
- [ ] 不出现“提交成功但未计数”。
- [ ] 能解释 socketMutex 与 pending mutex 的不同职责。

**参考答案与解释**

**路径 A：**

```text
socketMutex 已持有
  → tryBeginOperation 在 stopping 分界线前成功
  → pending++
  → WSARecv 提交
  → stop 随后取消
  → completion worker finishOperation 并恢复 unique_ptr
```

**路径 B：**

```text
stop 在 pending mutex 下设置 stopping
  → tryBeginOperation 返回 false
  → pending 不增加
  → 不调用 WSARecv
```

### 33.6 任务六：推演 canceled receive 的所有权

**练习**

从 receive operation 已经 pending 开始，一直写到 `IoOperation` 和 `ConnectionContext` 最终可析构。

必须包含：

```text
registry remove
socket close
markClosed
失败 completion
finishOperation
恢复 unique_ptr
handler terminal gate
shared_ptr 释放
```

**验收标准**

- [ ] registry remove 不会立即销毁 context。
- [ ] socket close 不会立即销毁 operation。
- [ ] canceled completion 仍携带原 `OVERLAPPED*`。
- [ ] worker 先对账再恢复 operation 所有权。
- [ ] handler 的重复 close 成为 loser。
- [ ] operation 析构时释放其 connection 强引用。

**参考答案与解释**

```text
receive operation 持有 connection shared_ptr
  → closing winner 从 registry 移除 connection
  → 取消并关闭 socket
  → connection phase = Closed
  → Windows 交付失败 completion
  → worker 调用 finishOperation
  → worker 从 OVERLAPPED* 恢复 unique_ptr<IoOperation>
  → receive handler 看到 failure / terminal
  → closeConnection 再次竞争失败
  → handler 返回
  → IoOperation 析构
  → operation 持有的 connection shared_ptr 释放
  → 最后一个强引用消失时 ConnectionContext 才析构
```

### 33.7 任务七：判断停机步骤能否交换

**练习**

分别说明交换下列步骤会产生什么问题：

1. 先 snapshot connections，再设置 `m_stopping`。
2. 先退出 completion workers，再等待 pending 为零。
3. 先关闭 completion port，再取消 sockets。
4. 先把 connection socket 设为 `INVALID_SOCKET`，再 registry remove。
5. 先 join task workers，再设置 pool stopping。
6. stop 持有 pending mutex 时直接等待 socket mutex。
7. accept handler 读取 stopping 为 false 后暂停，stop 完成 snapshot，handler 再执行 registry add。

**验收标准**

- [ ] 能指出新连接或新 I/O 漏入快照或排空过程的风险。
- [ ] 能指出丢失 completion consumer 的风险。
- [ ] 能指出 registry key 丢失和 quota 泄漏。
- [ ] 能指出任务 worker 无法退出。
- [ ] 能画出 pending/socket 两锁 deadlock 环。
- [ ] 能指出独立 atomic check 与 registry add 之间“先检查、后执行”的竞态窗口。

**参考答案与解释**

1. 新 accept 或新 operation 仍可能在 snapshot 后进入，停机遗漏工作。
2. canceled completion 没有消费者，operation 无法回收，pending 也无法归零。
3. GQCS 基础设施先失效，取消结果无法按原协议排空。
4. registry 以原 socket 为 key，移除失败并残留 quota。
5. worker 仍可能继续等待 condition 或领取新任务，join 无法完成。
6. post 路径可能持有 socket mutex 并等待 pending mutex，与 stop 形成锁环。
7. 新 connection 不在 stop snapshot 中，首次 receive 又会被 stopping 拒绝；如果没有回滚路径，registry entry 和 accepted socket 会残留。

### 33.8 任务八：解释任务池停止

**练习**

分别说明 `TaskPool::stop()` 对以下任务的处理：

1. 仍在 deque 中的 task。
2. 已取出但尚未检查 `m_stopping` 的 task。
3. 正在同步文件 I/O 中的 task。
4. 正在执行 CPU 编码的 task。

并解释 `CancelSynchronousIo(thread.native_handle())` 的唯一参数。

**验收标准**

- [ ] waiting task 由 `clear()` 销毁。
- [ ] running task 不会被 `clear()` 强行撤回。
- [ ] 同步 I/O 只进行尽力而为的取消。
- [ ] CPU 工作依赖协作检查或自然返回。
- [ ] 最终完成点是 thread join。

**参考答案与解释**

1. waiting task 从 deque 清除，捕获对象随 `std::function` 析构。
2. 已取出的 task 可能进入函数体，必须自行检查 stopping/terminal。
3. stop 使用 worker 的 native thread handle 请求取消该线程的同步 I/O，然后等待 task 返回。
4. CPU 编码不会被同步 I/O 取消 API 直接终止，只能检查状态或执行结束。

`hThread` 指定发起同步 I/O 的 worker thread，不是文件 handle，也不是 task ID。

### 33.9 任务九：阅读生命周期与韧性测试

**练习**

阅读：

- `tests/TransportLifecycleTests.cpp`
- `tests/TransportResilienceTests.cpp`

分别回答：

1. lifecycle test 为什么循环创建 40 个 server instance？
2. connector thread 为什么在 shutdown 同时持续连接？
3. 为什么客户端只写一个不完整 byte？
4. `stop()` 返回后检查 `listeningPort() == 0` 能证明什么？
5. resilience test 中的 partial-frame disconnect 验证哪种隔离？
6. 大量 one-shot 请求结束后显式 `stop()` 又覆盖什么？

**验收标准**

- [ ] 能指出测试主动制造 listener/accept/receive 与 stop 的竞态。
- [ ] 知道 incomplete packet 让 receive 保持 pending。
- [ ] 知道多轮生命周期用于放大偶发 race。
- [ ] 不把 `listeningPort() == 0` 当作全部内部资源的单独证明。
- [ ] 能解释单连接故障不应污染后续连接和全局停机。

**参考答案与解释**

1. 多轮创建与销毁用于提高偶发竞态、漏 join 和资源残留的暴露概率。
2. 它让 listening socket 关闭与新连接到达、accept completion 并发。
3. 不完整 Packet 不会结束业务链，服务端通常继续保留 pending receive，便于验证取消排空。
4. 它证明 public listening state 已清零；测试整体无 hang 返回还间接验证 stop 链完成。
5. partial-frame connection 中断后，后续正常连接仍应可服务，说明失败被限制在单连接范围。
6. 压力结束后的 stop 验证大量 operation 完成后 transport 仍能干净收尾。

### 33.10 任务十：最终综合验收

**练习**

闭卷复述下面场景：

```text
服务端有：
2 个 pending AcceptEx
连接 A 有 1 个 pending receive
连接 B 有 1 个 pending send
文件 task 正在同步读取
timeout monitor 正在等待
2 个 completion workers 阻塞在 GQCS

此时生命周期拥有者线程调用 stop()
```

一直写到 `transport.stopped` 日志出现。

**验收标准**

- [ ] 先在 pending mutex 下建立 stopping 分界线。
- [ ] listening socket 被取消并关闭，不再 accept 补位。
- [ ] A、B 都由 closing winner 关闭。
- [ ] 文件 task pool 停止并 join。
- [ ] timeout monitor 被唤醒并 join。
- [ ] 4 个已登记 operation 的 completion 全部被 worker 消费。
- [ ] canceled handler 不重复关闭连接。
- [ ] pending 归零后才出现退出 packet。
- [ ] 两个 worker 各取得一个退出 packet，并完成 join。
- [ ] completion port 最后关闭。

**参考答案与解释**

```text
生命周期拥有者取得 pending mutex
  → stopping false → true
  → 释放 pending mutex
  → listeningPort = 0，唤醒 timeout monitor
  → acceptMutex 下取消 2 个 AcceptEx 并关闭 listener
  → snapshot A、B
  → A、B 分别赢得 Closing，清等待状态并取消 socket I/O
  → completion workers 从取消开始即可并发取得最终 packet
  → 停止并 join screen/file/shell task pools
  → 文件同步 I/O 被尽力取消，task 返回
  → join timeout monitor
  → 两个 completion workers 始终保持 GQCS
  → 2 个 accept、A receive、B send 的最终 packet 可能已处理完，也可能仍在返回
  → 每个 packet 先 finishOperation，再恢复 operation
  → canceled handlers 看到 stopping/terminal，不续投、不重复 cleanup
  → global pending 从 4 变为 0
  → stop 进入 condition wait；若此前已归零就立即返回，否则等待剩余 packet 对账
  → 投递 2 个 StopCompletionKey packet
  → 两个 completion workers 返回并 join
  → CloseHandle(completionPort)
  → completionPort = nullptr
  → 记录 transport.stopped
```

完成全部任务后，应能用一句话概括阶段九：

> 安全停机先关闭所有新工作入口，再取消旧工作并保留 completion 消费能力，直到每笔 operation 完成对账，最后才退出 worker 和释放 completion port。

---

## 34. 下一阶段衔接

阶段九完成了服务端 IOCP 主体的生命周期闭环：

```text
启动
  → 异步 accept
  → receive / parse / dispatch
  → 任务池生产
  → 有序 send
  → 单连接关闭
  → 整体安全停机
```

阶段十将转向客户端和外围 Windows 能力，重点包括：

1. Qt 客户端连接、请求和 worker 生命周期。
2. 屏幕流与控制流客户端为什么分别维护连接。
3. 客户端断线、重连和主动停止怎样与服务端语义对应。
4. 截图、输入注入、剪贴板、Shell 和服务管理等外围 API。

进入阶段十前，应能准确回答：

> 为什么 `CancelIoEx()`、`closesocket()`、`pending == 0`、completion worker join 和 completion port close 是五个不同的完成点？

---

## 35. 官方资料与项目资料

阅读官方资料时重点核对：取消请求不等待、取消 operation 的 completion 语义、socket shutdown/close 区别、人工 completion packet 和 condition variable predicate wait。

- [Microsoft Learn：`CancelIoEx` function](https://learn.microsoft.com/en-us/windows/win32/api/ioapiset/nf-ioapiset-cancelioex)
- [Microsoft Learn：`CancelSynchronousIo` function](https://learn.microsoft.com/en-us/windows/win32/api/ioapiset/nf-ioapiset-cancelsynchronousio)
- [Microsoft Learn：`shutdown` function](https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-shutdown)
- [Microsoft Learn：`closesocket` function](https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-closesocket)
- [Microsoft Learn：`GetQueuedCompletionStatus` function](https://learn.microsoft.com/en-us/windows/win32/api/ioapiset/nf-ioapiset-getqueuedcompletionstatus)
- [Microsoft Learn：`PostQueuedCompletionStatus` function](https://learn.microsoft.com/en-us/windows/win32/api/ioapiset/nf-ioapiset-postqueuedcompletionstatus)
- [Microsoft Learn：I/O Completion Ports](https://learn.microsoft.com/en-us/windows/win32/fileio/i-o-completion-ports)
- [Microsoft Learn：`condition_variable` class](https://learn.microsoft.com/en-us/cpp/standard-library/condition-variable-class?view=msvc-170)
- [Microsoft Learn：`thread` class](https://learn.microsoft.com/en-us/cpp/standard-library/thread-class?view=msvc-170)

以下项目资料路径相对于 `D:\CodeRepository\claude\remote_control`：

- `server_transport/include/RemoteControlTransport.h`
- `server_transport/internal/RemoteControlTransportImpl.h`
- `server_transport/src/RemoteControlTransportRuntime.cpp`
- `server_transport/src/RemoteControlTransport.cpp`
- `server_transport/src/RemoteControlTransportFileTransfer.cpp`
- `tests/ConnectionStateMachineTests.cpp`
- `tests/TransportLifecycleTests.cpp`
- `tests/TransportResilienceTests.cpp`
