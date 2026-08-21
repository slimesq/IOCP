# IOCP 阶段七：状态机、有序发送与背压学习讲义

> 前置知识：阶段六已经跟踪一个 `TestConnection` 请求从首包分类、响应进入发送路径，到最终发送完成后正常关闭。
> 贯穿项目：`D:\CodeRepository\claude\remote_control`。
> 学习范围：连接角色的一次性分类、并发关闭的唯一执行者、每连接单一发送槽位、FIFO 发送队列、发送完成交接和单连接背压。任务池中的业务生产放在阶段八，取消 I/O、资源释放和安全停机放在阶段九。
> 本讲义只给出理解机制所需的关键代码，练习以纸面推演为主，不要求构建或修改项目。

## 1. 阶段七学习主线

阶段六只观察了最简单的发送情况：

```text
一个连接
  → 一个 TestConnection 请求
  → 一个响应
  → 一个 send operation
  → RequestComplete
```

这个路径没有回答多线程服务端必须解决的四个问题：

```text
两个 worker 同时尝试给连接分类，谁能决定最终角色？
多个线程同时产生响应，谁先调用 WSASend？
慢客户端来不及接收，服务端允许累计多少 bytes？
发送完成和关闭请求并发发生，谁负责最终关闭？
```

先把本阶段的机制分成两层，后面就不容易混淆：

| 层次 | 解决的问题 | 主要对象 |
| --- | --- | --- |
| 连接生命周期层 | 连接是什么角色、谁有权开始关闭 | `ConnectionStateMachine`、`ConnectionRegistry` |
| 每连接发送层 | 哪项先发、积压多少、何时交接 | `sendMutex`、`sendQueue`、`queuedSendBytes`、`sendPending` |

两层通过两个位置衔接：`enqueueBytes()` 会拒绝已经进入 `Closing` 或 `Closed` 的连接；发送链需要关闭时统一进入 `closeConnection()` 争夺关闭资格。

阶段七把这四个问题统一为一组连接级不变量：

```text
角色不变量：
  一个连接最多从 AwaitingRequest 分类一次

关闭不变量：
  多个关闭发起者中只有一个线程进入 Closing

发送不变量：
  每条连接最多占用一个发送槽位
  等待响应按 FIFO 排列

容量不变量：
  新 bytes 只有通过容量检查后才能进入连接发送状态
```

完整学习主线：

```text
首个 Packet
  → ConnectionRegistry::tryClassify
  → ConnectionStateMachine::tryClassify
  → CAS：AwaitingRequest → 固定业务角色

响应生产者 A 产生响应 A
  → enqueueBytes
  → sendPending 从 false 变为 true
  → A 成为当前 send operation

响应生产者 B、C 产生响应 B、C
  → 发现 sendPending == true
  → B、C 进入 sendQueue 尾部

A 的发送完成通知
  ├─ 只完成一部分：同一个 operation 继续剩余 bytes
  └─ 全部完成：扣减 A，取 queue 头部 B，继续 postSend

队列最终排空
  ├─ closeAfterSend == false：连接继续当前业务角色
  └─ closeAfterSend == true：closeConnection(RequestComplete)

任意错误、超时或停机发起关闭
  → tryBeginClosing
  → 只有一个线程赢得 Closing
```

先记住九个术语，其他名称在对应章节再学习：

| 术语 | 含义 |
| --- | --- |
| 连接阶段（phase） | `ConnectionPhase` 当前值，表示连接所处的生命周期或固定业务角色。 |
| 原子比较交换（CAS） | 只有原值等于预期值时，才原子地写入目标值。 |
| 关闭胜出者（closing winner） | 第一个成功把活动 phase 改成 `Closing` 的线程。 |
| 不变量（invariant） | 无论线程怎样交错执行，都必须保持成立的约束。 |
| 先进先出（FIFO） | 先成功进入等待队列的响应先成为下一项。 |
| 发送槽位（send slot） | 当前连接提交或准备提交一项发送工作的唯一资格。 |
| 在途项与等待项 | 在途项由当前 `IoOperation` 持有；等待项保存在 `sendQueue`。 |
| 发送积压（backlog） | 当前连接为了发送而保留的在途与等待 bytes 总量。 |
| 背压（backpressure） | 消费速度落后时，用有界准入阻止发送积压无限增长。 |

建议分五个学习单元推进：

1. **建立合法状态图（第 4～6 节）**
   - 解决的问题：哪些 phase 可以互相转换。
   - 学完自检：能闭卷画出完整合法转换图。
2. **理解 CAS 和关闭 winner（第 7～10 节）**
   - 解决的问题：一次分类和一次关闭怎样抵抗并发。
   - 学完自检：能推演两个分类线程和十六个关闭线程。
3. **建立发送不变量（第 11～13 节）**
   - 解决的问题：哪些字段和调用前提共同保证单在途发送。
   - 学完自检：能解释五条发送不变量及四个字段为什么必须共同修改。
4. **推演 FIFO 与背压（第 14～18 节）**
   - 解决的问题：三个 response 怎样顺序发送，容量怎样拒绝慢连接积压。
   - 学完自检：能完成 A、B、C 全状态推演和容量计算。
5. **映射源码并综合验收（第 19～21 节）**
   - 解决的问题：状态、bytes、锁、线程和失败入口怎样连成整体。
   - 学完自检：能不看代码复述阶段七全部不变量。

---

## 2. 知识范围

### 2.1 核心内容

| 主题 | 本阶段掌握内容 |
| --- | --- |
| 连接状态 | `ConnectionPhase`、合法转换、strong/weak CAS、closing winner |
| 角色容量 | registry mutex 如何把注册检查、状态分类和 stream quota 更新组成一个事务 |
| 有序发送 | 四个发送字段、五条不变量、单发送槽位、FIFO 交接与部分发送 |
| 资源边界 | 两个发送上限、overflow-safe 容量判断、`Backpressure` 与 `CapacityLimit` |

### 2.2 后续内容

| 主题 | 后续阶段 |
| --- | --- |
| shell、文件、截图任务怎样产生 response | 阶段八 |
| 文件发送完成后怎样驱动下一批读取 | 阶段八 |
| 屏幕帧请求合并与共享帧缓存 | 阶段八 |
| `CancelIoEx()`、`shutdown()`、`closesocket()` 的关闭顺序 | 阶段九 |
| pending I/O 计数归零与 worker 安全退出 | 阶段九 |
| 超时扫描和停机期间的完整资源证明 | 阶段九 |

本阶段会阅读 `closeConnection()` 开头的 closing winner 判断，但不展开 winner 取得资格后的 socket 取消与资源 drain。

---

## 3. 学习完成标准

完成本阶段后，应能够：

1. 写出七个 `ConnectionPhase`，画出合法转换图，并区分 phase 与 close reason。
2. 逐项解释 `tryClassify()`、strong CAS 的参数、返回值和 expected 回写。
3. 解释 weak CAS loop、closing winner 以及 `_previousPhase` 的作用。
4. 推演 registry mutex 内的 quota check、state CAS 和 counter update 事务。
5. 说明四个发送字段的职责，并检查五条发送不变量。
6. 逐项解释 `hasSendCapacity()` 与 `enqueueBytes()` 的参数和调用前提。
7. 推演 A、B、C 入队、部分发送、完整交接和最终 drain 的状态变化。
8. 解释 `push_back()`、`front()`、`pop_front()` 和非空 item 前提怎样共同支持 FIFO。
9. 推演 `requestCloseAfterSend()` 与最终发送完成的两种先后，并说明记录最终关闭意图后为什么不能再生产响应。
10. 根据两个发送上限完成容量计算，解释 first-send exception 和 overflow-safe 判断。
11. 区分 `CapacityLimit`、`Backpressure` 与 `RequestComplete`，并说明 IOCP completion queue 为什么不能替代业务 send queue。
12. 按源码位置连续追踪状态分类、发送准入、发送交接和关闭 winner。

建议投入 8～12 小时。

---

## 4. 为什么阶段六的线性路径不够

阶段六满足四个简化条件：

```text
只有一个合法首包
只有一个 response
没有已有 send
容量一定足够
```

真实服务端很快会遇到不同交错。

### 4.1 两个分类发起者

错误的 read-then-write：

```text
线程 A 读取 phase = AwaitingRequest
线程 B 读取 phase = AwaitingRequest

线程 A 写入 ScreenStream
线程 B 写入 ControlStream
```

结果取决于最后一次普通写入，连接角色不再可靠。

正常主链中，每条连接只保留一个 receive operation，首包通常不会真的由两个 worker 同时处理。这里仍使用 CAS，是为了让状态对象自身具备明确的并发契约：即使测试、错误路径或后续代码变化带来重复分类，也只能有一个调用成功。

### 4.2 多个响应生产者

```text
生产者 A 创建响应 A
生产者 B 创建响应 B
生产者 C 创建响应 C
```

本阶段不需要先学习任务池。只要 A 已经占用发送槽位，即使同一个 worker 随后又解析出 B、C 两个响应，它们也必须等待；阶段八才会加入真正来自业务 worker 的并发生产者。

如果多个生产者直接在同一 socket 上并发调用 `WSASend()`：

- 调用先后由线程调度决定，而不是业务顺序决定。
- Winsock provider 可能拆分大型 send，并使并发请求产生非预期交错。
- completion 可能由不同 worker 处理。
- 很难判断哪个 operation 完成后才能关闭。

### 4.3 慢客户端

```text
服务端生产 10 MiB/s
客户端只消费 100 KiB/s
```

如果每个新 response 都无条件保存：

```text
send backlog 持续增长
  → 单连接占用越来越多内存
  → 其他连接受到影响
  → 最终可能耗尽进程资源
```

### 4.4 多个关闭发起者

同一时刻可能发生：

```text
receive worker 发现协议错误
send worker 发现 I/O 失败
超时线程发现连接超时
stop thread 开始服务器停机
```

这些线程都可以提出“应该关闭”，但只能有一个线程执行状态清理和 quota 释放。

阶段七的目标不是消除并发，而是让任意交错都保持同一组不变量。

---

## 5. `ConnectionPhase` 与合法状态图

项目定义：

```cpp
enum class ConnectionPhase
{
    AwaitingRequest,
    OneShot,
    FileTransfer,
    ScreenStream,
    ControlStream,
    Closing,
    Closed,
};
```

每个 phase 的职责：

| phase | 类别 | 含义 |
| --- | --- | --- |
| `AwaitingRequest` | 初始 phase | 已接入并已提交 receive，等待首个 Packet 决定连接角色。 |
| `OneShot` | 业务角色 | 处理一次性命令，最终 response 完成后关闭。 |
| `FileTransfer` | 业务角色 | 处理目录、下载或删除类文件请求。 |
| `ScreenStream` | 持久角色 | 持续处理屏幕帧请求。 |
| `ControlStream` | 持久角色 | 持续处理鼠标和键盘控制 Packet。 |
| `Closing` | terminal phase | 某个线程已经取得关闭资格，新协议和 I/O 工作应被拒绝。 |
| `Closed` | terminal phase | 连接清理流程已经结束。 |

合法状态图：

```text
                         ┌─→ OneShot ─────────┐
                         ├─→ FileTransfer ────┤
AwaitingRequest ─────────┼─→ ScreenStream ────┼─→ Closing ─→ Closed
                         └─→ ControlStream ───┘

AwaitingRequest 也可以因错误、超时或停机直接进入 Closing
```

