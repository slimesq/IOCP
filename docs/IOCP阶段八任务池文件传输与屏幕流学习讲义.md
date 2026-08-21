# IOCP 阶段八：任务池、文件传输与屏幕流学习讲义

> 前置知识：阶段七已经建立连接角色、单一发送槽位、FIFO 发送队列和背压边界。
> 贯穿项目：`D:\CodeRepository\claude\remote_control`。
> 学习范围：completion worker 与业务任务池的职责边界、固定大小的有界任务池、增量文件传输、发送完成驱动的续传、屏幕帧请求合并、共享帧缓存，以及屏幕流与控制流的连接隔离。
> 取消同步文件 I/O、排空 IOCP completion、资源释放和安全停机属于阶段九。

## 1. 阶段八学习主线

阶段七已经解决了“响应怎样安全发送”：

```text
业务代码产生完整 bytes
  → enqueueBytes() 检查容量
  → 每条连接只占用一个发送槽位
  → 等待项按 FIFO 交接
  → 发送队列排空
```

阶段八继续回答“这些 bytes 应该由谁、在什么时候产生”。

如果直接在 completion worker 中读取大文件或截图，代码虽然能工作，却会破坏 IOCP 的共享调度能力：

```text
completion worker 取到一个网络完成通知
  → 开始读取大文件或执行 PNG 编码
  → 线程长时间不能再调用 GetQueuedCompletionStatus()
  → 其他连接已经完成的收发操作无人处理
```

正确主线是把网络推进和阻塞业务分开：

```text
接收完成：completion worker
  ① 解析 Packet
  ② 确定连接角色
  ③ 向对应任务池提交一个短任务
  ④ 立即返回 completion 循环

业务生产：普通任务 worker
  ⑤ 执行可能阻塞的文件、Shell 或截图操作
  ⑥ 每次只生产有限数据
  ⑦ 把完整 bytes 交给 enqueueBytes()
  ⑧ 立即返回任务池

网络消费：completion worker
  ⑨ 处理 WSASend 完成通知
  ⑩ 当前发送项和等待队列都排空后，决定是否提交下一批业务任务
```

这里形成两个有界队列：

| 队列 | 保存什么 | 上限解决什么问题 |
| --- | --- | --- |
| 任务池等待队列 | 尚未被业务 worker 取走的函数对象 | 防止阻塞任务无限堆积 |
| 每连接发送队列 | 已经生成、尚未发完的 bytes | 防止慢客户端导致内存无限增长 |

两种边界缺一不可：

```text
只有任务池上限，没有发送上限：
  worker 仍可能不断预读文件，把数据堆进发送队列

只有发送上限，没有任务池上限：
  completion worker 仍可能无限提交等待任务

任务池有界 + 一批发完才生产下一批：
  阻塞工作、内存积压和生产提前量同时受控
```

本阶段需要掌握八个术语：

| 术语 | 含义 |
| --- | --- |
| completion worker | 调用 `GetQueuedCompletionStatus()`，处理网络完成通知并推进少量状态的线程。 |
| 业务 worker | 从普通任务池取任务，执行文件、Shell、截图等可能阻塞工作的线程。 |
| 等待任务 | 已被 `submit()` 接受，但尚未被某个业务 worker 取出的任务。 |
| 活动任务 | 已从等待队列移除，当前正在某个业务 worker 上执行的任务。 |
| 有界批次 | 一次只读取、编码并发送有限数量的数据，而不是一次处理完整数据源。 |
| 续传点 | 前一发送项完全发完且发送队列排空后，重新提交下一批业务任务的位置。 |
| 请求合并 | 多个重复请求只保留“还需要再做一次”的意图，不保存每个请求。 |
| 共享帧缓存 | 多个屏幕连接在很短时间内复用同一份已序列化屏幕响应。 |

建议按五个学习单元推进：

1. **划清线程职责（第 4～5 节）**
   - 目标：判断哪些工作能放在 completion worker。
   - 自检：能给十种操作选择正确执行位置。
2. **理解有界任务池（第 6～9 节）**
   - 目标：理解固定 worker、等待队列和拒绝语义。
   - 自检：能推演任务提交、唤醒、取出和执行。
3. **掌握增量文件流（第 10～15 节）**
   - 目标：理解目录和下载为什么不会无限预读。
   - 自检：能推演 131 项目录和 150 KiB 下载。
4. **掌握屏幕流控（第 16～19 节）**
   - 目标：理解一帧在途、请求合并和共享缓存。
   - 自检：能推演连续帧请求和多个观看者。
5. **综合源码与边界（第 20～22 节）**
   - 目标：连接任务生产、网络消费和失败入口。
   - 自检：能独立完成阶段练习。

---

## 2. 知识范围

### 2.1 本阶段核心内容

| 主题 | 需要掌握的内容 |
| --- | --- |
| 职责边界 | completion worker 只做短小、可预测的网络推进；阻塞业务进入独立任务池 |
| 任务池 | 固定线程、有限等待队列、条件变量唤醒、非阻塞提交和任务拒绝 |
| 任务生命周期 | lambda 按值保存请求，`weak_ptr` 不延长失效连接寿命，执行前重新检查状态 |
| 文件状态 | `FileTransferState` 怎样跨越多个短任务和多个发送完成通知 |
| 目录流 | `QDirIterator` 增量枚举、每批最多 64 项、终止标记 |
| 下载流 | 文件长度头、每批最多 64 KiB、实际读取长度和空文件 |
| 续传节奏 | 当前发送项完全发完且发送队列排空后，才向文件池提交下一批 |
| 屏幕状态 | `Idle`、`FramePending`、`FramePendingWithQueuedRequest` |
| 帧缓存 | 16 ms 生命周期、跨连接复用、同一连接不重复接收同一缓存帧 |
| 连接隔离 | 屏幕大数据与控制低延迟命令为什么使用不同 TCP 长连接 |

### 2.2 本阶段不展开的内容

| 主题 | 后续位置 |
| --- | --- |
| `TaskPool::stop()` 怎样取消同步 I/O 并等待线程退出 | 阶段九 |
| `CancelIoEx()`、`shutdown()`、`closesocket()` 的完整顺序 | 阶段九 |
| pending I/O 怎样排空后再关闭 completion port | 阶段九 |
| GDI 截图与 PNG 编码的内部实现 | Windows/Qt 图像专题 |
| 客户端 GUI 绘制、缩放和鼠标坐标映射 | 客户端界面专题 |
| 任务优先级、work stealing、动态扩缩容 | 通用线程池进阶专题 |

阶段八只需要知道：截图和 PNG 编码可能耗时，因此放入截图任务池；不需要先学习它们的图像实现。

---

## 3. 学习完成标准

完成本阶段后，应能够：

1. 说明 completion worker 与普通任务 worker 的职责差异。
2. 解释为什么 completion worker 不能直接执行大文件读取、递归删除、Shell 打开和 PNG 编码。
3. 逐项解释 `TaskPool` constructor、`submit()`、`runWorker()` 的参数、等待队列上限和锁边界。
4. 解释异步 lambda 为什么按值捕获 Packet，却只弱引用连接。
5. 写出 `FileTransferState` 每个字段的稳定含义。
6. 推演目录每批 64 项、下载每批 64 KiB、终止 Packet、短读和空文件。
7. 解释 `finished == true` 为什么只表示“最终批次已经入队”。
8. 解释 send completion 为什么是下一批文件生产的节拍器，并区分四种主要关闭结果。
9. 画出屏幕帧三个状态的转换图，推演多个提前到达的请求怎样合并。
10. 解释 16 ms 帧缓存的复用条件，以及截图为什么仍被全局 mutex 串行化。
11. 解释屏幕流和控制流为什么使用两条独立 TCP 连接。
12. 按源码位置连续追踪文件请求或屏幕请求的完整业务链。

建议投入 8～12 小时。

---

## 4. completion worker 与业务 worker 的职责边界

### 4.1 completion worker 是共享推进器

completion worker 服务的不是某一个连接，而是完成端口上的所有连接。

假设服务端只有两个 completion worker：

```text
worker A：处理连接 1 的 WatchScreen，随后同步截图和 PNG 编码
worker B：处理连接 2 的 DeleteFile，随后递归删除一个大目录

此时：
  连接 3 的 WSARecv 已经完成
  连接 4 的 WSASend 已经完成
  新连接的 AcceptEx 也已经完成

但两个 completion worker 都被业务工作占住，
这些完成通知只能继续停留在 completion port 中。
```

内核完成了 I/O，不等于应用已经处理了 completion。应用仍需要 worker 调用 `GetQueuedCompletionStatus()` 取走通知、更新状态并投递后续 I/O。

因此 completion worker 的工作应满足两个条件：

```text
耗时短：
  不等待磁盘、Shell、截图、GUI 或其他不可预测资源

步骤有限：
  一次 completion 只更新有限状态并投递有限后续工作
```

### 4.2 项目中的职责划分

- **completion worker**
  - 取得 `AcceptEx`、`WSARecv`、`WSASend` 完成通知。
  - 追加接收 bytes、解析已经到达的 Packet、给连接分类并更新少量状态。
  - 调用 `enqueueBytes()`、`postReceive()`、`postSend()`，只做状态准入和异步 I/O 投递。
- **Shell 命令池**
  - 调用操作系统 Shell 打开文件；该调用耗时不可预测。
- **文件池**
  - 执行目录枚举、文件读取和递归删除；这些操作可能等待磁盘或网络文件系统。
- **截图池**
  - 执行截图、PNG 编码和屏幕 Packet 序列化。
- **GUI 线程**
  - 操作 Qt GUI 对象；host service 负责在内部转交到具有线程亲和性的 GUI 线程。

这里不是说“所有业务代码都必须进入任务池”。像 `TestConnection` 这种只构造一个很小 Packet 的路径，可以直接在 completion worker 中完成。

判断标准不是函数名称，而是：

```text
这段工作是否可能长时间等待？
耗时是否受输入规模影响？
是否访问具有线程亲和性的对象？
是否可能让一个连接长期占住共享 completion worker？
```

### 4.3 completion worker 只负责“投递”，不负责“等待”

文件请求的首包到达后，completion worker 做到提交成功就结束：

```cpp
if (_packet.command == remote_control::Command::ListDirectory ||
    _packet.command == remote_control::Command::DownloadFile ||
    _packet.command == remote_control::Command::DeleteFile)
{
    if (!this->m_connectionRegistry.tryClassify(
            _connection, ConnectionPhase::FileTransfer))
    {
        this->closeConnection(_connection, ConnectionCloseReason::ProtocolViolation);
        return false;
    }

    if (!this->scheduleFileRequest(_connection, _packet))
    {
        this->closeConnection(_connection, ConnectionCloseReason::TaskRejected);
    }
    return false;
}
```

`handleInitialPacket()` 的两个参数：

| 参数 | 作用 |
| --- | --- |
| `_connection` | 当前收到首包的连接上下文；分类结果和后续响应都属于它。 |
| `_packet` | 已经解析完成的首个协议包；`command` 决定业务类型，`payload` 保存路径等参数。 |

`scheduleFileRequest()` 的两个参数：