不允许的转换：

```text
AwaitingRequest → AwaitingRequest 作为“分类结果”
OneShot → ScreenStream
ScreenStream → ControlStream
Closing → 任意活动 phase
Closed → Closing
Closed → 任意活动 phase
```

需要区分 phase 与 close reason：

| 值 | 回答的问题 | 示例 |
| --- | --- | --- |
| `ConnectionPhase` | 连接现在处于哪个生命周期或业务角色 | `ScreenStream`、`Closing` |
| `ConnectionCloseReason` | 连接为什么进入 terminal 生命周期 | `IoFailure`、`Backpressure`、`RequestComplete` |

一个连接可以从 `ScreenStream` 进入 `Closing`，同时把 reason 记录为 `PeerDisconnected`。phase 和 reason 不能互相替代。

---

## 6. `ConnectionStateMachine` 的最小接口

项目接口：

```cpp
class ConnectionStateMachine final
{
public:
    ConnectionStateMachine() = default;

    [[nodiscard]] bool tryClassify(ConnectionPhase _phase) noexcept;
    [[nodiscard]] bool tryBeginClosing(ConnectionPhase* _previousPhase) noexcept;
    void markClosed() noexcept;

    [[nodiscard]] ConnectionPhase phase() const noexcept;
    [[nodiscard]] bool isTerminal() const noexcept;

private:
    std::atomic<ConnectionPhase> m_phase{
        ConnectionPhase::AwaitingRequest};
};
```

初始状态直接由成员初始化给出：

```text
构造 ConnectionStateMachine
  → m_phase = AwaitingRequest
```

接口参数与结果：

1. **`tryClassify(_phase)`**
   - `_phase`：希望分配的业务角色。
   - 作用：尝试执行一次 `AwaitingRequest → role`。
   - 结果：只有成功分类的线程得到 `true`。
2. **`tryBeginClosing(_previousPhase)`**
   - `_previousPhase`：可为空的输出指针；winner 可通过它取得关闭前 phase。
   - 作用：尝试把任意活动 phase 变为 `Closing`。
   - 结果：只有 closing winner 得到 `true`。
3. **`markClosed()`**
   - 参数：无。
   - 作用：尝试执行 `Closing → Closed`。
   - 结果：无返回值。
4. **`phase()`**
   - 参数：无。
   - 作用：读取当前 atomic phase。
   - 结果：返回 phase snapshot。
5. **`isTerminal()`**
   - 参数：无。
   - 作用：判断当前是否为 `Closing` 或 `Closed`。
   - 结果：返回 `bool`。

`noexcept` 表示这些状态操作承诺不抛出 C++ exception。关闭与 completion 路径可以使用它们，而不需要为 exception 设计另一套状态恢复逻辑。

为什么 `m_phase` 使用 `std::atomic<ConnectionPhase>`：

- 多个线程可以无 data race 地读取当前 phase。
- phase 的条件检查和写入可以合并成一次 CAS。
- 分类和关闭不需要用同一把大 mutex 包住所有连接。

atomic 只保护 phase 自身。角色 quota、send queue 等多字段业务不变量仍然需要各自的 mutex。

---

## 7. `tryClassify()`：一次性角色分类

项目实现：

```cpp
bool ConnectionStateMachine::tryClassify(ConnectionPhase _phase) noexcept
{
    if (_phase != ConnectionPhase::OneShot &&
        _phase != ConnectionPhase::FileTransfer &&
        _phase != ConnectionPhase::ScreenStream &&
        _phase != ConnectionPhase::ControlStream)
    {
        return false;
    }

    ConnectionPhase expected{ConnectionPhase::AwaitingRequest};
    return this->m_phase.compare_exchange_strong(expected, _phase);
}
```

### 7.1 `_phase` 参数

| 参数 | 允许值 | 作用 |
| --- | --- | --- |
| `_phase` | `OneShot`、`FileTransfer`、`ScreenStream`、`ControlStream` | CAS 成功时写入的目标业务角色。 |

传入 `AwaitingRequest`、`Closing` 或 `Closed` 会在 CAS 前直接返回 `false`。

### 7.2 `compare_exchange_strong()` 的两个参数

当前调用：

```cpp
this->m_phase.compare_exchange_strong(expected, _phase)
```

两个参数在 CAS 前后的变化：

1. **`expected`**
   - 调用前：值为 `AwaitingRequest`。
   - CAS 成功：保持原预期值。
   - CAS 失败：被回写为 `m_phase` 的实际值。
2. **`_phase`**
   - 调用前：保存希望得到的业务角色。
   - CAS 成功：该值被写入 `m_phase`。
   - CAS 失败：不会写入。

返回值：

- `true`：`m_phase` 原来确实是 `AwaitingRequest`，现已原子地改成 `_phase`。
- `false`：原值不符合预期，或连接已经由其他线程分类/关闭。

这里使用 strong 版本，因为分类只做一次 CAS 尝试。strong 不会在“当前值确实等于 expected”时产生 spurious failure。

项目没有显式传入 `std::memory_order` 参数。本阶段只学习这里可见的 expected/desired 语义，不展开自定义 memory order。

### 7.3 两个分类线程怎样竞争

初始：

```text
m_phase = AwaitingRequest
```

线程 A：

```text
expected = AwaitingRequest
desired = ScreenStream
CAS 成功
m_phase = ScreenStream
```

线程 B 随后执行：

```text
expected = AwaitingRequest
实际值 = ScreenStream
CAS 失败
expected 被改成 ScreenStream
m_phase 不变
```

最终只有一个合法角色：

```text
AwaitingRequest → ScreenStream
```

而不会发生：

```text
AwaitingRequest → ScreenStream → ControlStream
```

### 7.4 首包与角色的对应关系

当前项目的支持命令：

| 首包 command | 分类结果 |
| --- | --- |
| `TestConnection`、`ListDrives`、`RunFile` | `OneShot` |
| `ListDirectory`、`DownloadFile`、`DeleteFile` | `FileTransfer` |
| `WatchScreen` | `ScreenStream` |
| `ControlChannel` | `ControlStream` |

分类只决定连接角色，不执行文件读取、shell 打开或截图；这些业务工作在阶段八学习。

---

## 8. `tryBeginClosing()`：只有一个关闭 winner

项目实现：

```cpp
bool ConnectionStateMachine::tryBeginClosing(
    ConnectionPhase* _previousPhase) noexcept
{
    ConnectionPhase current{this->m_phase.load()};
    while (current != ConnectionPhase::Closing &&
           current != ConnectionPhase::Closed)
    {
        if (this->m_phase.compare_exchange_weak(
                current, ConnectionPhase::Closing))
        {
            if (_previousPhase)
            {
                *_previousPhase = current;
            }
            return true;
        }
    }
    return false;
}
```

### 8.1 `_previousPhase` 参数

| 参数 | 是否允许为空 | 作用 |
| --- | --- | --- |
| `_previousPhase` | 允许传入 `nullptr` | winner 成功时，接收进入 `Closing` 前的活动 phase。 |

调用示例：

```cpp
ConnectionPhase previousPhase{ConnectionPhase::AwaitingRequest};
if (!connection->state.tryBeginClosing(&previousPhase))
{
    return;
}
```

`&previousPhase` 是输出地址。函数只在 CAS 成功时写入；loser 不应使用自己的初始值推断连接原角色。

如果调用者不关心原角色，可以传入：

```cpp
state.tryBeginClosing(nullptr)
```

### 8.2 `load()` 与 weak CAS 参数

`m_phase.load()` 没有显式参数，返回调用时刻的 atomic phase snapshot。

当前 weak CAS：

```cpp
this->m_phase.compare_exchange_weak(
    current,
    ConnectionPhase::Closing)
```

| 参数 | 输入含义 | CAS 失败后的变化 |
| --- | --- | --- |
| `current` | 本轮认为 `m_phase` 当前具有的值 | 接收实际值；spurious failure 时也可能保持原值 |
| `ConnectionPhase::Closing` | 本轮希望写入的 desired value | 失败时不会写入 |

weak 版本允许 spurious failure，所以必须放在 loop 中。即使没有其他线程修改 phase，它也可能暂时返回 `false`；下一轮继续尝试即可。

### 8.3 loop 为什么能够收敛

每次失败后有两种情况：

```text
普通竞争失败
  → current 被更新成实际 phase
  → 如果仍是活动 phase，下一轮尝试 active → Closing

伪失败（spurious failure）
  → current 仍可能保持原值
  → 下一轮用同一 expected 重试
```

一旦另一个线程已经写入 `Closing` 或 `Closed`：

```text
while 条件不再成立
  → 返回 false
```

### 8.4 十六个关闭线程

项目测试同时启动 16 个关闭竞争线程：

```text
初始 phase = ScreenStream

线程 0  ─┐
线程 1  ─┤
...      ├─→ tryBeginClosing
线程 15 ─┘
```

不需要关心哪个线程胜出，只检查三个结果：

```text
closingWinners = 1
previousScreenPhases = 1
phase = Closing
```

只有胜出者返回 `true`、取得原角色 `ScreenStream` 并继续关闭流程；其余 15 个线程返回 `false`，不重复记录 reason 或清理资源。

### 8.5 `closeConnection()` 怎样使用 winner

阶段七只看开头：

```cpp
ConnectionPhase previousPhase{ConnectionPhase::AwaitingRequest};
if (!_connection->state.tryBeginClosing(&previousPhase))
{
    return;
}

_connection->closeReason.store(_reason);
```

`closeConnection()` 的两个参数在阶段六已经介绍：

| 参数 | 本阶段关注点 |
| --- | --- |
| `_connection` | 包含 atomic phase、close reason、registry identity 和连接级共享状态。 |
| `_reason` | 只有 closing winner 才能写入的终止原因。 |

`closeReason.store(_reason)` 的显式参数 `_reason` 是要保存的新值；memory-order 参数省略，使用默认顺序。

loser 在 `store()` 前返回，因此后到的错误不会覆盖第一个成功关闭者记录的 reason。

winner 后续怎样取消 I/O、关闭 socket 并等待全局 pending count，留到阶段九。

---

## 9. `markClosed()`、`phase()` 与 `isTerminal()`

项目实现：

```cpp
void ConnectionStateMachine::markClosed() noexcept
{
    ConnectionPhase expected{ConnectionPhase::Closing};
    static_cast<void>(this->m_phase.compare_exchange_strong(
        expected,
        ConnectionPhase::Closed));
}

ConnectionPhase ConnectionStateMachine::phase() const noexcept
{
    return this->m_phase.load();
}

bool ConnectionStateMachine::isTerminal() const noexcept
{
    ConnectionPhase const current{this->phase()};
    return current == ConnectionPhase::Closing ||
        current == ConnectionPhase::Closed;
}
```

### 9.1 `markClosed()`

`markClosed()` 没有参数，也没有返回值。内部 CAS 的两个参数：

| 参数 | 作用 |
| --- | --- |
| `expected` | 要求当前必须是 `Closing`；失败时接收实际 phase。 |
| `ConnectionPhase::Closed` | 成功时写入的最终 phase。 |

所以它只允许：

```text
Closing → Closed
```