| 参数 | 作用 |
| --- | --- |
| `_connection` | 文件响应最终发送到的连接。 |
| `_packet` | 需要交给文件 worker 的完整命令和 payload。 |

这里返回 `false` 的含义不是“任务失败”，而是文件角色不再继续接收第二个业务请求。文件 worker 会在后台产生响应，completion worker 可以立即回到 completion 循环。

屏幕流不同。它是持久连接，首个 `WatchScreen` 完成分类后，后续仍可以继续接收新的空 payload 帧请求。

---

## 5. 四组 worker 与两种容量

### 5.1 默认 worker 数

项目通过 `RemoteControlTransportOptions` 配置各组 worker：

| 执行组 | 默认数量 | 主要工作 |
| --- | ---: | --- |
| completion workers | 根据硬件限制在 2～4 个 | 网络完成通知和连接状态推进 |
| Shell 命令池 | 2 个 | `RunFile` 对应的操作系统打开请求 |
| 文件池 | 4 个 | 目录、下载、删除 |
| 截图池 | 2 个 | 截图、PNG 编码和帧 Packet 序列化 |

completion worker 数的计算：

```cpp
unsigned int const hardwareThreads{
    std::max(1U, std::thread::hardware_concurrency())};

int const workerCount{
    std::max(
        this->m_options.minimumCompletionWorkerCount,
        std::min(
            this->m_options.maximumCompletionWorkerCount,
            static_cast<int>(hardwareThreads)))};
```

相关值的作用：

| 值 | 作用 |
| --- | --- |
| `hardwareThreads` | 当前运行环境报告的硬件并发度；至少按 1 处理。 |
| `minimumCompletionWorkerCount` | completion worker 的最低数量，默认 2。 |
| `maximumCompletionWorkerCount` | completion worker 的最高数量，默认 4。 |
| `workerCount` | 最终实际创建的 completion worker 数。 |

三个 `std::max()` / `std::min()` 调用的参数：

1. `std::max(1U, hardware_concurrency())`
   - 第一个参数：无法获知并发度时使用的下限 1。
   - 第二个参数：系统报告的并发度。
   - 结果：保证 `hardwareThreads` 至少为 1。
2. `std::min(maximum, hardwareThreads)`
   - 第一个参数：配置允许的最高 worker 数。
   - 第二个参数：硬件并发度。
   - 结果：先限制最高值。
3. `std::max(minimum, previousResult)`
   - 第一个参数：配置要求的最低 worker 数。
   - 第二个参数：上一步结果。
   - 结果：再保证不低于最低值。

`std::thread::hardware_concurrency()` 没有参数，返回实现建议的硬件并发度；返回 0 表示无法获知，所以项目先通过 `std::max(1U, ...)` 保证后续计算至少使用 1。

### 5.2 默认等待队列容量

三个普通任务池分别配置等待任务上限：

| 任务池 | worker 数 | 最多等待任务数 |
| --- | ---: | ---: |
| Shell 命令池 | 2 | 16 |
| 文件池 | 4 | 64 |
| 截图池 | 2 | 8 |

必须区分：

```text
等待任务：
  仍保存在 TaskPool::m_tasks 中

活动任务：
  已经被 worker 从 m_tasks 取走，正在执行
```

`maximumQueuedFileTasks == 64` 只限制等待队列，不包含正在四个文件 worker 上执行的任务。

在四个 worker 全部忙碌、等待队列也满的时刻：

```text
活动任务最多：4
等待任务最多：64
已接受但尚未完成的任务最多：4 + 64 = 68
```

这只是任务池这一层的上界。每个连接还受连接配额、发送队列容量和协议状态约束。

### 5.3 为什么使用三个独立业务池

如果所有阻塞业务共享一个池：

```text
多个大目录递归删除占满全部 worker
  → RunFile 只能等待
  → WatchScreen 也只能等待
  → 文件系统压力扩散成屏幕和控制体验问题
```

分池后：

```text
文件池满：
  只拒绝或延迟新的文件任务

截图池仍有自己的 worker 和等待容量
Shell 池也仍有自己的 worker 和等待容量
```

分池不能消除 CPU、内存和磁盘等物理资源竞争，但能阻止一种业务把所有普通 worker 名额全部占完。

---

## 6. `TaskPool` 保存了什么

关键声明：

```cpp
class TaskPool final
{
public:
    TaskPool(int _workerCount, std::size_t _maximumQueuedTasks);

    [[nodiscard]] bool submit(std::function<void()> _task);

private:
    void runWorker();

    std::size_t m_maximumQueuedTasks{0};
    bool m_stopping{false};
    std::mutex m_mutex;
    std::condition_variable m_condition;
    std::deque<std::function<void()>> m_tasks;
    std::vector<std::thread> m_threads;
};
```

各字段职责：

| 字段 | 由谁保护 | 含义 |
| --- | --- | --- |
| `m_maximumQueuedTasks` | 构造后只读 | 等待队列允许保存的最大任务数 |
| `m_stopping` | `m_mutex` | 停止开始后拒绝新任务，并唤醒 worker 退出 |
| `m_tasks` | `m_mutex` | 尚未被 worker 取走的 FIFO 任务 |
| `m_condition` | 与 `m_mutex` 配合 | 队列为空时让 worker 睡眠；提交或停止时唤醒 |
| `m_threads` | 生命周期代码 | 固定数量、可重复执行任务的 worker 线程 |

任务类型是：

```cpp
std::function<void()>
```

它表示：

```text
调用时不传参数
执行完不返回业务结果
所需输入通过 lambda capture 保存
结果通过连接发送队列、状态或其他线程安全通道发布
```

因此任务池本身不理解“下载”“截图”或“打开文件”。它只负责运行可调用对象。

---

## 7. `TaskPool` constructor：一次创建固定 worker

关键实现：

```cpp
TaskPool::TaskPool(int _workerCount, std::size_t _maximumQueuedTasks)
    : m_maximumQueuedTasks{_maximumQueuedTasks}
{
    this->m_threads.reserve(static_cast<std::size_t>(_workerCount));
    for (int index{0}; index < _workerCount; ++index)
    {
        this->m_threads.emplace_back([this] { this->runWorker(); });
    }
}
```

constructor 的两个参数：

| 参数 | 作用 |
| --- | --- |
| `_workerCount` | 创建多少个常驻业务 worker；项目在构造 transport 前保证它大于 0。 |
| `_maximumQueuedTasks` | `m_tasks` 最多保存多少个等待任务；不计算已被 worker 取出的活动任务。 |

`reserve()` 的参数：

| 参数 | 作用 |
| --- | --- |
| `_newCapacity` | 预留至少能保存多少个 `std::thread` 元素的 vector 容量；不会创建线程，也不会改变 `size()`。 |

这里传入 `_workerCount`，目的是创建线程时避免 `m_threads` 反复扩容。

`emplace_back()` 接收构造新元素所需的参数。这里唯一参数是 lambda：

```cpp
[this] { this->runWorker(); }
```

每次循环构造一个 `std::thread`，新线程从 `runWorker()` 开始执行。

这种设计与“每个请求创建一个线程”不同：

| 每请求创建线程 | 固定任务池 |
| --- | --- |
| 请求越多，线程数越多 | 线程数由 `_workerCount` 固定 |
| 创建、销毁成本反复出现 | worker 被重复使用 |
| 峰值资源难以估计 | 活动任务数由 worker 数限制 |
| 通常没有等待队列上限 | `submit()` 可以立即拒绝满队列 |

---

## 8. `submit()`：非阻塞准入

关键实现：

```cpp
bool TaskPool::submit(std::function<void()> _task)
{
    {
        std::lock_guard<std::mutex> const lock{this->m_mutex};
        if (this->m_stopping ||
            this->m_tasks.size() >= this->m_maximumQueuedTasks)
        {
            return false;
        }
        this->m_tasks.push_back(std::move(_task));
    }

    this->m_condition.notify_one();
    return true;
}
```

`submit()` 的参数：

| 参数 | 作用 |
| --- | --- |
| `_task` | 一个无参数、无返回值的可调用对象；函数按值接收它，然后把其所有权移动到等待队列。 |

返回值：

| 返回值 | 含义 |
| --- | --- |
| `true` | 任务已进入等待队列，之后会由某个 worker 尝试执行。 |
| `false` | 任务池正在停止，或等待队列已经达到上限；任务没有进入任务池。 |

`submit()` 不等待队列出现空位。

这是 completion worker 能安全调用它的关键：

```text
队列未满：
  加锁 → push_back → 解锁 → 唤醒一个 worker → 返回 true

队列已满：
  加锁 → 发现满 → 解锁 → 立即返回 false
```

如果改成“满了就等待”，completion worker 可能卡在任务池容量上，仍然会停止处理所有连接的 completion。

### 8.1 `push_back()` 与 `std::move()`

```cpp
this->m_tasks.push_back(std::move(_task));
```

`push_back()` 的参数是要追加到 deque 尾部的任务对象。

`std::move(_task)` 不执行任务，也不移动线程；它把 `_task` 转换为可移动值，让 deque 接管 lambda 内部捕获对象，避免不必要复制。

提交成功后，不应再依赖原 `_task` 保存的内容。

### 8.2 `notify_one()` 为什么放在解锁之后

`notify_one()` 没有参数，它通知至少一个正在等待该条件变量的 worker 重新检查条件。

项目先释放 `m_mutex` 再通知：

```text
提交线程先把任务完整放入队列并解锁
  → worker 被唤醒
  → worker 可以直接竞争并取得 m_mutex
```

即使在锁内通知也不会丢任务，但被唤醒的 worker 仍要等待提交线程解锁；锁外通知减少这段无意义竞争。

### 8.3 FIFO 只约束取出顺序

等待任务使用：

```cpp
push_back()  // 从尾部加入
front()      // 查看头部
pop_front()  // 从头部移除
```

因此等待队列按 FIFO 取出。

但是多个 worker 可以并行执行：

```text
worker A 先取出任务 1，但任务 1 需要等待磁盘
worker B 后取出任务 2，但任务 2 很快完成

结果：
  取出顺序仍是 1、2
  完成顺序可以是 2、1
```

文件连接不依赖“整个文件池的完成顺序”。同一文件连接在正常路径上一次只提交一个批次任务，前一批发送完成后才允许提交下一批，因此连接内顺序由续传规则保证。

---

## 9. `runWorker()`：等待、取出、解锁、执行

关键实现：

```cpp
void TaskPool::runWorker()
{
    while (true)
    {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock{this->m_mutex};
            this->m_condition.wait(
                lock,
                [this] {
                    return this->m_stopping || !this->m_tasks.empty();
                });

            if (this->m_stopping && this->m_tasks.empty())
            {
                return;
            }

            task = std::move(this->m_tasks.front());
            this->m_tasks.pop_front();
        }

        task();
    }
}
```

`runWorker()` 没有参数。每个 worker 都通过 `this` 访问同一个任务池的 mutex、条件变量、队列和停止状态。

### 9.1 为什么使用 `std::unique_lock`

`std::unique_lock<std::mutex>` constructor 的参数：