如果错误地在 `OneShot` 或 `ScreenStream` 调用，CAS 失败，phase 不变。项目忽略返回值，因为正常调用位置已经由 closing winner 控制。

### 9.2 `phase()`

`phase()` 没有参数，返回当前 atomic phase 的 snapshot。snapshot 只描述读取瞬间；返回后其他线程仍可能把活动 phase 改成 `Closing`。

所以这种写法不能代替 CAS：

```cpp
if (state.phase() == ConnectionPhase::AwaitingRequest)
{
    // 此处其他线程仍可能已经改变 phase。
}
```

### 9.3 `isTerminal()`

`isTerminal()` 没有参数。它把两个 phase 统一视为 terminal：

```text
Closing → true
Closed  → true
其他    → false
```

提交 receive、提交 send、处理 Packet 等路径可以先检查 terminal 状态并拒绝新工作。

`isTerminal()` 只是读取判断；真正争夺关闭资格仍必须调用 `tryBeginClosing()`。

---

## 10. `ConnectionRegistry`：状态与角色 quota 的事务

项目中有两个同名函数，先区分职责：

1. **`ConnectionStateMachine::tryClassify(_phase)`**
   - 保护范围：单个 connection 的 atomic phase。
   - quota：不处理。
2. **`ConnectionRegistry::tryClassify(_connection, _phase)`**
   - 保护范围：registry entry、phase 和跨连接 stream counter。
   - quota：在同一事务中处理。

只用 atomic phase 不能保证角色 quota。例如两个连接都想成为唯一允许的 `ScreenStream`：

```text
connection A 有自己的 atomic phase
connection B 也有自己的 atomic phase
```

两个 atomic 彼此独立，无法共同保护全局 `screenStreamCount`。

项目使用 `ConnectionRegistry`：

```cpp
class ConnectionRegistry final
{
public:
    ConnectionRegistry(
        std::size_t _maximumConnections,
        int _maximumScreenStreams,
        int _maximumControlStreams);

    [[nodiscard]] bool tryClassify(
        std::shared_ptr<ConnectionContext> const& _connection,
        ConnectionPhase _phase);

    void remove(
        std::shared_ptr<ConnectionContext> const& _connection,
        ConnectionPhase _previousPhase);
};
```

阶段五已经使用过 `add(SOCKET _socket)`：唯一参数 `_socket` 是刚接入的 connected socket；成功时返回新注册的 `shared_ptr<ConnectionContext>`，total capacity 已满或 socket key 重复时返回空指针。

### 10.1 constructor 的三个参数

| 参数 | 默认项目配置 | 作用 |
| --- | ---: | --- |
| `_maximumConnections` | 256 | registry 最多同时保存多少个 active connection。 |
| `_maximumScreenStreams` | 4 | 同时分类为 `ScreenStream` 的最大连接数。 |
| `_maximumControlStreams` | 4 | 同时分类为 `ControlStream` 的最大连接数。 |

`OneShot` 和 `FileTransfer` 没有单独角色 counter，但仍受 total connection capacity 约束。

### 10.2 `tryClassify()` 的两个参数

| 参数 | 作用 |
| --- | --- |
| `_connection` | 必须仍在 registry 中的目标连接。共享引用保证函数期间对象存活。 |
| `_phase` | 首包请求选择的 immutable protocol role。 |

实现中的 `m_connectionsBySocket.find(_connection->socket)` 有一个参数：要查找的 socket key；返回对应 iterator。`end()` 没有参数，返回“未找到或遍历结束”的 sentinel iterator。

关键实现：

```cpp
bool ConnectionRegistry::tryClassify(
    std::shared_ptr<ConnectionContext> const& _connection,
    ConnectionPhase _phase)
{
    if (!_connection)
    {
        return false;
    }

    std::lock_guard<std::mutex> const lock{this->m_mutex};

    if (this->m_connectionsBySocket.find(_connection->socket) ==
        this->m_connectionsBySocket.end())
    {
        return false;
    }

    if (_phase == ConnectionPhase::ScreenStream &&
        this->m_screenStreamCount >= this->m_maximumScreenStreams)
    {
        return false;
    }

    if (_phase == ConnectionPhase::ControlStream &&
        this->m_controlStreamCount >= this->m_maximumControlStreams)
    {
        return false;
    }

    if (!_connection->state.tryClassify(_phase))
    {
        return false;
    }

    if (_phase == ConnectionPhase::ScreenStream)
    {
        ++this->m_screenStreamCount;
    }
    else if (_phase == ConnectionPhase::ControlStream)
    {
        ++this->m_controlStreamCount;
    }
    return true;
}
```

### 10.3 `std::lock_guard` 的参数与生命周期

当前构造：

```cpp
std::lock_guard<std::mutex> const lock{this->m_mutex};
```

constructor 的唯一参数 `this->m_mutex` 是要取得所有权的 mutex reference：

```text
进入这一行
  → lock_guard 锁住 m_mutex

离开当前 C++ scope
  → lock_guard destructor 自动解锁
```

destructor 没有参数。即使函数从中间 `return false`，局部 `lock` 仍会析构并释放 mutex。

### 10.4 为什么 quota check 必须在 state CAS 前

顺序：

```text
锁住 registry
  → 确认 connection 仍注册
  → 检查目标角色 quota
  → state CAS 分类
  → 分类成功后增加对应 counter
  → 解锁 registry
```

如果 screen quota 已满：

```text
tryClassify 返回 false
connection.state 仍是 AwaitingRequest
screenStreamCount 不变
```

项目测试随后还能把这个连接分类为 `OneShot`，证明失败的 quota attempt 没有污染 phase。

### 10.5 atomic 与 mutex 各自负责什么

| 机制 | 保护范围 |
| --- | --- |
| `ConnectionStateMachine` atomic CAS | 单个连接的 phase 只能合法转换一次。 |
| `ConnectionRegistry::m_mutex` | connection map、两个 stream counter、quota check 与 counter update 的整体一致性。 |

两者不是重复保护：atomic 维护连接内状态，registry mutex 维护跨连接总量。

### 10.6 `remove()` 为什么需要 previous phase

函数参数：

| 参数 | 作用 |
| --- | --- |
| `_connection` | 要从 active map 移除的 closing connection。 |
| `_previousPhase` | 进入 `Closing` 前的角色，用于释放对应 stream quota。 |

调用 remove 时，当前 state 已经是 `Closing`：

```text
state.phase() = Closing
```

仅查看当前 phase 已经无法知道应递减 screen counter 还是 control counter，所以 closing winner 必须保存 `_previousPhase`。

```text
ScreenStream → Closing
  → previousPhase = ScreenStream
  → registry.remove(..., ScreenStream)
  → screenStreamCount--
```

`remove()` 还会在同一把 registry mutex 下确认 socket key 仍存在，并且 map 中保存的正是当前 `_connection`，然后才 erase entry 和递减 quota。这样旧 connection 引用不会误删后来复用同一 socket key 的对象。

这就是 state CAS 输出和 registry quota cleanup 的衔接点。

---

## 11. 每连接发送状态与五条不变量

`ConnectionContext` 中与阶段七有关的字段：

```cpp
std::mutex sendMutex;
std::deque<QByteArray> sendQueue;
std::size_t queuedSendBytes{0};
bool sendPending{false};
bool closeAfterSend{false};
```

字段职责：

| 字段 | 保存什么 | 不保存什么 |
| --- | --- | --- |
| `sendMutex` | 其余四个发送字段的互斥访问权 | 不保护 socket handle，也不保护 registry map |
| `sendQueue` | 尚未成为 send operation 的 waiting items | 不包含当前 in-flight item |
| `queuedSendBytes` | 当前 in-flight item 与全部 waiting items 的完整 byte 总数 | 不是本次 completion 的 byte 数 |
| `sendPending` | send slot 是否已经被当前发送链占用 | 不表示 queue 中有多少项 |
| `closeAfterSend` | 队列排空并完成最后一项后是否正常关闭 | 不会主动发送，也不是 close reason |

新连接初始值：

```text
sendQueue = []
queuedSendBytes = 0
sendPending = false
closeAfterSend = false
```

下面的发送不变量适用于尚未进入 terminal phase 的连接。closing winner 会清空 queue 和 admission counter；取消后仍返回的 operation 怎样收尾属于阶段九。

### 11.1 不变量一：queue 不包含当前 operation

假设 A 正在发送，B 和 C 等待：

```text
当前 IoOperation 持有 A
sendQueue = [B, C]
```

不能写成：

```text
sendQueue = [A, B, C]
```

否则 A completion 时无法判断 queue 头部是“刚完成的 A”还是“下一项 B”。

### 11.2 不变量二：byte counter 包含全部 retained bytes

```text
queuedSendBytes
  = A 的完整 sendBytes.size()
  + B.size()
  + C.size()
```

即使 A 已经部分完成，operation 仍然持有完整 `QByteArray`，所以 A 在全部完成前仍按完整 item size 计数。

`queuedSendBytes` 是项目采用的“逻辑 retained byte 数”，用于稳定执行容量策略；它不是精确的进程堆内存统计值。判断是否准入时应使用这个 counter，不能拿任务管理器中的瞬时内存反推 queue 状态。

### 11.3 不变量三：queue 非空时 send slot 必须被占用

稳定状态下：

```text
sendQueue 非空 → sendPending == true
sendPending == false → sendQueue 为空
```

`sendPending` 在真正调用 `postSend()` 前先变为 `true`，因此更准确的理解是：

> 一个发送链已经取得当前连接唯一的 send slot，正在提交或已经存在一个在途 operation。

### 11.4 不变量四：最终关闭意图之后不再产生新响应

`closeAfterSend = true` 表示当前连接已经提交最终响应。协议层和业务生产者必须停止继续 enqueue 新响应。

如果记录最终关闭意图后仍允许新生产者入队，关闭线程可能在旧队列排空时关闭 socket，使新响应被取消。

因此 `closeAfterSend` 不只是发送字段，也是响应生产者的生命周期边界。

### 11.5 不变量五：每个 accepted send item 都必须非空

`handleSendCompletion()` 使用空 `QByteArray` 表示“没有下一项”：

```cpp
QByteArray nextBytes;

if (!nextBytes.isEmpty())
{
    // 创建下一条 send operation。
}
```

`isEmpty()` 没有参数；size 为 0 时返回 `true`。因此 current item 和 `sendQueue` 中的 waiting item 都必须是非空 bytes，否则空 waiting item 被取出后会被误判为“queue 已经没有下一项”。

这个前提由调用方保证，而不是由 `enqueueBytes()` 自己检查：

```text
enqueuePacket
  → serialize
  → bytes.isEmpty() 时不调用 enqueueBytes

直接调用 enqueueBytes 的文件与屏幕路径
  → 显式拒绝空 bytes，或保证批次至少含一个已成功序列化的 Packet
```

所以“成功进入发送状态的 item 非空”是理解当前实现不可缺少的调用契约。

---

## 12. 为什么每条连接只保留一个 send slot

需要先准确区分三个顺序：

| 顺序 | 谁提供 | 能否直接替代应用队列 |
| --- | --- | --- |
| 多次 `WSASend()` 调用进入 transport 的顺序 | Winsock | 不能解决多个 producer 谁先发起调用 |
| completion packet 进入 IOCP 的顺序 | Windows I/O system | packet 即使排队有序，也可能由 worker 以不同顺序取出和处理 |
| response 的业务 FIFO | 项目 `sendQueue` | 由应用明确维护 |