| 参数 | 作用 |
| --- | --- |
| `this->m_mutex` | 要取得所有权的 mutex；构造时加锁，离开作用域时解锁。 |

这里不用 `std::lock_guard`，因为 `condition_variable::wait()` 必须在休眠时暂时释放 mutex，并在返回前重新取得 mutex。`unique_lock` 支持这种可解锁、再加锁的生命周期。

### 9.2 `condition_variable::wait()` 的两个参数

```cpp
this->m_condition.wait(lock, predicate);
```

| 参数 | 作用 |
| --- | --- |
| `lock` | 当前持有 `m_mutex` 的 `unique_lock`；等待期间会自动解锁，返回前重新加锁。 |
| `predicate` | 判断是否可以继续执行的函数；本项目条件是“正在停止，或者队列非空”。 |

等待过程可以理解为：

```text
持有 m_mutex 检查 predicate
  ├─ predicate == true：直接继续
  └─ predicate == false：
       原子地释放 m_mutex 并休眠
       被通知后重新取得 m_mutex
       再次检查 predicate
```

反复检查 predicate 可以抵抗虚假唤醒。worker 即使无缘无故醒来，只要“未停止且队列仍为空”，就会继续等待，而不会访问空队列。

### 9.3 为什么必须在 mutex 外执行 `task()`

代码先在锁内取出并删除 queue 头部，随后离开作用域释放 `m_mutex`，才调用：

```cpp
task();
```

如果在锁内执行任务：

```text
worker A 取得 m_mutex
  → 执行一个耗时 5 秒的文件任务

这 5 秒内：
  其他 worker 无法取任务
  submit() 无法加入任务
  多 worker 任务池退化为单线程
```

任务池 mutex 只保护“任务池自己的短状态”，绝不能覆盖具体业务任务。

`m_stopping` 如何与清空队列、取消同步 I/O 和线程 `join()` 配合，放到阶段九学习。

---

## 10. 异步任务怎样保存输入和连接

### 10.1 `scheduleFileRequest()` 的捕获方式

关键代码：

```cpp
bool RemoteControlTransport::Impl::scheduleFileRequest(
    std::shared_ptr<ConnectionContext> const& _connection,
    remote_control::Packet const& _packet)
{
    std::weak_ptr<ConnectionContext> const weakConnection{_connection};

    return this->m_fileTaskPool.submit(
        [this, weakConnection, _packet] {
            std::shared_ptr<ConnectionContext> const connection{
                weakConnection.lock()};

            if (!connection ||
                connection->state.isTerminal() ||
                this->m_stopping.load())
            {
                return;
            }

            switch (_packet.command)
            {
                case remote_control::Command::ListDirectory:
                    this->streamDirectory(connection, _packet.payload);
                    break;
                case remote_control::Command::DownloadFile:
                    this->streamDownload(connection, _packet.payload);
                    break;
                case remote_control::Command::DeleteFile:
                    this->deleteTarget(connection, _packet.payload);
                    break;
                default:
                    this->closeConnection(
                        connection,
                        ConnectionCloseReason::ProtocolViolation);
                    break;
            }
        });
}
```

函数参数：

| 参数 | 作用 |
| --- | --- |
| `_connection` | 发起文件请求的连接；函数只用它构造 `weak_ptr`。 |
| `_packet` | 命令与路径 payload；lambda 按值捕获，确保任务稍后执行时仍有完整输入。 |

lambda capture：

| capture | 为什么这样保存 |
| --- | --- |
| `this` | 访问 transport 的任务池、host service 和成员函数；其安全退出依赖阶段九的任务池停止顺序。 |
| `weakConnection` | 不因为排队任务而强行延长已经断开的连接寿命。 |
| `_packet` | 按值复制 Packet；原接收处理函数返回后，任务仍可安全读取命令和 payload。 |

### 10.2 `weak_ptr::lock()` 没有参数

```cpp
std::shared_ptr<ConnectionContext> const connection{
    weakConnection.lock()};
```

`lock()` 没有参数。

返回结果：

| 结果 | 含义 |
| --- | --- |
| 非空 `shared_ptr` | 连接对象仍存在；当前任务临时取得一份强引用。 |
| 空 `shared_ptr` | 连接对象已经销毁；任务直接返回。 |

任务被接受不等于任务一定要执行完整业务：

```text
t0：submit() 返回 true
t1：客户端断开，连接开始关闭
t2：文件 worker 才取到任务
t3：weakConnection.lock() 失败，或 isTerminal() 为 true
t4：任务不再访问文件系统，也不再发送响应
```

三个执行前检查分别解决：

| 检查 | 防止什么 |
| --- | --- |
| `!connection` | 连接对象已经不存在 |
| `connection->state.isTerminal()` | 连接仍暂时存在，但已经进入 `Closing` 或 `Closed` |
| `m_stopping.load()` | transport 已经开始停止，不应再启动新业务工作 |

### 10.3 Shell 打开使用独立任务池

`RunFile` 的结构与文件请求相同，但投递到 Shell 命令池：

```cpp
bool RemoteControlTransport::Impl::scheduleOpenFile(
    std::shared_ptr<ConnectionContext> const& _connection,
    QByteArray const& _payload)
{
    std::weak_ptr<ConnectionContext> const weakConnection{_connection};
    return this->m_shellCommandTaskPool.submit([this, weakConnection, _payload] {
        auto const connection{weakConnection.lock()};
        if (!connection || connection->state.isTerminal() || this->m_stopping.load())
        {
            return;
        }

        QString const path{remote_control::decodeUtf8(_payload)};
        bool const success{this->m_hostServices.isFilePathAllowed(path) &&
                           QFileInfo::exists(path) &&
                           this->m_hostServices.openFile(path)};
        QString const message{success
                                  ? QObject::tr("Open request accepted.")
                                  : QObject::tr("Failed to open file: %1").arg(path)};
        this->sendFinalPacket(
            connection,
            makeStatusPacket(remote_control::Command::RunFile, success, message));
    });
}
```

函数参数：

| 参数 | 作用 |
| --- | --- |
| `_connection` | 最终接收打开结果状态 Packet 的连接。 |
| `_payload` | 客户端传来的 UTF-8 路径 bytes；按值捕获到异步 lambda。 |

相关 host service 函数：

| 函数 | 参数 | 返回值 |
| --- | --- | --- |
| `isFilePathAllowed(_path)` | `_path` 是准备访问的本地路径 | 允许访问返回 `true` |
| `openFile(_path)` | `_path` 是已经存在且允许访问的文件路径 | 操作系统接受打开请求返回 `true` |

Shell 调用可能涉及操作系统、文件关联和进程启动，耗时不可预测，所以不放在 completion worker。

---

## 11. `FileTransferState`：跨任务保存文件进度

文件传输不是一个长时间占住 worker 的函数，而是一连串短任务：

```text
初始化传输状态
  → 生产第 1 批
  → worker 返回
  → 第 1 批发完
  → 生产第 2 批
  → worker 返回
  → ...
```

局部变量无法跨越这些独立任务，因此项目使用：

```cpp
enum class FileTransferKind
{
    Directory,
    Download,
};

struct FileTransferState final
{
    explicit FileTransferState(FileTransferKind _kind);

    FileTransferKind kind{FileTransferKind::Directory};
    std::ifstream file;
    qint64 remainingBytes{0};
    bool headerPending{true};
    std::unique_ptr<QDirIterator> directoryIterator;
    bool finished{false};
};
```

constructor 参数：

| 参数 | 作用 |
| --- | --- |
| `_kind` | 指定这份状态属于目录枚举还是文件下载；续传时据此选择下一批生产函数。 |

字段的稳定含义：

| 字段 | 目录传输 | 下载传输 |
| --- | --- | --- |
| `kind` | `Directory` | `Download` |
| `file` | 不使用 | 保持已经打开的二进制输入流 |
| `remainingBytes` | 不使用 | 尚未读取并加入发送链的数据字节数 |
| `headerPending` | 不使用 | 文件长度头是否尚未加入发送链 |
| `directoryIterator` | 保存下一项枚举位置 | 不使用 |
| `finished` | 终止目录 Packet 已加入最终批次 | 最后一段数据或空文件头已加入最终批次 |

最容易误解的是 `finished`：

```text
finished == false：
  当前批次发完后，还需要生产下一批

finished == true：
  最终批次已经进入发送链
  但它可能仍在发送，客户端不一定已经收到
```

真正关闭连接的时刻仍然是最终批次完全发完、发送队列排空之后。

连接保存这份状态：

```cpp
std::mutex fileTransferMutex;
std::shared_ptr<FileTransferState> fileTransfer;
```

`fileTransferMutex` 保护连接当前指向哪一份传输状态；`shared_ptr` 让状态可以同时被连接和当前短任务安全持有。

文件读取和目录枚举不在 `fileTransferMutex` 内执行。锁只用于短暂安装、取得或清除状态。

---

## 12. 目录流：每批最多 64 项

### 12.1 初始化目录传输

关键代码：

```cpp
void RemoteControlTransport::Impl::streamDirectory(
    std::shared_ptr<ConnectionContext> const& _connection,
    QByteArray const& _payload)
{
    QString const path{remote_control::decodeUtf8(_payload)};
    QFileInfo const directoryInfo{path};
    QDir const directory{path};
    if (!this->m_hostServices.isFilePathAllowed(path) ||
        !directoryInfo.exists() ||
        !directoryInfo.isDir() ||
        !directory.isReadable())
    {
        remote_control::FileEntry invalidEntry;
        invalidEntry.isInvalid = true;
        invalidEntry.hasNext = false;
        this->sendFinalPacket(_connection,
                              {remote_control::Command::ListDirectory,
                               invalidEntry.toPayload()});
        return;
    }

    auto transfer{std::make_shared<FileTransferState>(FileTransferKind::Directory)};
    transfer->directoryIterator = std::make_unique<QDirIterator>(
        path, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);

    {
        std::lock_guard<std::mutex> const lock{_connection->fileTransferMutex};
        if (this->m_stopping.load() || _connection->state.isTerminal())
        {
            return;
        }
        _connection->fileTransfer = transfer;
    }
    this->queueDirectoryBatch(_connection, transfer);
}
```

`streamDirectory()` 的参数：

| 参数 | 作用 |
| --- | --- |
| `_connection` | 保存目录传输状态并接收目录响应的连接。 |
| `_payload` | UTF-8 编码的目标目录路径。 |

`QDirIterator` constructor 的两个关键参数：

| 参数 | 作用 |
| --- | --- |
| `path` | 要增量枚举的目录路径。 |
| filters | 选择普通目录项、隐藏项和系统项，并排除 `.` 与 `..`。 |

`std::make_shared<FileTransferState>(_kind)` 把 `_kind` 转交给 `FileTransferState` constructor，并返回共享所有权对象；这里传入 `Directory`。`std::make_unique<QDirIterator>(_path, _filters)` 把目录路径和过滤条件转交给 `QDirIterator` constructor，并返回独占迭代器对象。

`QDirIterator` 不会先把完整目录复制到一个大列表。它保存枚举位置，适合在多个批次之间继续。

先把 `transfer` 安装到连接，再生产第一批。这样第一批发送完成后，completion worker 能从连接中找到同一份状态。