Winsock 文档说明，多次 `WSASend()` 的调用顺序也是 buffer 进入 transport 的顺序；同时明确警告，不应从多个线程并发调用同一 stream socket 的 `WSASend()`，因为 provider 可能拆分大型 send 并造成非预期 data interleaving。

所以问题不是“TCP 会不会保持 byte order”，而是：

```text
响应生产者 A 和 B 并发
  → 谁先真正调用 WSASend 不确定
  → 两个 operation completion 处理次序也更复杂
  → byte counter 和最终关闭很难保持单一负责人
```

项目选择更强、更容易证明的规则：

```text
每连接最多一个 send slot
  → 只有发送槽位的持有者可以 postSend
  → 其他响应生产者只写入 FIFO queue
  → 当前 item 全部完成后再交接下一项
```

好处：

- 业务 enqueue 顺序就是发送 item 顺序。
- 部分发送只影响当前 operation，不影响等待 queue。
- 每个 completion 只需要处理一个明确的 current item。
- byte counter 的增加和扣减都有唯一位置。
- final close 只需等待 queue drain。

IOCP 负责高效通知“某个 operation 完成”，不负责定义每条 connection 的业务发送协议。

---

## 13. `sendMutex` 怎样保护一组状态

项目在 enqueue、completion、final-close request 和 close cleanup 中使用同一把 `sendMutex`。

典型写法：

```cpp
{
    std::lock_guard<std::mutex> const lock{connection->sendMutex};
    // 读取或修改 sendQueue、queuedSendBytes、sendPending、closeAfterSend。
}
```

### 13.1 `lock_guard` constructor 参数

构造语义与第 10.3 节相同：唯一参数是要锁住的 mutex reference。这里传入 `connection->sendMutex`，因此当前 scope 获得的是发送状态的访问权；离开 scope 时 destructor 自动解锁。

### 13.2 为什么四个字段必须在同一个 critical section 修改

错误拆分：

```text
先无锁 queuedSendBytes += size
稍后再锁 sendPending
最后单独 push queue
```

其他线程可能在中间看到：

```text
queuedSendBytes 已增加
sendPending 仍为 false
queue 仍为空
```

这个瞬间不满足任何合法发送状态。

正确做法：

```text
锁住 sendMutex
  → 检查容量
  → 更新 byte counter
  → 决定成为首项还是等待项
  → 更新 sendPending 或 sendQueue
解锁 sendMutex
```

### 13.3 在锁内决定动作，在锁外执行动作

`enqueueBytes()` 在锁内只决定：

```text
当前 bytes 是首项还是等待项
```

真正的 `postSend()` 在解锁后调用。

`handleSendCompletion()` 在锁内只决定：

```text
下一项 bytes 是什么
发送链排空后是否关闭
```

真正的 `postSend()` 或 `closeConnection()` 也在解锁后执行。

这样做的原因：

- 缩短 producer 等待 `sendMutex` 的时间。
- 避免持有 send-state lock 时进入 native I/O submission。
- `postSend()` 内部还会取得 `socketMutex`，分离 lock scope 可以降低 lock-order 风险。
- `closeConnection()` 会访问更多连接状态，不应在 send critical section 内递归进入。

### 13.4 三把 mutex 不可混用

| mutex | 保护对象 | 本阶段是否深入 |
| --- | --- | --- |
| `ConnectionRegistry::m_mutex` | active map 和 stream quota counters | 是 |
| `ConnectionContext::sendMutex` | queue、byte counter、pending、final intent | 是 |
| `ConnectionContext::socketMutex` | native I/O submission 与 socket close 的互斥 | 阶段九展开 |

一把 mutex 只保护其约定的数据。拿到 `sendMutex` 不表示可以安全关闭 socket。

---

## 14. 容量准入与 `enqueueBytes()`

项目有两个发送上限：

```cpp
constexpr std::size_t MaximumQueuedSendBytes{
    2U * 1024U * 1024U};

constexpr std::size_t MaximumSingleSendBytes{
    16U * 1024U * 1024U};
```

| 上限 | 值 | 作用 |
| --- | ---: | --- |
| `MaximumQueuedSendBytes` | 2 MiB | 已有 send 占用 slot 时，限制当前连接的总 retained bytes。 |
| `MaximumSingleSendBytes` | 16 MiB | 限制任何单个 send item 的最大 serialized size。 |

### 14.1 `hasSendCapacity()`

项目实现：

```cpp
bool hasSendCapacity(
    std::size_t _queuedBytes,
    std::size_t _additionalBytes) noexcept
{
    if (_additionalBytes > MaximumSingleSendBytes)
    {
        return false;
    }
    if (_queuedBytes == 0)
    {
        return true;
    }
    return _additionalBytes <= MaximumQueuedSendBytes &&
        _queuedBytes <= MaximumQueuedSendBytes - _additionalBytes;
}
```

两个参数：

| 参数 | 作用 |
| --- | --- |
| `_queuedBytes` | 当前连接已经保留的在途与等待 byte 总数。 |
| `_additionalBytes` | 本次 producer 希望新增的完整 item byte 数。 |

返回值：

- `true`：本次 item 可以进入连接发送状态。
- `false`：单 item 过大，或已有 backlog 加上新 item 会超过队列上限。

### 14.2 first-send exception

当 `_queuedBytes == 0`：

```text
只检查 additional <= 16 MiB
```

所以一个 12 MiB 的 first item 可以直接占用 send slot，即使它大于 2 MiB queue limit。

一旦已有 retained bytes：

```text
queued + additional <= 2 MiB
```

这表示设计允许“一个较大的独立 send”，但不允许在它后面继续积累 backlog。

### 14.3 为什么不直接相加

直观写法：

```cpp
_queuedBytes + _additionalBytes <= MaximumQueuedSendBytes
```

如果两个无符号整数相加 overflow，结果会 wrap，错误地通过检查。

项目先保证：

```text
additional <= maximum
```

再检查：

```text
queued <= maximum - additional
```

减法不会 underflow，等价地证明总和不超过 maximum。

### 14.4 容量例子

1. **当前 retained 为 0，新增 12 MiB**
   - 结果：接收。
   - 原因：first item 小于 16 MiB。
2. **当前 retained 为 0，新增 16 MiB + 1 byte**
   - 结果：拒绝。
   - 原因：单 item 超限。
3. **当前 retained 为 1 MiB，新增 1 MiB**
   - 结果：接收。
   - 原因：总量正好为 2 MiB。
4. **当前 retained 为 1 MiB，新增 1 MiB + 1 byte**
   - 结果：拒绝。
   - 原因：总量超过 2 MiB。
5. **当前 retained 为 12 MiB，新增 1 byte**
   - 结果：拒绝。
   - 原因：已有大 first item 时不允许新增 backlog。

### 14.5 `enqueueBytes()` 的两个参数

声明：

```cpp
bool enqueueBytes(
    std::shared_ptr<ConnectionContext> const& _connection,
    QByteArray const& _bytes);
```

| 参数 | 作用 |
| --- | --- |
| `_connection` | 非空 connection；提供 terminal state、send mutex、发送字段和目标 socket。函数会直接解引用它，因此调用方必须保证有效。 |
| `_bytes` | producer 已经构造好的非空完整 serialized item。函数不会把它解释成半个 Packet。 |

当前实现不会在入口检查 `_connection == nullptr`、`_bytes.isEmpty()` 或 `closeAfterSend`。调用方必须同时保证：connection 有效、item 非空，并且记录最终关闭意图后不再调用这个入口。

聚焦状态变化的实现：

```cpp
bool RemoteControlTransport::Impl::enqueueBytes(
    std::shared_ptr<ConnectionContext> const& _connection,
    QByteArray const& _bytes)
{
    QByteArray firstBytes;
    std::size_t const bytesSize{
        static_cast<std::size_t>(_bytes.size())};
    {
        std::lock_guard<std::mutex> const lock{
            _connection->sendMutex};

        if (this->m_stopping.load() ||
            _connection->state.isTerminal())
        {
            return false;
        }

        if (!hasSendCapacity(
                _connection->queuedSendBytes,
                bytesSize))
        {
            return false;
        }

        _connection->queuedSendBytes += bytesSize;
        if (_connection->sendPending)
        {
            _connection->sendQueue.push_back(_bytes);
            return true;
        }

        _connection->sendPending = true;
        firstBytes = _bytes;
    }

    return this->postSend(
        std::make_unique<IoOperation>(
            _connection,
            std::move(firstBytes)));
}
```

项目原函数在容量拒绝时还会写 structured log；上面只省略日志，不改变分支。

### 14.6 首项与等待项两条分支

容量检查通过并增加 `queuedSendBytes` 后，只按 `sendPending` 选择一条分支：

| 锁内观察 | 锁内动作 | 解锁后的动作 |
| --- | --- | --- |
| `sendPending == false` | 设为 `true`，把 `_bytes` 保存到 `firstBytes` | 创建 `IoOperation` 并调用 `postSend()` |
| `sendPending == true` | `sendQueue.push_back(_bytes)` | 直接返回，不提交第二个 send |

首项创建 operation 时：

```cpp
std::make_unique<IoOperation>(
    _connection,
    std::move(firstBytes))
```

| constructor 参数 | 作用 |
| --- | --- |
| `_connection` | 让 operation 持有当前连接强引用。 |
| `std::move(firstBytes)` | 把完整首项的 byte storage 转移给 operation。 |

等待项调用 `push_back()`；它的唯一参数 `_bytes` 是追加到 deque 尾部的非空 item。等待项不调用 `postSend()`，只能由当前 send completion 交接。

---

## 15. 三个 response 的 FIFO 推演

假设：

```text
A = 100 bytes
B = 200 bytes
C = 300 bytes
closeAfterSend = false
```

### 15.1 入队过程

1. **初始**
   - 当前发送项：无。
   - `sendQueue`：`[]`。
   - `queuedSendBytes`：`0`。
   - `sendPending`：`false`。
2. **enqueue A**
   - 当前发送项：A。
   - `sendQueue`：`[]`。
   - `queuedSendBytes`：`100`。
   - `sendPending`：`true`。
3. **enqueue B**
   - 当前发送项：A。
   - `sendQueue`：`[B]`。
   - `queuedSendBytes`：`300`。
   - `sendPending`：`true`。
4. **enqueue C**
   - 当前发送项：A。
   - `sendQueue`：`[B, C]`。
   - `queuedSendBytes`：`600`。
   - `sendPending`：`true`。

关键点：

- A 不在 queue 中，它由 current `IoOperation` 持有。
- B 先于 C 调用 `push_back()`，所以 `[B, C]` 保存业务 FIFO。
- `queuedSendBytes = 100 + 200 + 300`，不是 queue 两项的 500。
- B 和 C 的 producer 返回后，不会产生第二或第三个在途 `WSASend()`。

FIFO 保证的是“成功进入 send critical section 并完成 admission 的顺序”。如果两个 producer 真正同时到达，谁先取得 `sendMutex`，谁先进入 queue；queue 不会自动推断额外的业务优先级。

### 15.2 A 完成后的交接

```text
A 全部完成
  → queuedSendBytes -= 100
  → 从 queue 头部取得 B
  → queue 变为 [C]
  → sendPending 继续保持 true
  → B 成为新 operation
```