### 12.2 生产一个目录批次

项目常量：

```cpp
constexpr int DirectoryEntriesPerBatch{64};
```

关键循环：

```cpp
void RemoteControlTransport::Impl::queueDirectoryBatch(
    std::shared_ptr<ConnectionContext> const& _connection,
    std::shared_ptr<FileTransferState> const& _transfer)
{
    if (this->m_stopping.load() || _connection->state.isTerminal())
    {
        return;
    }

    QByteArray bytes;
    int encodedEntryCount{0};
    while (_transfer->directoryIterator &&
           _transfer->directoryIterator->hasNext() &&
           encodedEntryCount < DirectoryEntriesPerBatch)
    {
        _transfer->directoryIterator->next();
        QFileInfo const info{_transfer->directoryIterator->fileInfo()};
        remote_control::FileEntry entry;
        entry.isDirectory = info.isDir();
        entry.fileName = info.fileName();
        QByteArray const packetBytes{remote_control::Packet{
            remote_control::Command::ListDirectory, entry.toPayload()}.serialize()};
        if (packetBytes.isEmpty())
        {
            this->closeConnection(_connection, ConnectionCloseReason::InternalFailure);
            return;
        }
        bytes.append(packetBytes);
        ++encodedEntryCount;
    }

    if (!_transfer->directoryIterator || !_transfer->directoryIterator->hasNext())
    {
        remote_control::FileEntry terminalEntry;
        terminalEntry.hasNext = false;
        bytes.append(remote_control::Packet{
            remote_control::Command::ListDirectory, terminalEntry.toPayload()}.serialize());
        _transfer->finished = true;
    }

    if (!this->enqueueBytes(_connection, bytes))
    {
        this->closeConnection(_connection, ConnectionCloseReason::Backpressure);
    }
}
```

`queueDirectoryBatch()` 的参数：

| 参数 | 作用 |
| --- | --- |
| `_connection` | 当前目录响应的目标连接；用于状态检查、发送入队和错误关闭。 |
| `_transfer` | 保存当前 `QDirIterator` 位置与 `finished` 状态的目录传输对象。 |

`QDirIterator` 相关函数：

| 函数 | 参数 | 作用 |
| --- | --- | --- |
| `hasNext()` | 无 | 判断迭代器后面是否还有目录项。 |
| `next()` | 无 | 前进到下一项，并返回其路径；本代码只使用它推进位置。 |
| `fileInfo()` | 无 | 取得迭代器当前项的 `QFileInfo`。 |

`QByteArray::append(_data)` 的 `_data` 是要追加到尾部的 bytes。这里连续追加多个完整 Packet 的 wire bytes，形成一个发送批次。

“每批最多 64 项”限制的是目录项数量，不是固定字节数。文件名长度不同，批次的实际 bytes 也不同；生成后仍要经过阶段七的 `enqueueBytes()` 字节容量检查。

### 12.3 一个发送项可以包含多个 Packet

假设当前批次有三个目录项：

```text
bytes =
  Packet(条目 A) 的完整 wire bytes
  + Packet(条目 B) 的完整 wire bytes
  + Packet(条目 C) 的完整 wire bytes
```

`enqueueBytes()` 看到的是一个 `QByteArray` 发送项，但客户端 Packet parser 会从 TCP 字节流中依次解析出三个 Packet。

这复用了阶段三已经掌握的粘包处理：

```text
一次 socket read 得到多少 bytes
  ≠
一次只对应一个 Packet
```

### 12.4 为什么还要终止 Packet

普通 `FileEntry` 默认：

```cpp
bool hasNext{true};
```

目录枚举结束时，服务端追加：

```cpp
terminalEntry.hasNext = false;
```

终止 Packet 有三个作用：

1. 客户端不需要通过“连接何时关闭”猜测目录是否完整。
2. 空目录仍会返回一个明确响应。
3. 客户端能区分“合法结束”和“中途断开”。

终止 Packet 与最后几个目录项可以位于同一个发送批次。

### 12.5 131 个条目的完整推演

目录恰好有 131 个条目：

```text
第 1 个文件任务：
  编码条目 1～64
  iterator 仍有下一项
  finished = false
  enqueue 1 个发送批次
  worker 返回

第 1 批完全发完：
  completion worker 提交下一文件任务

第 2 个文件任务：
  编码条目 65～128
  iterator 仍有下一项
  finished = false
  enqueue 1 个发送批次
  worker 返回

第 2 批完全发完：
  completion worker 提交下一文件任务

第 3 个文件任务：
  编码条目 129～131
  iterator 已结束
  再追加 1 个 hasNext == false 的终止 Packet
  finished = true
  enqueue 1 个发送批次
  worker 返回

第 3 批完全发完：
  completion worker 发现 finished == true
  关闭连接，原因是 RequestComplete
```

结果是三个发送批次、132 个协议 Packet，其中 131 个是目录项，1 个是终止标记。

---

## 13. 下载流：长度头加 64 KiB 数据块

### 13.1 初始化下载状态

项目常量：

```cpp
constexpr int DownloadChunkSize{64 * 1024};
```

关键初始化代码：

```cpp
void RemoteControlTransport::Impl::streamDownload(
    std::shared_ptr<ConnectionContext> const& _connection,
    QByteArray const& _payload)
{
    QString const path{remote_control::decodeUtf8(_payload)};
    if (!this->m_hostServices.isFilePathAllowed(path))
    {
        this->sendFinalPacket(_connection,
                              {remote_control::Command::DownloadFile,
                               makeFileSizePayload(-1)});
        return;
    }

    auto transfer{std::make_shared<FileTransferState>(FileTransferKind::Download)};
    transfer->file.open(std::filesystem::path{path.toStdWString()}, std::ios::binary);
    if (!transfer->file.is_open())
    {
        this->sendFinalPacket(_connection,
                              {remote_control::Command::DownloadFile,
                               makeFileSizePayload(-1)});
        return;
    }

    transfer->file.seekg(0, std::ios::end);
    std::streamoff const fileSize{transfer->file.tellg()};
    transfer->file.seekg(0, std::ios::beg);
    if (fileSize < 0 || !transfer->file)
    {
        this->sendFinalPacket(_connection,
                              {remote_control::Command::DownloadFile,
                               makeFileSizePayload(-1)});
        return;
    }

    transfer->remainingBytes = static_cast<qint64>(fileSize);
    {
        std::lock_guard<std::mutex> const lock{_connection->fileTransferMutex};
        if (this->m_stopping.load() || _connection->state.isTerminal())
        {
            return;
        }
        _connection->fileTransfer = transfer;
    }
    this->queueDownloadChunk(_connection, transfer);
}
```

`streamDownload()` 的参数：

| 参数 | 作用 |
| --- | --- |
| `_connection` | 保存下载状态并接收长度头与文件数据的连接。 |
| `_payload` | UTF-8 编码的源文件路径。 |

`std::ifstream::open()` 的两个参数：

| 参数 | 作用 |
| --- | --- |
| 文件路径 | 要打开的本地文件；项目使用 `std::filesystem::path` 保留 Windows 宽字符路径。 |
| `std::ios::binary` | 以二进制方式读取，避免文本模式转换字节。 |

`path.toStdWString()` 没有参数，把 Qt 字符串转换为宽字符串；该结果用于构造 Windows 路径。

`seekg()` 的参数：

| 参数 | 作用 |
| --- | --- |
| 第一个参数 | 相对基准位置的偏移量；这里传 0。 |
| 第二个参数 | 基准位置；`std::ios::end` 是文件末尾，`std::ios::beg` 是文件开头。 |

`tellg()` 没有参数，返回当前读取位置。先移动到末尾再调用它，就得到文件长度；随后必须回到开头准备真正读取。

`makeFileSizePayload(_size)` 的参数：

| 参数 | 作用 |
| --- | --- |
| `_size` | 非负值表示完整文件长度；负值 `-1` 表示下载初始化失败。 |

### 13.2 第一批为什么可能有两个 Packet

关键生产代码：

```cpp
void RemoteControlTransport::Impl::queueDownloadChunk(
    std::shared_ptr<ConnectionContext> const& _connection,
    std::shared_ptr<FileTransferState> const& _transfer)
{
    if (this->m_stopping.load() || _connection->state.isTerminal())
    {
        return;
    }

    QByteArray bytes;
    if (_transfer->headerPending)
    {
        bytes = remote_control::Packet{
            remote_control::Command::DownloadFile,
            makeFileSizePayload(_transfer->remainingBytes)}.serialize();
        _transfer->headerPending = false;
    }

    if (_transfer->remainingBytes > 0)
    {
        qint64 const requestedBytes{
            std::min<qint64>(_transfer->remainingBytes, DownloadChunkSize)};
        QByteArray chunk{static_cast<int>(requestedBytes), Qt::Uninitialized};
        _transfer->file.read(chunk.data(), static_cast<std::streamsize>(requestedBytes));
        std::streamsize const readBytes{_transfer->file.gcount()};
        if (readBytes <= 0)
        {
            this->closeConnection(_connection, ConnectionCloseReason::IoFailure);
            return;
        }
        chunk.resize(static_cast<int>(readBytes));
        bytes.append(remote_control::Packet{
            remote_control::Command::DownloadFile, chunk}.serialize());
        _transfer->remainingBytes -= chunk.size();
    }

    _transfer->finished = _transfer->remainingBytes == 0;
    if (bytes.isEmpty() || !this->enqueueBytes(_connection, bytes))
    {
        this->closeConnection(_connection, ConnectionCloseReason::Backpressure);
    }
}
```

`queueDownloadChunk()` 的参数：

| 参数 | 作用 |
| --- | --- |
| `_connection` | 当前下载的目标连接；用于状态检查、入队和失败关闭。 |
| `_transfer` | 保持文件流、剩余长度、头部状态和完成状态的下载对象。 |

第一次调用时 `headerPending == true`：

```text
第一个 Packet：
  payload = 完整文件长度

如果文件非空，紧接着还有第二个 Packet：
  payload = 最多 64 KiB 文件数据

两个 Packet 的 wire bytes 被拼成同一个发送批次
```

后续调用不再发送长度头，只发送最多一个数据 Packet。

### 13.3 `std::min()`、`read()` 与 `gcount()`

`std::min<qint64>()` 的两个参数：

| 参数 | 作用 |
| --- | --- |
| `_transfer->remainingBytes` | 还没有读取并入队的数据量。 |
| `DownloadChunkSize` | 单批读取上限 64 KiB。 |

结果 `requestedBytes` 永远不超过剩余数据，也不超过 64 KiB。

`std::ifstream::read()` 的两个参数：

| 参数 | 作用 |
| --- | --- |
| `chunk.data()` | 接收文件 bytes 的可写内存起始地址。 |
| `requestedBytes` | 希望最多读取多少字节。 |

用于创建 `chunk` 的 `QByteArray` constructor：

```cpp
QByteArray chunk{
    static_cast<int>(requestedBytes),
    Qt::Uninitialized};
```