此时状态为：

- 当前发送项：B。
- `sendQueue`：`[C]`。
- `queuedSendBytes`：`500`。
- `sendPending`：`true`。

### 15.3 B、C 完成

1. **B 完成**
   - 完成后当前项：C。
   - `sendQueue`：`[]`。
   - `queuedSendBytes`：`300`。
   - `sendPending`：`true`。
2. **C 完成**
   - 完成后当前项：无。
   - `sendQueue`：`[]`。
   - `queuedSendBytes`：`0`。
   - `sendPending`：`false`。

`sendPending` 在 A→B、B→C 交接时不会短暂变成 `false`。否则另一个 producer 可能误以为 send slot 空闲并并发提交新 operation。

### 15.4 `deque` 三个函数

| 函数 | 参数 | 作用 |
| --- | --- | --- |
| `push_back(_bytes)` | 一个 waiting item | 把新 response 放到队尾。 |
| `front()` | 无参数 | 返回队头第一个 waiting item 的引用；调用前必须确认非空。 |
| `pop_front()` | 无参数 | 移除已经转交给局部变量的队头 item。 |

项目取下一项：

```cpp
nextBytes = std::move(connection->sendQueue.front());
connection->sendQueue.pop_front();
```

`std::move()` 的唯一参数是 queue 头部表达式。它把该表达式转换为可移动值类别，真正的 byte ownership transfer 由 `nextBytes` 的 move assignment 完成。

顺序始终是：

```text
sendQueue 队头 → 局部 nextBytes → 下一条 IoOperation
```

---

## 16. `handleSendCompletion()` 怎样交接下一项

阶段六已经说明三个参数：

| 参数 | 本阶段关注点 |
| --- | --- |
| `_operation` | 当前 in-flight item 的唯一 operation ownership。 |
| `_success` | 当前 overlapped send 是否成功完成。 |
| `_transferredBytes` | 本次 completion 消耗当前 item 的多少 bytes。 |

进入 offset 和 queue 逻辑前，函数先经过 completion gate：

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

completion gate 依次处理四种情况：

1. `_success == false`：尝试以 `IoFailure` 进入统一关闭入口。
2. `_success` 为 `true`，但 `_transferredBytes == 0`：尝试以 `PeerDisconnected` 进入统一关闭入口。
3. connection 已 terminal：不再交接 queue；关闭调用会成为 loser，不覆盖已有 reason。
4. 成功、非零且非 terminal：此时才能推进 `sendOffset` 并处理 queue。

下面截取的是通过 gate 之后的关键实现：

```cpp
_operation->sendOffset +=
    static_cast<int>(_transferredBytes);

if (_operation->sendOffset < _operation->sendBytes.size())
{
    static_cast<void>(this->postSend(std::move(_operation)));
    return;
}

QByteArray nextBytes;
bool closeAfterSend{false};
{
    std::lock_guard<std::mutex> const lock{
        connection->sendMutex};

    std::size_t const completedSize{
        static_cast<std::size_t>(
            _operation->sendBytes.size())};

    connection->queuedSendBytes =
        completedSize <= connection->queuedSendBytes
        ? connection->queuedSendBytes - completedSize
        : 0;

    if (!connection->sendQueue.empty())
    {
        nextBytes = std::move(connection->sendQueue.front());
        connection->sendQueue.pop_front();
    }
    else
    {
        connection->sendPending = false;
        closeAfterSend = connection->closeAfterSend;
    }
}

if (!nextBytes.isEmpty())
{
    static_cast<void>(this->postSend(
        std::make_unique<IoOperation>(
            connection,
            std::move(nextBytes))));
    return;
}

if (closeAfterSend)
{
    this->closeConnection(
        connection,
        ConnectionCloseReason::RequestComplete);
    return;
}
```

`!nextBytes.isEmpty()` 能代表“确实取到了下一项”，依赖第 11.5 节的非空 item 契约。

### 16.1 部分完成不交接 queue

```text
A size = 100
本次 transferred = 40
sendOffset = 40
```

动作：

```text
同一个 A operation 重新 post 剩余 [40, 100)
sendQueue 仍是 [B, C]
queuedSendBytes 仍是 600
sendPending 仍是 true
```

为什么 byte counter 不减 40：

- operation 仍持有完整 A buffer。
- A 还没有完成，不能让 B 占用 send slot。
- counter 统计 retained memory，不是 transport 已确认的累计 byte 数。

`sendOffset` 的算术与 `WSABUF` 刷新已经在阶段四学习；阶段七新增的是它与 queue 状态必须保持隔离。

### 16.2 完整 item 才扣减

`completedSize` 是 `_operation->sendBytes.size()`，不是本次 `_transferredBytes`。

```text
A 最终完成
  → 一次性 queuedSendBytes -= A.size()
```

防御式表达式：

```cpp
completedSize <= queuedSendBytes
    ? queuedSendBytes - completedSize
    : 0
```

正常不变量下左侧应始终成立。右侧归零防止意外 underflow 扩大成极大的 `std::size_t`。

### 16.3 有下一项时保持 pending

如果 queue 非空：

```text
把队头移动到 nextBytes
pop_front
不修改 sendPending
解锁 sendMutex
提交 nextBytes
```

`sendPending` 从 A 到 B 始终保持 `true`，表示 send slot 连续交接而不是先释放再争抢。

### 16.4 queue 空时才释放 slot

```text
sendQueue.empty() == true
  → sendPending = false
  → 读取 closeAfterSend 快照
```

读取 `closeAfterSend` 与释放 slot 在同一个 critical section 中完成，后续关闭判断使用局部 snapshot，不需要继续持锁。

### 16.5 队列排空后的业务继续

当 queue 已 drain 且 `closeAfterSend == false` 时，项目才继续检查：

```text
ScreenStream → completeScreenFrame
FileTransfer → continueFileTransfer
```

这说明 send completion 可以反向驱动下一批业务生产。阶段七只建立“必须等 queue drain”的发送条件；具体截图和文件 producer 在阶段八学习。

---

## 17. `closeAfterSend` 与两种并发先后

项目函数：

```cpp
void RemoteControlTransport::Impl::requestCloseAfterSend(
    std::shared_ptr<ConnectionContext> const& _connection)
{
    bool closeNow{false};
    {
        std::lock_guard<std::mutex> const lock{
            _connection->sendMutex};
        _connection->closeAfterSend = true;
        closeNow = !_connection->sendPending &&
            _connection->sendQueue.empty();
    }

    if (closeNow)
    {
        this->closeConnection(
            _connection,
            ConnectionCloseReason::RequestComplete);
    }
}
```

唯一参数：

| 参数 | 作用 |
| --- | --- |
| `_connection` | 非空 connection；保存 final intent、send slot、waiting queue 和关闭入口。函数会直接解引用它。 |

函数没有返回值。它保证“记录 final intent”和“检查当前是否已 drain”在同一个 send critical section 中完成。

这个函数成立需要两个调用前提：

1. final response 必须先成功进入发送状态，再记录 final intent。
2. final intent 记录后，协议层不得再产生新的 response。

项目用 `sendFinalPacket()` 固定第一个顺序：

```cpp
if (this->enqueuePacket(_connection, _packet))
{
    this->requestCloseAfterSend(_connection);
}
else
{
    this->closeConnection(
        _connection,
        ConnectionCloseReason::Backpressure);
}
```

如果反过来先调用 `requestCloseAfterSend()`，而当前又没有 send，连接会立即开始关闭，随后 final packet 无法入队。

第二个前提是协议层契约：`enqueueBytes()` 没有检查 `closeAfterSend`。`sendMutex` 能保证 flag、slot 和 queue 的检查不丢失，但不能自动阻止一个逻辑上已经结束的 producer 再次调用 enqueue。

### 17.1 关闭请求先发生

```text
sendPending = true
sendQueue = []
closeAfterSend = false
```

`requestCloseAfterSend()`：

```text
closeAfterSend = true
closeNow = false
不关闭
```

final completion：

```text
sendPending = false
读取 closeAfterSend = true
closeConnection(RequestComplete)
```

### 17.2 completion 先发生

虽然 `sendFinalPacket()` 先调用 enqueue、下一行才设置关闭标志，但 enqueue 内的 `postSend()` 已经把 operation 交给系统。另一个 completion worker 可能在当前响应生产者继续执行下一行前，先处理这个 operation 的完成通知。

final completion 抢先：

```text
sendPending = false
sendQueue = []
当时 closeAfterSend = false
不关闭
```

随后 `requestCloseAfterSend()`：

```text
closeAfterSend = true
发现 pending == false 且 queue empty
closeNow = true
closeConnection(RequestComplete)
```

### 17.3 为什么两种顺序都不会丢失关闭

两个参与者都在 `sendMutex` 下检查同一组条件：

```text
已经记录“发送完后关闭”
  并且
当前没有发送槽位
  并且
发送队列没有等待项
```

在“最终 item 已先入队，并且记录最终关闭意图后没有新生产者”这两个前提下，最后一个使全部条件成立的线程负责调用 `closeConnection()`。

### 17.4 closing winner 仍是最后防线

send 路径决定应当关闭后，仍调用统一入口：

```cpp
closeConnection(
    connection,
    ConnectionCloseReason::RequestComplete);
```

如果 timeout 或 I/O failure 已经先把连接变为 `Closing`，`tryBeginClosing()` 会让当前 send worker 成为 loser 并直接返回。

所以：

```text
sendMutex
  → 保证“已经要求最终关闭”和“发送链已经排空”不会互相错过

state CAS
  → 防止多个关闭原因重复执行清理
```

两层保护解决的是不同问题。

---

## 18. backpressure：慢连接的资源边界

### 18.1 没有 backpressure 会发生什么

```text
响应生产者持续 enqueue
客户端持续慢读
  → current send 长时间不完成
  → sendQueue 中的等待项持续增长
  → 每项 QByteArray 都保留内存
```

单个恶意或故障客户端可能占用服务端大部分内存。

### 18.2 admission control 在复制进 queue 前完成

顺序：

```text
锁住 sendMutex
  → 读取 queuedSendBytes
  → hasSendCapacity
  ├─ false：拒绝当前 item
  └─ true：增加 counter，再决定成为首项还是等待项
```

拒绝发生在 `sendQueue.push_back()` 前，所以不合格 item 不会成为新的 waiting item。

这并不表示 producer 从未分配过内存：`_bytes` 在调用前已经构造。admission control 限制的是 transport 是否继续持有这项 bytes，而不是撤销序列化阶段已经发生的临时分配。

### 18.3 enqueue 失败怎样传播

典型调用：

```cpp
if (!this->enqueuePacket(_connection, response))
{
    this->closeConnection(
        _connection,
        ConnectionCloseReason::Backpressure);
}
```

`enqueuePacket()` 的两个参数在阶段六已经介绍：

| 参数 | 本阶段关注点 |
| --- | --- |
| `_connection` | 被执行 per-connection capacity check 的连接。 |
| `_packet` | 序列化后将作为一个完整 send item 计量。 |

容量拒绝后关闭当前慢连接，可以立即释放其 waiting queue，并阻止该连接继续生产。具体 socket cancel 和 pending operation 收尾在阶段九学习。