| 参数 | 作用 |
| --- | --- |
| `requestedBytes` | 先为本批希望读取的字节数分配空间。 |
| `Qt::Uninitialized` | 不预先填充缓冲区内容，因为紧接着的 `read()` 会覆盖实际读取区域。 |

`gcount()` 没有参数，返回上一次非格式化读取实际取得的字节数。

不能直接假设 `readBytes == requestedBytes`。

因此代码按 `readBytes` 缩小 `chunk`，并只从 `remainingBytes` 中减去真实进入 Packet 的 `chunk.size()`。

`chunk.resize(_size)` 的 `_size` 是调整后的有效字节数。这里传入 `readBytes`，用于丢弃“已分配但本次没有读到数据”的尾部空间。

### 13.4 150 KiB 下载推演

文件大小为 `150 KiB = 150 × 1024 bytes`。

批次变化：

1. **第 1 批**
   - 发送：长度头 Packet 和 64 KiB 数据 Packet。
   - 入队后：`remainingBytes == 86 KiB`，`finished == false`。
2. **第 2 批**
   - 发送：64 KiB 数据 Packet。
   - 入队后：`remainingBytes == 22 KiB`，`finished == false`。
3. **第 3 批**
   - 发送：22 KiB 数据 Packet。
   - 入队后：`remainingBytes == 0`，`finished == true`。

第三批入队时，文件读取已经结束，但连接不能立即关闭：

```text
第 3 批仍可能正在 WSASend
  → 等待完整发送完成
  → 等待发送队列排空
  → continueFileTransfer() 看到 finished == true
  → closeConnection(RequestComplete)
```

### 13.5 空文件

空文件初始化后，`remainingBytes == 0`，`headerPending == true`。

第一次 `queueDownloadChunk()`：

```text
生成文件长度为 0 的头 Packet
不进入数据读取分支
finished = true
enqueue 头 Packet
```

空文件仍有一个明确的成功响应，不能把“没有数据 Packet”误判为下载失败。

### 13.6 为什么不一次读完整文件

如果直接按文件长度分配内存：

```text
六个客户端同时下载 2 GiB 文件
  → 业务层可能尝试保留约 12 GiB 文件内容
  → 客户端越慢，内存保留越久
```

增量设计把生产提前量限制为：

```text
读取最多 64 KiB
  → 入队
  → worker 返回
  → 只有该批发完后才允许读取下一批
```

稳定状态下，业务层不会把完整文件预读到内存。实际内存还包含 Packet 头、临时序列化对象、当前发送 operation 和系统网络缓冲区，因此不应把“64 KiB”误解为连接全部内存的精确值。

---

## 14. 发送完成怎样驱动下一批文件生产

### 14.1 续传入口只在发送队列排空后发生

阶段七已经学习过 `handleSendCompletion()` 的发送交接。本阶段只关注函数尾部：

```cpp
void RemoteControlTransport::Impl::handleSendCompletion(
    std::unique_ptr<IoOperation> _operation,
    bool _success,
    DWORD _transferredBytes)
{
    // 省略阶段七已经掌握的失败、部分发送和 FIFO 交接。
    // 只有当前发送项完整完成且 sendQueue 为空，才会到达这里。
    if (closeAfterSend)
    {
        this->closeConnection(connection, ConnectionCloseReason::RequestComplete);
        return;
    }

    ConnectionPhase const currentPhase{connection->state.phase()};
    if (currentPhase == ConnectionPhase::ScreenStream)
    {
        this->completeScreenFrame(connection);
    }
    else if (currentPhase == ConnectionPhase::FileTransfer)
    {
        this->continueFileTransfer(connection);
    }
}
```

函数参数：

| 参数 | 作用 |
| --- | --- |
| `_operation` | 当前完成的 send operation；保存连接、完整发送项、当前偏移量等状态。 |
| `_success` | 本次 IOCP 完成是否成功。 |
| `_transferredBytes` | 本次完成实际发送的字节数；可能小于发送项剩余长度。 |

只有以下条件全部成立，代码才会走到业务续传：

```text
本次发送成功
当前发送项已经全部发完
发送队列没有下一等待项
没有 closeAfterSend 最终关闭意图
连接仍处于 FileTransfer 或 ScreenStream
```

部分发送不会触发下一批。存在等待发送项时也不会触发下一批。

### 14.2 `continueFileTransfer()` 的两条分支

关键实现：

```cpp
void RemoteControlTransport::Impl::continueFileTransfer(
    std::shared_ptr<ConnectionContext> const& _connection)
{
    std::shared_ptr<FileTransferState> transfer;
    {
        std::lock_guard<std::mutex> const lock{_connection->fileTransferMutex};
        transfer = _connection->fileTransfer;
        if (!transfer)
        {
            return;
        }
        if (transfer->finished)
        {
            _connection->fileTransfer.reset();
        }
    }

    if (transfer->finished)
    {
        this->closeConnection(_connection, ConnectionCloseReason::RequestComplete);
        return;
    }

    std::weak_ptr<ConnectionContext> const weakConnection{_connection};
    bool const submitted{this->m_fileTaskPool.submit([this, weakConnection, transfer] {
        auto const connection{weakConnection.lock()};
        if (!connection || connection->state.isTerminal() || this->m_stopping.load())
        {
            return;
        }

        if (transfer->kind == FileTransferKind::Directory)
        {
            this->queueDirectoryBatch(connection, transfer);
        }
        else
        {
            this->queueDownloadChunk(connection, transfer);
        }
    })};
    if (!submitted)
    {
        this->closeConnection(_connection, ConnectionCloseReason::TaskRejected);
    }
}
```

`continueFileTransfer()` 的参数：

| 参数 | 作用 |
| --- | --- |
| `_connection` | 刚刚排空当前发送链的文件连接；函数从中取得持续传输状态。 |

两条主分支：

```text
transfer->finished == true：
  清除连接持有的状态
  最终批次已经发完
  正常关闭连接

transfer->finished == false：
  当前批次已经发完
  向文件池提交下一批生产任务
  completion worker 立即返回
```

### 14.3 完整时间线

以下载为例：

```text
t0  接收完成进入 completion worker
    解析 DownloadFile
    scheduleFileRequest()
    文件任务进入队列
    completion worker 返回

t1  文件 worker 取到任务
    打开文件并读取第 1 个 64 KiB
    enqueueBytes(第 1 批)
    文件 worker 返回

t2  第 1 批 WSASend 只完成一部分
    completion worker 更新 sendOffset
    继续 postSend 同一发送项
    不提交文件任务

t3  第 1 批完全发完，sendQueue 也为空
    completion worker 调用 continueFileTransfer()
    下一批任务进入文件池
    completion worker 返回

t4  任意空闲文件 worker 读取第 2 批
    enqueueBytes(第 2 批)
    文件 worker 返回
```

整个过程中：

```text
文件 worker 不等待网络
completion worker 不等待文件
慢客户端不产生新的文件读取任务
```

### 14.4 为什么慢客户端反而释放文件 worker

慢客户端使发送完成变慢：

```text
前一批迟迟没有完整发送完成
  → 不调用 continueFileTransfer()
  → 不提交该连接的下一批文件任务
  → 文件 worker 可以处理其他连接
```

这就是“由消费者进度驱动生产者”的价值。

如果文件 worker 在一个循环中不断读取并等待发送容量：

```text
慢客户端会长期占住一个文件 worker
四个慢客户端就可能占满默认四个文件 worker
其他目录请求无法得到执行机会
```

当前项目每次只生产一批，因此任务执行时间短，多个连接的批次可以在文件池中交错执行。

---

## 15. 两层拒绝不能混淆

阶段八同时出现任务池容量和发送容量：

| 失败结果 | 发生位置 | 含义 |
| --- | --- | --- |
| `TaskRejected` | `TaskPool::submit()` 返回 `false` | 普通任务池正在停止或等待队列已满 |
| `Backpressure` | `enqueueBytes()` 无法接受新 bytes | 当前连接的发送积压或发送项不满足准入条件 |
| `CapacityLimit` | 连接注册或持久流分类 | 总连接数、屏幕流数或控制流数达到上限 |
| `IoFailure` | 文件读取或 socket I/O 失败 | 底层 I/O 没有按正常路径完成 |
| `RequestComplete` | 最终响应已发完 | 正常业务完成，不是错误 |

### 15.1 任务池拒绝

```text
文件池已有 4 个活动任务
等待队列已有 64 个任务
completion worker 再调用 submit()
  → 立即返回 false
  → 连接以 TaskRejected 关闭
  → completion worker 不等待
```

### 15.2 发送背压

```text
文件 worker 已经生产一个批次
调用 enqueueBytes()
发现该连接无法再接受这些 bytes
  → 返回 false
  → 连接以 Backpressure 关闭
  → worker 不等待发送队列腾出空间
```

### 15.3 为什么两层都采用“拒绝而不是等待”

```text
任务池满时等待：
  会阻塞提交者，提交者可能正是 completion worker

发送队列满时等待：
  会阻塞业务 worker，慢客户端会占满整个业务池

立即拒绝：
  牺牲过载连接
  保住共享线程和其他连接
```

这是服务端过载保护，不应简单理解为“程序不愿意工作”。

---

## 16. 屏幕流为什么需要三个状态

屏幕流与文件流都使用发送完成作为节拍，但业务语义不同：

| 文件流 | 屏幕流 |
| --- | --- |
| 数据源有限，最终正常关闭 | 持久连接，按请求持续产生帧 |
| 每一批都不能丢 | 多个重复刷新请求可以合并 |
| 下一批由“还有未发送数据”决定 | 下一帧由“是否还保留一个请求”决定 |

项目状态：

```cpp
enum class ScreenFrameFlowState
{
    Idle,
    FramePending,
    FramePendingWithQueuedRequest,
};
```

三个值的准确含义：

| 状态 | 当前帧 | 额外请求 |
| --- | --- | --- |
| `Idle` | 没有捕获、排队或发送中的帧 | 没有 |
| `FramePending` | 有一帧正在截图、等待发送或正在发送 | 没有 |
| `FramePendingWithQueuedRequest` | 有一帧正在处理 | 只保留一个“完成后再来一帧”的意图 |

这里的 `FramePending` 不只是“截图函数正在执行”。

从任务提交开始，一直到该帧发送项完全发完并且发送队列排空，它都保持 pending：

```text
截图任务已提交
  → 截图中
  → PNG 编码中
  → 帧 Packet 已进入发送链
  → WSASend 中
  → 发送队列排空

以上都属于一帧 pending
```

这样才能真正保证每条屏幕连接最多一帧在途。

---

## 17. `scheduleScreenFrame()`：开始或合并请求

关键代码：

```cpp
bool RemoteControlTransport::Impl::scheduleScreenFrame(
    std::shared_ptr<ConnectionContext> const& _connection)
{
    {
        std::lock_guard<std::mutex> const lock{_connection->screenFrameMutex};
        if (_connection->state.isTerminal())
        {
            return false;
        }
        if (_connection->screenFrameState == ScreenFrameFlowState::FramePending)
        {
            _connection->screenFrameState =
                ScreenFrameFlowState::FramePendingWithQueuedRequest;
            return true;
        }
        if (_connection->screenFrameState ==
            ScreenFrameFlowState::FramePendingWithQueuedRequest)
        {
            return true;
        }
        _connection->screenFrameState = ScreenFrameFlowState::FramePending;
    }

    bool const submitted{this->submitScreenFrame(_connection)};
    if (!submitted)
    {
        std::lock_guard<std::mutex> const lock{_connection->screenFrameMutex};
        _connection->screenFrameState = ScreenFrameFlowState::Idle;
    }
    return submitted;
}
```