需要注意，`enqueueBytes()` 的 `false` 是组合失败信号，也可能来自 server stopping、terminal state 或 first `postSend()` 没有继续提交。

调用方尝试 `Backpressure` 关闭并不意味着最终 reason 一定是 `Backpressure`：如果其他路径已经赢得 `Closing`，这次关闭会成为 loser。只有容量分支还会写出 `connection.backpressure` structured log。

### 18.4 backpressure 不是“网络错误”

```text
TCP 可能仍然连接
WSASend 也可能没有返回 error
```

服务端仍主动关闭，因为资源策略已经拒绝继续承担该连接的 backlog。

backpressure 可以有暂停 producer、等待容量、丢弃低优先级数据等多种策略。本项目选择的是“拒绝当前 item，并关闭对应连接”，不是让调用线程阻塞等待 queue 变空。

它回答的是：

> 即使网络最终可能发送成功，服务端是否愿意继续保留这些 bytes？

### 18.5 本阶段的三种容量或完成结果

| close reason / 结果 | 保护的资源或语义 | 本阶段位置 |
| --- | --- | --- |
| `CapacityLimit` | total connection 或 persistent stream 数量 | registry quota |
| `Backpressure` | 单连接 retained send bytes | send admission control |
| `RequestComplete` | final send chain 已正常 drain | close-after-send |

不能把 `Backpressure` 仅理解成一个 error enum。它是内存上限、慢客户端隔离和服务公平性的共同结果。

阶段八还会出现 task queue 的 `TaskRejected`，本阶段不要求掌握它的触发过程。

### 18.6 项目中的慢客户端场景

集成测试会启动多个 slow download client，让它们不及时消费大响应，然后再发起普通目录请求。

预期：

```text
慢下载连接受到有界发送与 backpressure 约束
  + 文件 worker 不被无限 backlog 占住
  → 普通目录请求仍能得到服务
```

文件 batch 怎样限制 producer 速度属于阶段八；本阶段只需要掌握 send side 的最后资源防线。

---

## 19. 映射到项目源码

下面各组入口都相对于同一个源项目根目录：

> 源项目根目录：`D:\CodeRepository\claude\remote_control`

### 19.1 phase 与 CAS

1. `server_transport\internal\RemoteControlTransportImpl.h:66`
   - 查看 `ConnectionPhase` 的全部值。
2. `server_transport\internal\RemoteControlTransportImpl.h:107`
   - 查看 `ConnectionStateMachine` 接口和 atomic member。
3. `server_transport\src\RemoteControlTransportRuntime.cpp:75`
   - 查看 strong CAS 一次分类。
4. `server_transport\src\RemoteControlTransportRuntime.cpp:86`
   - 查看 weak CAS closing loop。
5. `server_transport\src\RemoteControlTransportRuntime.cpp:103`
   - 查看 `Closing → Closed`。
6. `tests\ConnectionStateMachineTests.cpp:44`
   - 查看非法重复分类和 16-thread closing contention。

### 19.2 registry 与 quota

1. `server_transport\internal\RemoteControlTransportImpl.h:276`
   - 查看 registry map、quota counter 和接口。
2. `server_transport\src\RemoteControlTransportRuntime.cpp:227`
   - 查看三个 capacity 参数。
3. `server_transport\src\RemoteControlTransportRuntime.cpp:251`
   - 查看 mutex 内 quota check、state CAS、counter update。
4. `server_transport\src\RemoteControlTransportRuntime.cpp:288`
   - 查看根据 previous phase 释放 quota。
5. `tests\ConnectionStateMachineTests.cpp:99`
   - 查看 total capacity、role quota 和 removal 测试。

### 19.3 有序发送与 backpressure

1. `server_transport\internal\RemoteControlTransportImpl.h:268`
   - 查看五个连接级发送字段。
2. `server_transport\src\RemoteControlTransport.cpp:14`
   - 查看 2 MiB backlog 与 16 MiB single-item 上限。
3. `server_transport\src\RemoteControlTransport.cpp:46`
   - 查看 overflow-safe capacity check。
4. `server_transport\src\RemoteControlTransport.cpp:663`
   - 查看 first item、waiting item 和 byte admission。
5. `server_transport\src\RemoteControlTransport.cpp:590`
   - 查看 partial send、完整扣减、FIFO 交接和 queue drain。
6. `server_transport\src\RemoteControlTransport.cpp:712`
   - 查看 final intent 与 completion race。
7. `server_transport\src\RemoteControlTransport.cpp:727`
   - 查看 closing winner 怎样阻止重复 cleanup。
8. `tests\SmokeTests.cpp:743`
   - 查看 slow download 与服务公平性场景。

### 19.4 protocol role 映射

1. `server_transport\src\RemoteControlTransportProtocol.cpp:86`
   - 查看首包 command 怎样选择 role。
2. `server_transport\src\RemoteControlTransportProtocol.cpp:32`
   - 查看分类后 Packet 怎样按 current phase 路由。

### 19.5 阅读时追踪五条线

```text
phase 变化：
AwaitingRequest → 固定业务角色 → Closing → Closed

bytes 所有权：
生产者创建 bytes → 当前 IoOperation 或 sendQueue → completion 后释放

counter 变化：
接收新 item 时增加 queuedSendBytes
完整完成一个 item 时扣减 queuedSendBytes

锁的职责：
registry mutex 保护角色 quota 事务
sendMutex 保护单连接发送事务

线程交接：
响应生产线程 → completion worker → 下一项生产或争夺关闭资格
```

---

## 20. 常见错误与失败入口

### 20.1 主要失败入口

| 位置 | 条件 | 结果 |
| --- | --- | --- |
| state classification | target 不是四个业务角色 | `tryClassify()` 返回 `false` |
| state classification | phase 已不是 `AwaitingRequest` | CAS 失败，不覆盖现有角色 |
| registry classification | connection 已移除 | 返回 `false` |
| registry quota | screen/control quota 已满 | 分类失败，phase 保持不变 |
| send admission | connection terminal 或 server stopping | enqueue 返回 `false` |
| send item precondition | producer 传入空 bytes | 违反调用契约，可能破坏下一项 sentinel 语义 |
| send admission | single item 或 backlog 超限 | enqueue 返回 `false`、记录 backpressure log，调用方尝试以 `Backpressure` 关闭 |
| send completion | `_success == false` | `IoFailure` 关闭竞争 |
| send completion | `_transferredBytes == 0` | `PeerDisconnected` 关闭竞争 |
| final queue drain | `closeAfterSend == true` | `RequestComplete` 关闭竞争 |
| any close path | 已有线程进入 `Closing` | 当前调用成为 loser，直接返回 |

### 20.2 常见错误

| 错误 | 直接后果 | 根因 |
| --- | --- | --- |
| 用 `phase()` 的 load-then-store 代替 CAS | 两个线程都认为自己分类成功 | 条件检查和写入不是原子操作 |
| 允许角色之间互相转换 | 同一 TCP stream 被不同协议解释 | classification 应不可变 |
| 把 CAS 的 expected 当作纯输入参数 | 失败后继续使用过期预期值 | compare-exchange 会回写实际值 |
| 单次调用 weak CAS，不放入 loop | 合法关闭可能因 spurious failure 丢失 | weak 版本允许伪失败 |
| closing loser 继续清理 | quota 重复递减、socket 重复关闭 | 忽略 `tryBeginClosing()` 返回值 |
| winner 不保存 previous phase | registry 无法释放正确角色 quota | 当前 phase 已经变成 `Closing` |
| quota check 后先解锁，再做 state CAS | 多连接可能同时超出 quota | check、CAS、counter update 不是事务 |
| 认为 atomic phase 能保护 registry counter | counter 出现 data race | atomic 只保护自己的对象 |
| 把 current item 也放入 `sendQueue` | completion 交接语义混乱 | 没区分 operation ownership 与 waiting ownership |
| `queuedSendBytes` 只统计 queue | 容量低估当前 retained memory | 忽略 in-flight item |
| 部分 completion 就扣减 transferred bytes | counter 与完整 buffer ownership 不一致 | operation 仍持有完整 item |
| pop 下一项前把 `sendPending` 改成 false | 新 producer 可能并发占用 slot | A→B 交接中出现虚假空闲窗口 |
| waiting producer 也调用 `postSend()` | 同一 socket 出现多个并发 send | 没有遵守 slot owner 规则 |
| 持有 `sendMutex` 调用 `postSend()` | critical section 过长并引入 lock-order 风险 | 没有分离“决定动作”和“执行动作” |
| 把空 bytes 传给 `enqueueBytes()` | 空 waiting item 被误判为“没有下一项” | 忽略 completion 使用空 `QByteArray` 作为 sentinel |
| 在 `closeAfterSend` 后继续 enqueue | final drain 可能关闭新 response | 误以为 enqueue 入口会自动检查 final intent |
| 把 enqueue 的任意 `false` 都认定为容量超限 | 错判 stopping、terminal 或 submission failure | 返回值是组合失败信号，应结合 winner reason 与 backpressure log |
| 认为 2 MiB 是所有 single item 的绝对上限 | 错误拒绝合法 12 MiB first item | 忽略 16 MiB single-send limit 和 first-send branch |
| 直接计算 `queued + additional` | `size_t` overflow 后可能错误通过 | 没有使用 subtract-before-compare |
| 把 IOCP queue 当成业务 send queue | 无法维护每连接 FIFO、容量和 final close | completion dispatch 与业务顺序职责不同 |
| 认为 successful `WSASend` completion 表示客户端已收到 | 过早确认业务成功 | completion 只表示 transport 已消费 buffer，不保证远端 delivery |
| 把 `Backpressure` 当作普通网络 error | 失去慢连接隔离策略 | 它是服务端主动资源决策 |

---

## 21. 阶段练习与验收

按顺序完成。前四个任务建立状态和发送不变量，后面再加入部分完成、容量拒绝和关闭竞争。

### 21.1 任务一：完成合法状态转换表

**练习**

判断下面每个转换是“允许”还是“拒绝”，并指出应调用哪个状态函数：

1. `AwaitingRequest → OneShot`。
2. `AwaitingRequest → ScreenStream`。
3. `AwaitingRequest → Closing`。
4. `AwaitingRequest → Closed`。
5. `OneShot → ControlStream`。
6. `FileTransfer → Closing`。
7. `ScreenStream → Closing`。
8. `Closing → Closed`。
9. `Closed → Closing`。

然后回答：

1. `tryClassify()` 为什么不接受 `AwaitingRequest` 作为 target？
2. 为什么业务 role 之间没有直接边？
3. `Closing` 和 `Closed` 为什么都属于 terminal？

**验收标准**

- [ ] 只允许初始 phase 分类为四个业务角色。
- [ ] 任意非 terminal phase 都可以争夺 `Closing`。
- [ ] 只有 `Closing → Closed` 使用 `markClosed()`。
- [ ] 不允许 role 重新分类。
- [ ] 不允许 `Closed` 再进入任何活动状态。

**参考答案与解释**

1. `AwaitingRequest → OneShot`：允许，调用 `tryClassify(OneShot)`。
2. `AwaitingRequest → ScreenStream`：允许，调用 `tryClassify(ScreenStream)`。
3. `AwaitingRequest → Closing`：允许，调用 `tryBeginClosing(...)`。
4. `AwaitingRequest → Closed`：拒绝，必须先进入 `Closing`。
5. `OneShot → ControlStream`：拒绝，role 不可变。
6. `FileTransfer → Closing`：允许，调用 `tryBeginClosing(...)`。
7. `ScreenStream → Closing`：允许，调用 `tryBeginClosing(...)`。
8. `Closing → Closed`：允许，调用 `markClosed()`。
9. `Closed → Closing`：拒绝，已经是最终 phase。

`AwaitingRequest` 是初始等待状态，不是首包选择的业务角色。角色不可变可以保证后续 Packet 始终按同一协议解释。

### 21.2 任务二：推演 strong CAS 与 closing contention

**练习**

场景 A：

```text
初始 m_phase = AwaitingRequest
线程 A 的目标 = ScreenStream
线程 B 的目标 = ControlStream
```

假设 A 先 CAS 成功，分别写出线程 A、B 的以下状态：

- CAS 前 `expected`。
- CAS 时实际 phase。
- 返回值。
- CAS 后 `expected`。
- 最终 phase。

场景 B：

```text
当前 phase = ScreenStream
16 个线程同时调用 tryBeginClosing(&previousPhase)
```

回答：

1. `compare_exchange_weak()` 的两个参数分别是什么？
2. 为什么它必须放在 loop 中？
3. 几个线程返回 `true`？
4. 几个线程得到有效的 previous phase？
5. loser 为什么不能继续写 close reason？

**验收标准**

- [ ] A 把 `AwaitingRequest` 改为 `ScreenStream`。
- [ ] B 的 expected 被回写为 `ScreenStream`。
- [ ] 能区分 strong 无 spurious failure 与 weak 可 spurious failure。
- [ ] 16 个 contender 中只有一个 winner。
- [ ] 只有 winner 得到 `previousPhase = ScreenStream`。
- [ ] loser 在资源清理前返回。

**参考答案与解释**

1. **线程 A**
   - CAS 前 `expected`：`AwaitingRequest`。
   - 实际 phase：`AwaitingRequest`。
   - 返回值：`true`。
   - CAS 后 `expected`：`AwaitingRequest`。
   - 最终 phase：`ScreenStream`。
2. **线程 B**
   - CAS 前 `expected`：`AwaitingRequest`。
   - 实际 phase：`ScreenStream`。
   - 返回值：`false`。
   - CAS 后 `expected`：`ScreenStream`。
   - 最终 phase：`ScreenStream`。

closing contention：

```text
1 个胜出者：ScreenStream → Closing，返回 true
15 个失败者：观察到 Closing，返回 false
```

weak CAS 可能因竞争或 spurious failure 返回 false。loop 会用更新后的 `current` 重试，直到某线程成功，或发现已经 terminal。

### 21.3 任务三：推演 registry quota 事务

**练习**

使用项目测试配置：

```cpp
ConnectionRegistry registry{4, 1, 1};
```

constructor 三个参数分别表示：

```text
最大连接数 = 4
最大 ScreenStream 数 = 1
最大 ControlStream 数 = 1
```

依次执行：

1. add 连接 1、2、3、4。
2. add 连接 5。
3. 连接 1 分类为 `ScreenStream`。
4. 连接 2 也尝试 `ScreenStream`。
5. 连接 2 随后尝试 `OneShot`。
6. 连接 3 分类为 `ControlStream`。
7. 连接 1 进入 `Closing` 并从 registry remove。
8. 连接 4 尝试 `ScreenStream`。

填写每一步的结果、connection phase、screen/control counter 和 registry size。

**验收标准**

- [ ] 前四个 add 成功，第五个因 total capacity 失败。
- [ ] 连接 1 占用唯一 ScreenStream quota。
- [ ] 连接 2 的 ScreenStream 尝试失败后仍是 `AwaitingRequest`。
- [ ] 连接 2 仍可成功分类为 `OneShot`。
- [ ] 移除连接 1 后释放 ScreenStream quota。
- [ ] 连接 4 随后可成为 `ScreenStream`。
- [ ] 能解释为什么 quota check、state CAS、counter update 必须共用 registry mutex。

**参考答案与解释**

| 步骤 | 结果 | 关键状态 |
| --- | --- | --- |
| add 1～4 | 全部成功 | size = 4 |
| add 5 | 失败 | size 仍为 4 |
| 1 → Screen | 成功 | screen count = 1 |
| 2 → Screen | 失败 | 连接 2 仍为 AwaitingRequest |
| 2 → OneShot | 成功 | OneShot 不占 stream quota |
| 3 → Control | 成功 | control count = 1 |
| remove 1 | 成功 | size = 3，screen count = 0 |
| 4 → Screen | 成功 | screen count = 1 |

quota attempt 在 state CAS 前失败，所以不会留下半完成分类。remove 使用 winner 保存的 `previousPhase` 释放正确 counter。

### 21.4 任务四：判断发送状态是否满足不变量

**练习**

判断下列 snapshot 是否可能是正常稳定状态，并说明原因：

1. **Snapshot A**：当前项无，queue 为 `[]`，bytes 为 `0`，pending 为 `false`。
2. **Snapshot B**：当前项 A 100 bytes，queue 为 `[B 200]`，bytes 为 `300`，pending 为 `true`。
3. **Snapshot C**：当前项无，queue 为 `[B 200]`，bytes 为 `200`，pending 为 `false`。
4. **Snapshot D**：当前项 A 100 bytes，queue 为 `[]`，bytes 为 `0`，pending 为 `true`。
5. **Snapshot E**：A 已部分完成 40/100，queue 为 `[B 200]`，bytes 为 `260`，pending 为 `true`。
6. **Snapshot F**：A 已部分完成 40/100，queue 为 `[B 200]`，bytes 为 `300`，pending 为 `true`。
7. **Snapshot G**：当前项 A 100 bytes，queue 为 `[空 QByteArray]`，bytes 为 `100`，pending 为 `true`。

然后回答：

1. 哪四个字段必须由 `sendMutex` 共同保护？
2. current A 存在哪里？
3. 为什么 partial A 仍按 100 bytes 计数？
4. `sendPending = true` 最准确的含义是什么？
5. 为什么 queue item 不能为空？

**验收标准**

- [ ] 能区分 current operation 和 waiting queue。
- [ ] queue 非空时不会接受 `pending = false`。
- [ ] counter 包含 current 的完整 bytes。
- [ ] partial completion 不提前减少 counter。
- [ ] 知道 `closeAfterSend` 也属于同一 critical section。
- [ ] 知道空 `QByteArray` 被用作“没有下一项”的 sentinel。

**参考答案与解释**

| snapshot | 结果 | 原因 |
| --- | --- | --- |
| A | 合法 | 完全空闲。 |
| B | 合法 | A 在途、B 等待，总量 300。 |
| C | 非法 | queue 非空却没有 send slot owner。 |
| D | 非法 | A 的 100 bytes 没有计入 counter。 |
| E | 非法 | A buffer 仍完整保留，不能只按剩余 60 计数。 |
| F | 合法 | retained memory 为完整 A 100 + B 200。 |
| G | 非法 | completion 会把空 item move 出 queue，却因 `isEmpty()` 不提交下一项，send slot 可能一直保持占用。 |

`sendPending` 表示当前连接的唯一 send slot 已被发送链占用，可能正在准备提交，也可能已有 operation 在途。

### 21.5 任务五：区分产生顺序与实际入队顺序

**练习**

已知三个非空响应：

```text
A = 100 bytes
B = 200 bytes
C = 300 bytes
```

事件顺序：

```text
1. A 先取得 send slot。
2. B 比 C 更早创建完成，但还没有取得 sendMutex。
3. C 先取得 sendMutex 并成功入队。
4. B 随后取得 sendMutex 并成功入队。
5. A、C、B 依次完成。
```

依次写出以下时刻的当前发送项、`sendQueue`、`queuedSendBytes` 和 `sendPending`：

1. 初始。
2. enqueue A。
3. enqueue C。
4. enqueue B。
5. A 全部完成。
6. C 全部完成。
7. B 全部完成。

额外回答：

1. B 虽然先创建完成，为什么发送顺序仍是 A→C→B？
2. 这里的 FIFO 以哪个时刻作为排序点？
3. A→C 交接时为什么不能把 `sendPending` 短暂设为 false？
4. `front()` 和 `pop_front()` 是否有参数？

**验收标准**

- [ ] queue 顺序是 `[C, B]`，而不是 `[B, C]`。
- [ ] counter 入队时依次为 100、400、600。
- [ ] 完成时依次变为 500、200、0。
- [ ] 只有最终 B 完成后 pending 才变为 false。
- [ ] 能解释 queue head 到 next operation 的 ownership transfer。

**参考答案与解释**

1. **初始**：当前项无，queue 为 `[]`，bytes 为 `0`，pending 为 `false`。
2. **enqueue A**：当前项 A，queue 为 `[]`，bytes 为 `100`，pending 为 `true`。
3. **enqueue C**：当前项 A，queue 为 `[C]`，bytes 为 `400`，pending 为 `true`。
4. **enqueue B**：当前项 A，queue 为 `[C, B]`，bytes 为 `600`，pending 为 `true`。
5. **A 完成**：当前项 C，queue 为 `[B]`，bytes 为 `500`，pending 为 `true`。
6. **C 完成**：当前项 B，queue 为 `[]`，bytes 为 `200`，pending 为 `true`。
7. **B 完成**：当前项无，queue 为 `[]`，bytes 为 `0`，pending 为 `false`。

FIFO 保证的是成功进入 `sendMutex` 临界区并完成准入的顺序，不是响应创建完成的先后。`front()` 和 `pop_front()` 都没有参数；先 move `front()`，再 `pop_front()`，把最早准入的等待项转交给下一条 operation。

### 21.6 任务六：组合部分发送、队列排空与最终关闭

**练习**

这是一个合法的发送状态组合题，不绑定某个具体 command；只考查部分发送、队列交接和最终关闭怎样共同工作。

初始：

```text
当前发送项 A = 100 bytes，sendOffset = 0
sendQueue = [B 200, C 300]
queuedSendBytes = 600
sendPending = true
closeAfterSend = true
```

依次发生：

1. A completion 只完成 40 bytes。
2. A 下一次 completion 完成剩余 60 bytes。
3. B 全部完成。
4. C 全部完成。

每一步填写：

```text
当前发送项
sendOffset
sendQueue
queuedSendBytes
sendPending
是否关闭
```

然后单独推演只有一个最终 item 时的两种顺序：

```text
路径一：requestCloseAfterSend 先，completion 后
路径二：completion 先，requestCloseAfterSend 后
```

再回答：

1. 为什么必须先成功 enqueue 最终 item，再调用 `requestCloseAfterSend()`？
2. 为什么设置 `closeAfterSend` 后不能再产生 response？
3. `enqueueBytes()` 是否会替协议层自动拒绝最终关闭意图之后的新响应？

**验收标准**

- [ ] A 的 partial completion 不弹出 B。
- [ ] partial completion 后 counter 仍为 600。
- [ ] A 全部完成后 counter 才变为 500。
- [ ] B、C 保持 FIFO。
- [ ] close intent 不会跳过 waiting queue。
- [ ] 两种 final-item 先后都能触发一次 `RequestComplete`。
- [ ] 能说明 send mutex 与 state CAS 分别防止什么竞争。
- [ ] 能说明“最终 item 先入队”和“之后没有新生产者”两个调用前提。