`scheduleScreenFrame()` 的参数：

| 参数 | 作用 |
| --- | --- |
| `_connection` | 收到 `WatchScreen` 请求的屏幕长连接；函数修改它自己的帧流状态。 |

返回值：

| 返回值 | 含义 |
| --- | --- |
| `true` | 请求已经启动，或者已成功合并到现有 pending 帧之后。 |
| `false` | 连接已终止，或者新的截图任务没有被任务池接受。 |

### 17.1 三种输入状态

#### 输入是 `Idle`

```text
Idle
  → 锁内改成 FramePending
  → 锁外调用 submitScreenFrame()
```

如果任务池拒绝：

```text
重新加锁
  → 回滚为 Idle
  → 返回 false
  → 调用方以 TaskRejected 关闭连接
```

#### 输入是 `FramePending`

```text
FramePending
  → 改成 FramePendingWithQueuedRequest
  → 不提交第二个并行截图任务
  → 返回 true
```

#### 输入是 `FramePendingWithQueuedRequest`

```text
状态不变
不增加计数
不保存第三个请求
直接返回 true
```

这就是“最多合并一个”。

### 17.2 为什么在锁外提交任务

`screenFrameMutex` 只保护当前连接的帧状态。

如果持有它调用 `submitScreenFrame()`：

```text
连接状态锁
  → 再取得任务池锁
```

会扩大临界区并引入不必要的锁嵌套。

当前代码采用阶段七已经学过的模式：

```text
锁内：
  检查状态
  修改状态
  决定动作

锁外：
  执行任务池提交

提交失败：
  再加锁回滚状态
```

### 17.3 五个提前到达请求的推演

假设五个 `WatchScreen` 请求都在第一帧发送完成前到达：

1. 请求 1：`Idle → FramePending`，提交 1 个截图任务。
2. 请求 2：`FramePending → FramePendingWithQueuedRequest`，不提交新任务。
3. 请求 3：状态已经是 `FramePendingWithQueuedRequest`，不变，也不提交新任务。
4. 请求 4：状态不变，不提交新任务。
5. 请求 5：状态不变，不提交新任务。

第一帧完全发完后，服务端只再提交一帧：

```text
第一帧发送队列排空
  → 发现存在一个合并请求
  → 提交第二帧

第二帧发送队列排空
  → 没有新的合并请求
  → 回到 Idle
```

这五个请求最多形成两帧，而不是五帧。

请求合并保留的是“当前帧之后还需要刷新一次”，而不是“每个请求都必须对应一张历史截图”。

屏幕监控关心最新画面，过时的排队帧没有价值。

---

## 18. `submitScreenFrame()`：截图任务只生产一帧

关键代码：

```cpp
bool RemoteControlTransport::Impl::submitScreenFrame(
    std::shared_ptr<ConnectionContext> const& _connection)
{
    std::weak_ptr<ConnectionContext> const weakConnection{_connection};
    return this->m_screenCaptureTaskPool.submit([this, weakConnection] {
        auto const connection{weakConnection.lock()};
        if (!connection || connection->state.isTerminal() || this->m_stopping.load())
        {
            return;
        }

        QByteArray const packetBytes{this->makeScreenFramePacketBytes(connection)};
        bool const queued{!packetBytes.isEmpty() &&
                          this->enqueueBytes(connection, packetBytes)};
        if (!queued)
        {
            this->closeConnection(connection, ConnectionCloseReason::Backpressure);
        }
    });
}
```

`submitScreenFrame()` 的参数：

| 参数 | 作用 |
| --- | --- |
| `_connection` | 请求屏幕帧的持久连接；任务通过弱引用访问它。 |

返回值只表示截图任务是否被任务池接受，不表示截图已经成功、帧已经进入发送链或客户端已经收到帧。

截图 worker 执行时仍要重新检查连接与 transport 状态。

`makeScreenFramePacketBytes()` 失败会返回空 bytes；`enqueueBytes()` 也可能拒绝。当前调用点把这两种情况都归入 `queued == false`，随后关闭连接。阅读日志或调试时，不能只凭 `Backpressure` 名称断言一定是发送积压，还要向前检查帧生产是否返回空值。

---

## 19. 16 ms 共享帧缓存

### 19.1 完整代码路径

项目常量：

```cpp
constexpr qint64 SharedScreenFrameLifetimeMs{16};
```

关键实现：

```cpp
QByteArray
RemoteControlTransport::Impl::makeScreenFramePacketBytes(
    std::shared_ptr<ConnectionContext> const& _connection)
{
    std::lock_guard<std::mutex> const lock{this->m_screenFrameCacheMutex};
    qint64 const now{monotonicMilliseconds()};
    if (!this->m_screenFramePacketCache.isEmpty() &&
        now - this->m_screenFrameCacheTimestampMs <= SharedScreenFrameLifetimeMs &&
        _connection->lastScreenFrameId.load() != this->m_screenFrameCacheId)
    {
        _connection->lastScreenFrameId.store(this->m_screenFrameCacheId);
        return this->m_screenFramePacketCache;
    }

    QByteArray payload{this->m_hostServices.captureScreenPng()};
    if (payload.isEmpty())
    {
        return {};
    }
    QByteArray const packetBytes{remote_control::Packet{
        remote_control::Command::WatchScreen, std::move(payload)}.serialize()};
    if (packetBytes.isEmpty())
    {
        return {};
    }

    this->m_screenFramePacketCache = packetBytes;
    this->m_screenFrameCacheTimestampMs = monotonicMilliseconds();
    ++this->m_screenFrameCacheId;
    _connection->lastScreenFrameId.store(this->m_screenFrameCacheId);
    return packetBytes;
}
```

`makeScreenFramePacketBytes()` 的参数：

| 参数 | 作用 |
| --- | --- |
| `_connection` | 当前请求帧的屏幕连接；用于判断它是否已经收到最新缓存帧，并记录新帧 ID。 |

相关函数：

| 函数 | 参数 | 返回值 |
| --- | --- | --- |
| `monotonicMilliseconds()` | 无 | 单调递增的毫秒时间，用于计算缓存年龄，不受系统时间回拨影响。 |
| `captureScreenPng()` | 无 | 当前屏幕的 PNG bytes；失败返回空数组。 |
| `Packet::serialize()` | 无 | 当前 Packet 的完整 wire bytes；失败返回空数组。 |

### 19.2 复用缓存必须同时满足三个条件

```text
条件一：
  缓存非空

条件二：
  当前时间 - 缓存时间 <= 16 ms

条件三：
  当前连接上次收到的帧 ID != 当前缓存帧 ID
```

任意条件不满足，都重新截图。

### 19.3 为什么检查 `lastScreenFrameId`

假设连接 A 刚刚触发并收到缓存帧 42，此时全局缓存 ID 和 `A.lastScreenFrameId` 都是 42。

A 在 16 ms 内再次请求：

```text
A.lastScreenFrameId == 全局缓存 ID
  → 不能把完全相同的帧 42 再发给 A
  → 重新截图并生成帧 43
```

连接 B 尚未收到帧 42：

```text
B.lastScreenFrameId != 42
  → 如果缓存仍在 16 ms 内
  → B 可以复用帧 42
```

因此缓存主要服务于“不同观看连接在相近时间请求同一屏幕”，而不是让同一连接连续收到重复画面。

### 19.4 为什么缓存完整 Packet bytes

缓存内容不是原始屏幕像素，也不只是 PNG payload，而是 `WatchScreen` Packet 的完整序列化 wire bytes。

复用一次缓存同时省去：

1. 屏幕捕获。
2. PNG 编码。
3. Packet 构造。
4. Packet 序列化。

连接发送状态仍然独立。两个连接可以引用内容相同的响应 bytes，但各自拥有自己的发送槽位、发送进度和背压结果。

### 19.5 两个截图 worker 不代表并行截图

`m_screenFrameCacheMutex` 的作用域覆盖：

```text
检查缓存
  → 必要时 captureScreenPng()
  → Packet 序列化
  → 更新缓存
```

因此：

```text
截图 worker A 发生缓存 miss
  → 持有全局缓存 mutex
  → 执行截图和 PNG 编码

截图 worker B 同时处理另一个连接
  → 等待同一个 mutex
  → A 完成后，B 再检查缓存
  → 通常直接复用 A 刚生成的帧
```

默认两个截图 worker 用于承载多个连接任务并衔接缓存复用，不表示底层 GDI 截图可以同时执行两次。

### 19.6 三个观看者的推演

假设缓存开始为空：

```text
t = 100 ms
连接 A 请求
  → 缓存 miss
  → 捕获帧，生成 ID 7
  → A.lastScreenFrameId = 7

t = 110 ms
连接 B 请求
  → 缓存年龄 10 ms
  → B 尚未收到 ID 7
  → B 复用 ID 7

t = 112 ms
连接 A 再次请求
  → 缓存年龄 12 ms
  → A 已收到 ID 7
  → A 不能复用
  → 捕获新帧，生成 ID 8

t = 115 ms
连接 C 请求
  → 新缓存年龄约 3 ms
  → C 尚未收到 ID 8
  → C 复用 ID 8
```

---

## 20. `completeScreenFrame()` 与双连接设计

### 20.1 发送排空后才完成一帧

关键代码：

```cpp
void RemoteControlTransport::Impl::completeScreenFrame(
    std::shared_ptr<ConnectionContext> const& _connection)
{
    bool submitNextFrame{false};
    {
        std::lock_guard<std::mutex> const lock{_connection->screenFrameMutex};
        if (_connection->screenFrameState ==
                ScreenFrameFlowState::FramePendingWithQueuedRequest &&
            !_connection->state.isTerminal())
        {
            _connection->screenFrameState = ScreenFrameFlowState::FramePending;
            submitNextFrame = true;
        }
        else
        {
            _connection->screenFrameState = ScreenFrameFlowState::Idle;
        }
    }

    if (submitNextFrame && !this->submitScreenFrame(_connection))
    {
        {
            std::lock_guard<std::mutex> const lock{_connection->screenFrameMutex};
            _connection->screenFrameState = ScreenFrameFlowState::Idle;
        }
        this->closeConnection(_connection, ConnectionCloseReason::TaskRejected);
    }
}
```

`completeScreenFrame()` 的参数：

| 参数 | 作用 |
| --- | --- |
| `_connection` | 当前帧发送项已经完全发完且发送队列已经排空的屏幕连接。 |

状态转换：

```text
FramePending
  + 当前帧发送排空
  → Idle

FramePendingWithQueuedRequest
  + 当前帧发送排空
  → FramePending
  → 锁外提交下一帧

任意状态
  + 连接已经 terminal
  → Idle，不再提交
```