**参考答案与解释**

1. **A 完成 40 bytes**
   - 当前项：A；`sendOffset`：`40`。
   - queue：`[B, C]`；bytes：`600`；pending：`true`。
   - 是否关闭：否。
2. **A 再完成 60 bytes**
   - 当前项：B；`sendOffset`：`0`。
   - queue：`[C]`；bytes：`500`；pending：`true`。
   - 是否关闭：否。
3. **B 完成**
   - 当前项：C；`sendOffset`：`0`。
   - queue：`[]`；bytes：`300`；pending：`true`。
   - 是否关闭：否。
4. **C 完成**
   - 当前项：无；`sendOffset`：不适用。
   - queue：`[]`；bytes：`0`；pending：`false`。
   - 是否关闭：是，以 `RequestComplete` 关闭。

两种先后：

```text
关闭请求先发生：
flag = true → completion 释放发送槽位 → completion 负责关闭

completion 先发生：
发送槽位已空但 flag = false → request 设置 flag → request 负责关闭
```

`sendMutex` 防止 close condition 丢失；state CAS 防止其他错误或 timeout 同时重复执行 cleanup。

最终 bytes 必须先由 operation 或 queue 持有，随后才能设置最终关闭意图；否则空闲连接会被立即关闭。设置后，响应生产者必须在协议层停止，因为 `enqueueBytes()` 本身没有检查 `closeAfterSend`。

### 21.7 任务七：计算 capacity 与 backpressure

**练习**

已知：

```text
MaximumQueuedSendBytes = 2 MiB
MaximumSingleSendBytes = 16 MiB
```

判断下面每次调用 `hasSendCapacity(queued, additional)` 的返回值，并说明原因：

1. retained 为 0，新增 2 MiB + 1 byte。
2. retained 为 0，新增 16 MiB。
3. retained 为 0，新增 16 MiB + 1 byte。
4. retained 为 256 KiB，新增 1792 KiB。
5. retained 为 256 KiB，新增 1792 KiB + 1 byte。
6. retained 为 2 MiB，新增 1 byte。
7. retained 为 12 MiB，新增 1 byte。

回答：

1. 为什么 2 MiB + 1 byte 的 first item 仍可接收？
2. 为什么已有 12 MiB item 时不能再加入 1 byte？
3. 为什么检查使用 `queued <= max - additional`？
4. 容量拒绝为什么通常关闭为 `Backpressure`？
5. `Backpressure` 与 `CapacityLimit` 有什么区别？

**验收标准**

- [ ] 单 item 超过 16 MiB 必须拒绝。
- [ ] first item 在 16 MiB 内可以直接接收。
- [ ] 已有 retained bytes 后，总量不得超过 2 MiB。
- [ ] 能解释 subtract-before-compare 防 overflow。
- [ ] 能把 backpressure 解释为资源准入策略。
- [ ] 不把它与 total connection quota 混淆。

**参考答案与解释**

1. **0 + 2 MiB + 1 byte**：返回 `true`；first item 只受 16 MiB 上限约束。
2. **0 + 16 MiB**：返回 `true`；正好达到 single-item 上限。
3. **0 + 16 MiB + 1 byte**：返回 `false`；single item 超限。
4. **256 KiB + 1792 KiB**：返回 `true`；总量正好为 2 MiB。
5. **256 KiB + 1792 KiB + 1 byte**：返回 `false`；backlog 总量超过 2 MiB。
6. **2 MiB + 1 byte**：返回 `false`；已有积压后不能超过 2 MiB。
7. **12 MiB + 1 byte**：返回 `false`；已有大 first item 时不允许新 backlog。

`MaximumQueuedSendBytes` 限制已有 send 时的累计 retained bytes；`MaximumSingleSendBytes` 允许一个独立的大 item。使用减法比较避免无符号加法 wrap。

### 21.8 最终综合验收：两个真实边界场景

**练习**

场景一只验证持久连接的有序发送，不使用阶段八任务池：

```text
连接 1 的首包是 ControlChannel
  → registry 中 ControlStream quota 仍有空位
  → 分类为 ControlStream
  → 首包确认响应 A = 100 bytes，占用发送槽位

A 尚未完成时，同一 receive 处理链又解析出两个有效 Packet
  → 响应 B = 200 bytes
  → 响应 C = 300 bytes
  → B、C 依次调用 enqueue

A 第一次完成 40 bytes
A 第二次完成剩余 60 bytes
B、C 随后全部完成
本场景没有“发送后关闭”请求
```

场景二复用阶段六的单次请求，只验证 final intent 与 closing winner：

```text
连接 2 的首包是 TestConnection
  → 分类为 OneShot
  → sendFinalPacket 产生最终响应 D

requestCloseAfterSend 与 D completion 可能先后到达
超时线程也可能调用 closeConnection(IdleTimeout)
```

复述时必须指出：

1. 两个连接分别分类成什么 role，CAS 的 expected 和 desired 是什么？
2. 哪次分类增加 control quota，哪次不增加 stream quota？
3. A、B、C 分别由谁拥有，每次 enqueue 后 counter 是多少？
4. A partial completion 后哪些状态保持不变？
5. A→B→C 怎样交接，C 完成并排空队列后，连接 1 为什么保持打开？
6. `sendFinalPacket()` 为什么必须先 enqueue D，再设置 final intent？
7. D completion 与 `requestCloseAfterSend()` 两种先后分别由谁发起关闭？
8. `RequestComplete` 与 `IdleTimeout` 竞争时，哪个 reason 最终保留？
9. 为什么两个场景都不能依赖某个固定 completion worker？
10. 哪些实现细节仍属于阶段八和阶段九？

**验收标准**

- [ ] 连接 1 执行 `AwaitingRequest → ControlStream`，并占用一个 ControlStream quota。
- [ ] 当前发送项 A 不在 `sendQueue`，B、C 按 `[B, C]` 等待。
- [ ] counter 依次为 100、300、600，A partial completion 后仍为 600。
- [ ] A、B、C 按 FIFO 交接，只有 C 完成后 slot 才释放。
- [ ] 连接 1 的 `closeAfterSend == false`，因此队列排空后继续保持 `ControlStream`。
- [ ] 连接 2 执行 `AwaitingRequest → OneShot`，不增加 stream quota。
- [ ] D 必须先成功入队，随后才能设置 `closeAfterSend`。
- [ ] D completion 和 close request 的两种先后都不会丢失正常关闭。
- [ ] `RequestComplete` 与 `IdleTimeout` 只产生一个 closing winner，winner reason 不被覆盖。
- [ ] 不提前使用业务任务池、socket cancel 或 pending-I/O drain 细节。

**参考答案与解释**

完整答案至少包含以下十层：

1. 连接 1 在 registry mutex 下确认 ControlStream quota，strong CAS 的 expected 为 `AwaitingRequest`、desired 为 `ControlStream`；成功后 control counter 加一。
2. A 首先把 `sendPending` 改为 `true` 并由 `IoOperation` 持有；B、C 进入 queue，counter 依次为 100、300、600。
3. A 只完成 40 时，仅把 `sendOffset` 改为 40；queue、counter 和 pending 均不变。
4. A 全部完成后扣减 100，从 queue front move B；B 完成后同样交接 C，交接期间 pending 始终为 `true`。
5. C 完成后 counter 为 0、queue 为空、pending 为 `false`；由于没有最终关闭意图，而且 role 是 `ControlStream`，连接继续等待后续 Packet。
6. 连接 2 的 strong CAS 把 `AwaitingRequest` 改成 `OneShot`；它不占用 screen/control quota。
7. `sendFinalPacket()` 先让 D 取得 operation 或 queue ownership，成功后才调用 `requestCloseAfterSend()`；final intent 后不得再生产 response。
8. close request 先到时由 D completion 在 drain 后关闭；D completion 先到时由后到的 close request 发现 slot 已空并关闭。
9. send 路径和 timeout 路径都调用统一 `closeConnection()`；先把 `OneShot` CAS 为 `Closing` 的线程保留自己的 reason，loser 不重复清理。
10. producer task 怎样生成文件批次或屏幕帧属于阶段八；winner 怎样 cancel、shutdown、close 并 drain completion 属于阶段九。

全部任务通过后，阶段七才算完成。

---

## 22. 下一阶段衔接

阶段七已经建立了一个有容量上限的通用发送消费者：

```text
生产者提交完整 bytes
  → 容量准入
  → 成为首项，或进入 FIFO 等待队列
  → 一个发送槽位顺序消费
  → 队列排空后继续业务，或执行最终关闭
```

但当前把响应生产者当成抽象来源，还没有回答：

```text
目录条目从哪里读取？
下载文件为什么不能一次性读入内存？
截图和 PNG 编码为什么不能放在 completion worker？
发送完成后怎样安全地产生下一批数据？
```

阶段七结论在阶段八中的用途：

| 阶段七已经掌握 | 阶段八继续扩展 |
| --- | --- |
| `FileTransfer`、`ScreenStream` 是固定角色 | 为不同角色选择不同 task pool 和响应生产者 |
| `enqueueBytes()` 是有界发送入口 | 业务 worker 只生产有限 batch 后立即返回 |
| `queuedSendBytes` 限制慢客户端积压 | 文件读取不会无限领先于网络发送 |
| 队列排空是稳定事件 | send completion 驱动下一批文件读取或下一帧流程 |
| 每连接一个发送槽位 | 屏幕帧、文件 batch 和 status Packet 不会并发写乱 |
| backpressure 可以拒绝慢连接 | 任务池和网络侧共同保持全局公平性 |

进入阶段八前，应能够准确回答：

> A 已占用同一连接的发送槽位后，B、C 为什么只能进入 `sendQueue`，并等待 completion 按 FIFO 逐项交接？

---

## 23. 官方资料与项目资料

阅读官方资料时重点核对 CAS 的 expected 回写、weak spurious failure、RAII mutex、deque FIFO、Winsock 并发发送警告和 IOCP completion dispatch。

- [Microsoft Learn：`<atomic>` functions](https://learn.microsoft.com/en-us/cpp/standard-library/atomic-functions?view=msvc-170)
- [Microsoft Learn：`atomic` structure](https://learn.microsoft.com/en-us/cpp/standard-library/atomic?view=msvc-170)
- [Microsoft Learn：`lock_guard` class](https://learn.microsoft.com/en-us/cpp/standard-library/lock-guard-class?view=msvc-170)
- [Microsoft Learn：`deque` class](https://learn.microsoft.com/en-us/cpp/standard-library/deque-class?view=msvc-170)
- [Microsoft Learn：`WSASend` function](https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-wsasend)
- [Microsoft Learn：Overlapped I/O and Event Objects](https://learn.microsoft.com/en-us/windows/win32/winsock/overlapped-i-o-and-event-objects-2)
- [Microsoft Learn：I/O Completion Ports](https://learn.microsoft.com/en-us/windows/win32/fileio/i-o-completion-ports)

以下项目资料路径相对于 `D:\CodeRepository\claude\remote_control`：

- `server_transport\internal\RemoteControlTransportImpl.h`
- `server_transport\src\RemoteControlTransportRuntime.cpp`
- `server_transport\src\RemoteControlTransport.cpp`
- `tests\ConnectionStateMachineTests.cpp`
- `tests\SmokeTests.cpp`