如果合并帧的任务提交失败，代码先回滚 `Idle`，再以 `TaskRejected` 关闭连接。

### 20.2 屏幕与控制使用不同首包

服务端通过首包把两条连接分类：

```text
屏幕连接首包：
  WatchScreen
  → ConnectionPhase::ScreenStream

控制连接首包：
  ControlChannel
  → ConnectionPhase::ControlStream
```

客户端对应两个独立 worker 和两个独立 `QTcpSocket`：

| 客户端对象 | 连接用途 |
| --- | --- |
| `ScreenStreamWorker` | 发送 `WatchScreen`，接收 PNG 帧 |
| `ControlStreamWorker` | 先发送 `ControlChannel` 握手，再发送鼠标、锁定和解锁命令 |

### 20.3 一条 TCP 连接为什么不合适

TCP 是有序字节流。

假设屏幕帧和控制响应共用一条连接：

```text
发送队列头部：
  一个较大的 PNG 帧

发送队列后部：
  鼠标点击的很小状态响应
```

即使鼠标响应很小，也必须等待前面的 PNG bytes 发送完。这是同一有序字节流上的队头阻塞。

分成两条连接后：

```text
屏幕 socket：
  独立发送 PNG
  独立承受屏幕背压

控制 socket：
  独立发送小命令和状态
  不等待屏幕发送队列
```

### 20.4 两种业务的流控语义也不同

| 屏幕流 | 控制流 |
| --- | --- |
| 重复刷新请求可以合并 | 按下、释放、锁定等离散命令不能随意丢弃 |
| 只关心较新的画面 | 关心命令顺序 |
| 单帧 payload 较大 | 命令和状态通常较小 |
| 截图失败只应结束屏幕流 | 控制流可以独立继续或独立失败 |
| `screenStreamIdleTimeoutMs` | `controlStreamIdleTimeoutMs` |

如果强行共用连接，就需要在一个协议角色里同时维护两套不同流控规则，错误传播和超时也会互相影响。

两条连接带来的核心收益：

1. 避免大屏幕帧阻塞低延迟控制。
2. 为两种业务建立不同状态机和背压策略。
3. 让屏幕失败、重连和关闭不必破坏控制连接。
4. 让服务端分别限制屏幕流和控制流连接数。

---

## 21. 把三条业务链放在一起

### 21.1 三条链的共同结构与差异

- **Shell 链（`OneShot`）**
  - 任务池生产：执行一次 host service 调用，产生一个最终状态 Packet。
  - 发送排空：`closeAfterSend` 触发 `RequestComplete`。
- **文件链（`FileTransfer`）**
  - 任务池生产：每次最多生成 64 项目录或 64 KiB 下载数据。
  - 发送排空：`continueFileTransfer()` 继续下一批或正常关闭。
- **屏幕链（`ScreenStream`）**
  - 任务池生产：捕获或复用一帧，并产生一个帧 Packet。
  - 发送排空：`completeScreenFrame()` 执行一个合并请求或回到 `Idle`。

三条链都遵守同一边界：

```text
completion worker 只提交任务和推进发送状态
  → 业务 worker 生产有限响应后立即返回
  → enqueueBytes() 进入阶段七的有界发送链
```

文件必须保留每一批；屏幕只保留“当前帧之后再刷新一次”的意图；Shell 没有增量状态。

### 21.2 五把锁各自保护什么

- **`TaskPool::m_mutex`**
  - 保护：任务池停止标记和等待队列。
  - 允许：检查状态、入队、取得并移除队头。
  - 禁止：执行业务 task。
- **`fileTransferMutex`**
  - 保护：连接当前持有的传输状态指针。
  - 允许：安装、取得或清除状态指针。
  - 禁止：读取文件或枚举目录。
- **`screenFrameMutex`**
  - 保护：单连接帧流状态。
  - 允许：检查和转换三个状态。
  - 禁止：截图、编码或提交任务。
- **`m_screenFrameCacheMutex`**
  - 保护：全局帧缓存，并串行化实际捕获。
  - 允许：检查缓存；必要时截图、编码并更新缓存。
  - 禁止：混入无关的连接状态。
- **`sendMutex`**
  - 保护：单连接发送槽位、队列和字节计数。
  - 允许：容量准入和队列交接。
  - 禁止：文件读取、截图或等待网络。

注意 `m_screenFrameCacheMutex` 是一个有意扩大范围的例外。它覆盖截图和编码，是为了避免并发产生重复帧，并让等待任务优先复用刚生成的缓存。

### 21.3 阶段七与阶段八的衔接

| 阶段七机制 | 阶段八怎样使用 |
| --- | --- |
| `enqueueBytes()` 有界准入 | 文件批次和屏幕帧都从这里进入发送链 |
| 每连接一个发送槽位 | 一个文件批次或一帧不会与同连接其他响应并发写乱 |
| 部分发送继续同一 operation | 不会把一次部分完成误当成整个批次完成 |
| 等待项按 FIFO 交接 | 如果已有响应，业务续传会等全部等待项排空 |
| 队列排空事件 | 触发 `continueFileTransfer()` 或 `completeScreenFrame()` |
| `Backpressure` | 慢连接无法无限保留已生产 bytes |

阶段八没有重新实现发送队列，而是在发送队列的稳定“排空”事件上挂接业务续传。

---

## 22. 常见错误与失败入口

### 22.1 常见错误

#### 错误一：把等待容量当成总任务容量

`maximumQueuedFileTasks == 64` 不包含四个活动文件任务；极端时可以有 4 个活动任务和 64 个等待任务。

#### 错误二：让任务池锁覆盖业务执行或容量等待

业务 task 必须在 `m_mutex` 外执行；`submit()` 满队列时必须立即失败。否则普通任务池拥塞会阻塞其他 worker，甚至传播回 completion worker。

#### 错误三：异步任务保存了错误的生命周期

按引用捕获 `_packet` 可能在接收函数返回后失效；强持有连接又会延长无意义连接寿命。项目按值捕获 Packet，并通过 `weak_ptr::lock()` 临时取得连接。

#### 错误四：文件生产领先于网络消费

一次读取完整文件，或在 `enqueueBytes()` 成功后立即读取下一批，都会让慢客户端造成内存积压。正确续传点是前一批完全发完且发送队列排空以后。

#### 错误五：把 `finished` 当成客户端已收到

`finished` 在最终批次入队时变为 `true`；只有最终 WSASend 完整完成后，连接才能正常关闭。

#### 错误六：为每个屏幕请求都保存任务

屏幕只需要最新画面。项目保留一帧在途和一个追加意图，且 pending 一直持续到帧发送排空，不能在截图函数返回时提前改成 `Idle`。

#### 错误七：认为两个截图 worker 会并行截图

全局帧缓存 mutex 覆盖截图和编码；第二个 worker 通常等待后复用缓存，而不是并行捕获。

#### 错误八：屏幕与控制复用一条 TCP 连接

大 PNG 会阻塞后续控制响应，屏幕请求合并规则也不适用于必须保序的离散控制命令。

### 22.2 主要失败入口

| 位置 | 条件 | 结果 |
| --- | --- | --- |
| `scheduleFileRequest()` | 文件任务池满或停止 | `TaskRejected` |
| `continueFileTransfer()` | 下一批任务提交失败 | `TaskRejected` |
| `scheduleScreenFrame()` | 首帧任务提交失败 | 回滚 `Idle`，随后 `TaskRejected` |
| `completeScreenFrame()` | 合并帧任务提交失败 | 回滚 `Idle`，`TaskRejected` |
| `queueDirectoryBatch()` | Packet 序列化失败 | `InternalFailure` |
| `queueDirectoryBatch()` | 发送准入失败 | `Backpressure` |
| `queueDownloadChunk()` | 文件无法继续读取 | `IoFailure` |
| `queueDownloadChunk()` | bytes 为空或发送准入失败 | `Backpressure` |
| `makeScreenFramePacketBytes()` | 截图或序列化失败 | 返回空 bytes，调用方关闭连接 |
| `submitScreenFrame()` | 帧 bytes 为空或发送准入失败 | 当前实现关闭为 `Backpressure` |

---

## 23. 映射到项目源码

### 23.1 推荐阅读顺序

以下路径都相对于 `D:\CodeRepository\claude\remote_control`。

1. **先看对象、常量和配置**
   - `server_transport/include/RemoteControlTransport.h`
     - worker 数、等待容量和其他 transport 选项。
   - `server_transport/internal/RemoteControlTransportImpl.h`
     - `TaskPool`、`FileTransferState`、屏幕状态和成员声明。
2. **再看普通任务池**
   - `server_transport/src/RemoteControlTransportRuntime.cpp:139`
     - constructor、`submit()`、`runWorker()`。
3. **跟踪文件链**
   - `server_transport/src/RemoteControlTransportProtocol.cpp:86`
     - 文件首包分类与任务提交。
   - `server_transport/src/RemoteControlTransportFileTransfer.cpp:75`
     - 目录、下载、删除和续传。
   - `server_transport/src/RemoteControlTransport.cpp:591`
     - 发送完成后的业务分派。
4. **跟踪屏幕链**
   - `server_transport/src/RemoteControlTransportProtocol.cpp:283`
     - 截图提交、请求合并和共享缓存。
   - `server_transport/src/RemoteControlTransport.cpp:644`
     - 帧发送排空后的继续入口。
5. **最后核对客户端和测试**
   - `src/client/ScreenStreamWorker.cpp`
     - 客户端屏幕长连接。
   - `src/client/ControlStreamWorker.cpp`
     - 客户端控制长连接。
   - `tests/SmokeTests.cpp`
     - 多批目录、并发下载、慢下载和帧合并场景。

### 23.2 阅读文件时追踪六条线

- **线程线**：当前代码由 completion worker、文件 worker、Shell worker 还是截图 worker 执行。
- **输入线**：Packet、payload、transfer 怎样进入异步 lambda。
- **所有权线**：connection 使用 `weak_ptr` 还是 `shared_ptr`，transfer 由谁持有。
- **状态线**：`remainingBytes`、`finished`、`screenFrameState` 在何时变化。
- **发送线**：哪个函数调用 `enqueueBytes()`，何时由 send completion 继续。
- **失败线**：任务拒绝、背压、I/O 失败和正常完成分别怎样关闭。

---

## 24. 阶段练习与验收

所有任务都以纸面推演和源码阅读为主，不要求修改或构建项目。

### 24.1 任务一：给操作选择正确线程

为下列操作选择执行位置：

```text
A. 调用 GetQueuedCompletionStatus()
B. 把 recv bytes 追加到 receiveBuffer
C. 读取下载文件的下一段 64 KiB
D. 递归删除目录
E. 调用 captureScreenPng()
F. 更新 sendOffset
G. 调用 enqueueBytes()
H. 通过操作系统 Shell 打开文件
I. 解析已经完整到达的 Packet
J. 帧发送排空后决定是否提交下一帧
```

线程归类验收：

- A、B、F、I、J：completion worker。
- C、D：文件 worker。
- E：截图 worker。
- H：Shell worker。
- G：completion worker 或业务 worker 都可调用；它只做短状态准入和异步发送投递，不能等待网络。

还应能说明：执行位置由“是否可能阻塞、是否耗时不可预测”决定，不是由函数是否属于业务模块决定。

### 24.2 任务二：推演任务池容量与唤醒

给定：

```text
worker 数 = 2
maximumQueuedTasks = 3

worker 1 正在执行 A
worker 2 正在执行 B
等待队列依次保存 C、D、E
```

回答：

1. 此时有多少活动任务？
2. 有多少等待任务？
3. 有多少已接受但未完成任务？
4. 现在提交 F 会怎样？
5. A 完成并取走 C 后，再提交 F 会怎样？

验收要点：

```text
活动任务：2
等待任务：3
已接受未完成：5
第一次提交 F：false，因为等待队列已满
A 完成且 C 被取走后：等待队列降为 2，F 可以入队
```

必须明确：容量 3 只统计 C、D、E，不统计 A、B。

继续说明 `wait(lock, predicate)` 在以下三种状态下怎样执行：

唤醒验收：

| `m_stopping` | `m_tasks` | 结果 |
| --- | --- | --- |
| `false` | 空 | predicate 为 `false`，worker 释放 mutex 并等待 |
| `false` | 非空 | predicate 为 `true`，worker 取出队头任务 |
| `true` | 空 | predicate 为 `true`，随后满足“停止且队列空”，worker 返回 |

答案还需说明：predicate 防止虚假唤醒后访问空队列；task 在锁外执行，避免阻塞其他 worker 取任务和 `submit()` 入队。

### 24.3 任务三：检查异步 lambda 捕获生命周期

时间线：

```text
t0  handleInitialPacket() 收到一个 DownloadFile Packet
t1  scheduleFileRequest() 提交成功
t2  原 handleInitialPacket() 已经返回
t3  客户端断开，连接开始关闭
t4  文件 worker 才取到任务
```

回答：

1. `_packet` 为什么仍然有效？
2. 连接为什么不会被无意义任务强制保活？
3. worker 在 t4 应做什么？

验收要点：

```text
_packet 按值捕获，任务拥有自己的 Packet 副本
连接只通过 weak_ptr 捕获
worker 先 lock()，再检查 terminal 和 stopping
lock 失败或连接 terminal 时直接返回
```

### 24.4 任务四：推演 130 项目录

参考第 12.5 节的方法，但不要直接照抄其中的 131 项结果。独立写出：

1. 每个批次包含多少普通目录 Packet？
2. 哪一批设置 `finished = true`？
3. 终止 Packet 在哪里？
4. 一共多少发送批次和协议 Packet？
5. 连接何时关闭？

验收要点：

```text
批次一：64 个普通 Packet
批次二：64 个普通 Packet
批次三：2 个普通 Packet + 1 个终止 Packet

第三批设置 finished = true
共 3 个发送批次、131 个 Packet
第三批完整发完且发送队列排空后正常关闭
```

如果答案写成“第三批入队后立即关闭”，则没有通过。

### 24.5 任务五：推演 145 KiB 下载与空文件

参考第 13.4 节的方法，为 145 KiB 文件填写：

1. 第 1 批：是否有长度头、数据量、入队后剩余量、`finished`。
2. 第 2 批：是否有长度头、数据量、入队后剩余量、`finished`。
3. 第 3 批：是否有长度头、数据量、入队后剩余量、`finished`。

再说明 0 字节文件发送什么。

验收要点：

1. 第 1 批：有长度头，数据量为 64 KiB，入队后剩余 81 KiB，`finished == false`。
2. 第 2 批：没有长度头，数据量为 64 KiB，入队后剩余 17 KiB，`finished == false`。
3. 第 3 批：没有长度头，数据量为 17 KiB，入队后剩余 0，`finished == true`。

空文件只发送长度为 0 的头 Packet，第一批就标记 finished；仍要等头 Packet 发完再关闭。

### 24.6 任务六：解释慢下载公平性

场景：

```text
连接 A 下载一个大文件，但客户端读取很慢
连接 B 请求列出一个普通目录
文件池共有 4 个 worker
```

分别说明以下两种设计对 B 的影响：

```text
设计一：
  A 的 worker 循环读取完整文件，并等待发送队列有空间

设计二：
  A 每次只读取 64 KiB，入队后返回；
  前一批发完才提交下一任务
```

验收要点：

```text
设计一中，慢客户端会长期占住文件 worker；
多个慢客户端可以耗尽全部文件 worker。

设计二中，A 没有发送完成就不会产生下一文件任务；
worker 可以处理 B，A 的生产速度被网络消费速度限制。
```

### 24.7 任务七：推演屏幕请求与共享缓存

初始状态为 `Idle`，按下面时序推演：

```text
请求 1 到达
请求 2、3 在第一帧发送排空前到达
第一帧发送排空
请求 4 在第二帧 pending 期间到达
第二帧发送排空
第三帧发送排空
```

写出每一步的状态、是否提交截图任务，以及最终产生多少帧。

验收要点：

```text
请求 1：Idle → FramePending，提交 1 个任务
请求 2：FramePending → FramePendingWithQueuedRequest，不提交
请求 3：状态不变，不提交

第一帧排空：
  FramePendingWithQueuedRequest → FramePending
  提交第二帧

请求 4 在第二帧 pending 期间到达：
  FramePending → FramePendingWithQueuedRequest

第二帧排空：
  FramePendingWithQueuedRequest → FramePending
  提交第三帧

第三帧排空且没有新请求：
  FramePending → Idle

总计产生 3 帧
```

必须说明 pending 一直覆盖到发送队列排空，而不是截图函数返回。

继续推演共享缓存。给定：

```text
缓存帧 ID = 20
缓存年龄 = 10 ms

A.lastScreenFrameId = 20
B.lastScreenFrameId = 19
C.lastScreenFrameId = 0
```

A、B、C 依次请求。假设 A 先执行，并在重新截图后生成 ID 21；其他请求紧接着取得缓存锁。每条连接的结果应为：

```text
A 已收到 ID 20，不能复用，重新截图生成 ID 21
B 尚未收到 ID 21，并且缓存仍有效，复用 ID 21
C 尚未收到 ID 21，并且缓存仍有效，复用 ID 21
```

还应说明：全局缓存 mutex 让 B、C 等 A 完成，随后看到的是 A 刚更新的 ID 21。

### 24.8 任务八：解释双连接

场景：屏幕响应是一个较大的 PNG Packet，同时用户发出鼠标点击。

分别说明共用一条 TCP 连接和使用两条 TCP 连接时，鼠标响应会受到什么影响。

验收要点至少包括：

1. 同一 TCP 字节流严格有序，小控制响应不能越过前面的 PNG。
2. 屏幕请求允许合并，离散控制命令要求保序，两者流控语义不同。
3. 两条连接具有独立发送队列、背压、超时和失败范围。
4. 屏幕连接失败或重连时，不必同时销毁控制连接。

### 24.9 任务九：综合跟踪一条下载连接

从“`DownloadFile` 首包已经被 `Packet::tryParse()` 解析完成”开始，不看讲义流程图，按顺序写出函数名和执行线程。

一直写到最终连接以 `RequestComplete` 关闭。

验收时逐项检查：

1. **首包线程**：completion worker 调用 `handleInitialPacket()`、分类为 `FileTransfer`，再调用 `scheduleFileRequest()`。
2. **首批线程**：文件 worker 调用 `streamDownload()`、`queueDownloadChunk()` 和 `enqueueBytes()` 后返回。
3. **中间批次**：部分发送只继续同一 operation；完整发送并排空后，completion worker 才调用 `continueFileTransfer()`。
4. **最终关闭**：最终批次排空后再次进入 `continueFileTransfer()`，发现 `finished` 并以 `RequestComplete` 关闭。
5. **线程边界**：没有文件 worker 等待网络，也没有 completion worker 读取文件。

完成全部任务后，应能用一句话概括阶段八：

> completion worker 只推进网络与少量状态，普通任务池按有限批次生产响应，而发送队列排空决定文件下一批和屏幕下一帧何时开始。

---

## 25. 下一阶段衔接

阶段八建立了三个稳定边界：

```text
线程边界：
  completion worker 不执行不可预测的阻塞业务

生产边界：
  文件每次最多生产一批，屏幕每连接最多一帧在途并合并一个请求

容量边界：
  任务等待队列和每连接发送队列都可以立即拒绝过载
```

但连接、任务和内核 I/O 可以同时处于不同位置：

```text
连接已经开始关闭，文件任务仍在等待队列
文件 worker 正阻塞在同步读取
WSARecv 或 WSASend 已取消，但 completion 尚未被取走
任务池准备停止，业务 task 仍持有临时状态
completion worker 准备退出，pending I/O 仍未归零
```

阶段九将回答：

1. 谁取得一次性关闭资格。
2. 怎样阻止新 I/O 和新业务任务继续进入。
3. 怎样取消 socket 上的 pending I/O。
4. 怎样让同步文件操作尽快返回。
5. 为什么取消后仍必须消费最终 completion。
6. 为什么要先排空 pending I/O，再退出 completion worker 并关闭 completion port。

进入阶段九前，应能准确回答：

> 为什么“文件 worker 已经返回”不等于“文件批次已经发送完成”，而“调用了取消”也不会自动等于“内核再也不会返回 completion”？

---

## 26. 官方资料与项目资料

阅读官方资料时重点核对条件变量等待语义、固定线程对象、`weak_ptr` 的非拥有关系、文件流实际读取长度、`QDirIterator` 增量枚举和 IOCP completion dispatch。

- [Microsoft Learn：`condition_variable` class](https://learn.microsoft.com/en-us/cpp/standard-library/condition-variable-class?view=msvc-170)
- [Microsoft Learn：`thread` class](https://learn.microsoft.com/en-us/cpp/standard-library/thread-class?view=msvc-170)
- [Microsoft Learn：`weak_ptr` class](https://learn.microsoft.com/en-us/cpp/standard-library/weak-ptr-class?view=msvc-170)
- [Microsoft Learn：`basic_ifstream` class](https://learn.microsoft.com/en-us/cpp/standard-library/basic-ifstream-class?view=msvc-170)
- [Qt 6：`QDirIterator`](https://doc.qt.io/qt-6/qdiriterator.html)
- [Qt 6：`QByteArray`](https://doc.qt.io/qt-6/qbytearray.html)
- [Microsoft Learn：I/O Completion Ports](https://learn.microsoft.com/en-us/windows/win32/fileio/i-o-completion-ports)

以下项目资料路径相对于 `D:\CodeRepository\claude\remote_control`：

- `server_transport/include/RemoteControlTransport.h`
- `server_transport/include/RemoteControlHostServices.h`
- `server_transport/internal/RemoteControlTransportImpl.h`
- `server_transport/src/RemoteControlTransportRuntime.cpp`
- `server_transport/src/RemoteControlTransport.cpp`
- `server_transport/src/RemoteControlTransportProtocol.cpp`
- `server_transport/src/RemoteControlTransportFileTransfer.cpp`
- `src/client/ScreenStreamWorker.cpp`
- `src/client/ControlStreamWorker.cpp`
- `tests/SmokeTests.cpp`
