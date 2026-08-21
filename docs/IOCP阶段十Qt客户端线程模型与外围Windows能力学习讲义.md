# IOCP 阶段十：Qt 客户端线程模型与外围 Windows 能力学习讲义

> 前置知识：阶段九已经完成服务端 IOCP 的连接关闭、取消、排空和安全停机。
> 贯穿项目：`D:\CodeRepository\claude\remote_control`。
> 学习范围：Qt 事件循环、QObject thread affinity、跨线程调用、四种客户端连接模型、异步结果失效、worker 停机，以及项目使用的外围 Windows API。

---

## 1. 阶段十学习主线

阶段九回答了服务端怎样安全停机。阶段十把视角转到另外两端：

1. GUI 怎样在不阻塞界面的前提下发起网络请求。
2. 三个 Qt worker 怎样只在自己的线程操作 socket、timer 和文件。
3. 旧 endpoint、旧下载和旧流的异步结果怎样被丢弃。
4. 服务端业务接口怎样落到截图、鼠标、锁屏、UAC 和进程等待等 Windows 能力。

完整主线：

```text
GUI 线程
  → RemoteClient 选择连接模型
  → OneShotRequest 或 QueuedConnection
  → 对应 QTcpSocket 所在线程的事件循环
  → Packet 请求与响应
  → generation 过滤旧结果
  → signal 返回 GUI

服务端业务线程
  → RemoteControlHostServices 抽象接口
  → WindowsRemoteControlHostServices
  → WindowsPlatformIntegration / ScreenLockService
  → GDI、User32、Shell、Token、Process 等平台能力
```

建议按五个学习单元推进：

1. **建立 Qt 线程心智模型（第 4～10 节）**
   - 解决的问题：事件循环、thread affinity 和跨线程调用怎样配合。
   - 学完自检：能解释为什么不能从 GUI 线程直接调用 worker 的普通成员函数。
2. **理解客户端对象架构（第 11～17 节）**
   - 解决的问题：四种连接为什么使用不同的对象与连接生命周期。
   - 学完自检：能完整跟踪一次 `OneShotRequest` 的创建、响应和延迟删除。
3. **掌握三个常驻 worker（第 18～26 节）**
   - 解决的问题：generation、单帧在途、控制队列和事务下载怎样工作。
   - 学完自检：能说明 endpoint 改变后为什么旧 signal 不会污染新界面状态。
4. **理解外围 Windows 能力（第 27～32 节）**
   - 解决的问题：平台封装怎样管理 GDI、输入、模拟锁屏、UAC 和进程 handle。
   - 学完自检：能为每类 native resource 指出匹配的释放函数。
5. **源码复盘与综合验收（第 33～36 节）**
   - 解决的问题：把客户端、服务端业务线程和平台 API 串成端到端链路。
   - 学完自检：能从一次远程屏幕交互推演到服务端平台调用，再推演结果返回。

---

## 2. 知识范围

### 2.1 本阶段核心内容

- Qt 主事件循环与 worker thread 事件循环。
- QObject thread affinity 和 parent/child 约束。
- worker-object 模式与 `QThread` 对象本身的归属线程。
- `Qt::QueuedConnection` 与 `Qt::BlockingQueuedConnection`。
- `QObject::connect()` 和 `QMetaObject::invokeMethod()` 的参数。
- 异步 `QTcpSocket` 的连接、读取、正常断开和立即中止。
- `RemoteClient` 的四种连接模型。
- `OneShotRequest` 的 callback depth 与 `deleteLater()`。
- endpoint、download、screen stream 和 control stream generation。
- `ScreenStreamWorker` 的单帧在途规则。
- `ControlStreamWorker` 的握手、有序队列和鼠标移动合并。
- `FileDownloadWorker` 与 `QSaveFile` 的事务写入。
- worker 自己清资源、线程事件循环退出、调用线程 `wait()` 的顺序。
- `RemoteControlHostServices` 对平台能力的隔离。
- GDI 截图、鼠标输入、模拟锁屏、Shell/UAC、管理员判断和进程等待。

### 2.2 直接沿用的前置结论

以下内容已经在前面阶段建立，本阶段只使用结论：

1. TCP 是字节流，`readyRead` 不保证一次得到一个完整 Packet。
2. `Packet::tryParse()` 负责半包、粘包和完整帧提取。
3. 服务端根据首个命令把连接分类为 `OneShot`、`FileTransfer`、`ScreenStream` 或 `ControlStream`。
4. 服务端屏幕、文件和 Shell 慢任务不会阻塞 completion worker。
5. 服务端关闭连接后，客户端最终会收到断开或错误通知。
6. 服务端安全停机顺序已经由阶段九完成。

本阶段不重新推导 IOCP、`AcceptEx()`、`WSARecv()`、`WSASend()` 和 pending I/O 账本。

### 2.3 本阶段不展开的内容

- Qt 的全部 GUI 控件和布局系统。
- 自定义 `QThread::run()` 的线程子类模式。
- TLS、代理、自动重连和离线缓存。
- 多显示器截图拼接和高 DPI 坐标适配。
- Windows Session 0、服务账户和安全桌面切换。
- 真正的 Windows 会话锁定与身份认证协议。

---

## 3. 学习完成标准

完成本阶段后，应能做到：

1. 区分 GUI thread、`QThread` 对象和 `QThread` 管理的执行线程。
2. 解释 QObject thread affinity 怎样决定 queued callback 在哪里执行。
3. 说明带 parent 的 QObject 为什么不能随意 `moveToThread()`。
4. 逐项解释 `QObject::connect()` 和 `QMetaObject::invokeMethod()` 的参数。
5. 说明 `QueuedConnection` 为什么不会阻塞 GUI。
6. 说明 `BlockingQueuedConnection` 为什么可能卡住 GUI，并且同线程使用会死锁。
7. 解释 `QTcpSocket::connectToHost()` 为什么不等于连接已经建立。
8. 区分 `disconnectFromHost()`、`abort()` 和 `deleteLater()`。
9. 说明四种客户端连接模型分别解决什么问题。
10. 跟踪一个 `OneShotRequest` 从创建到安全删除的完整生命周期。
11. 解释嵌套事件循环为什么需要 `CallbackScope`。
12. 解释四种 generation 分别过滤什么旧结果。
13. 说明屏幕流为什么保持一条连接但一次只允许一帧在途。
14. 说明控制流为什么先握手，再保持一个命令等待响应。
15. 说明鼠标移动可以合并，但按下、释放和双击不能跨越合并。
16. 说明下载为什么使用 `QSaveFile`，以及项目为什么保留 `directWriteFallback == false`。
17. 逐步解释 worker shutdown、event loop quit 和 thread wait 的顺序。
18. 说明 GUI 对象为什么必须由 GUI thread 操作。
19. 解释 GDI 截图中 DC、bitmap、pixel pointer、`QImage` 和 `GdiFlush()` 的边界。
20. 逐项解释本阶段使用的关键 Win32 API 参数。
21. 说明项目“锁屏”为什么只是应用级模拟锁定，不是 Windows 安全边界。
22. 能从客户端请求一路追踪到 `WindowsPlatformIntegration`，再追踪结果返回。

建议投入 8～12 小时。

---

## 4. Qt 事件循环是异步客户端的执行引擎

### 4.1 GUI 主循环

客户端入口最终调用：

```cpp
return application.exec();
```

`QApplication::exec()` 没有参数。它进入 GUI 主事件循环，持续处理：

- 鼠标和键盘事件。
- 窗口绘制与关闭事件。
- timer 到期事件。
- socket 的异步通知。
- queued signal/slot 和 queued invoke callback。
- `deleteLater()` 产生的延迟删除事件。

它不是“一直占用 CPU 的死循环”。没有事件时，线程可以等待；事件到达后，Qt 再调用对应对象的方法。

### 4.2 worker thread 也需要事件循环

项目创建普通 `QThread` 并调用：

```cpp
thread->start();
```

`QThread::start()` 的可选参数是线程优先级；项目省略它，使用继承自创建线程的默认优先级。

普通 `QThread` 的默认 `run()` 会进入自己的事件循环。这个循环负责：

- 执行投递给 worker 的 queued callback。
- 驱动 worker thread 中的 `QTimer`。
- 驱动 worker thread 中的 `QTcpSocket`。
- 处理该线程对象的 `deleteLater()`。

没有事件循环时，queued callback 不会执行，timer 不会 timeout，异步 socket signal 也无法按当前设计推进。

### 4.3 GUI 不阻塞的根本原因

项目没有在 GUI 请求路径调用：

```text
waitForConnected：同步等待连接建立
waitForReadyRead：同步等待收到数据
waitForBytesWritten：同步等待发送缓冲区推进
waitForDisconnected：同步等待连接断开
```

这些函数会同步等待。项目改用：

```text
调用 connectToHost 后立即返回
  → connected signal 通知连接成功
  → 调用 write 后立即返回
  → readyRead signal 通知已有数据
  → readAll 取出当前可读字节
```

GUI 和 worker thread 都靠事件循环继续处理其他事件，而不是停在网络等待函数里。

---

## 5. QObject thread affinity 决定代码在哪里执行

### 5.1 thread affinity 是对象归属，不是 mutex

每个 QObject 都“生活”在一个线程中。可以通过：

```cpp
object->thread();
```

取得其归属线程。`thread()` 没有参数，返回管理该归属关系的 `QThread*`。

thread affinity 决定：

1. 投递给该对象的 queued callback 由哪个事件循环执行。
2. 该对象的 timer 和 socket 应在哪个线程使用。
3. `deleteLater()` 的删除事件由哪个线程处理。

它不自动给对象成员加锁。跨线程直接读写一个正在接收事件的 QObject，仍可能产生竞态。

### 5.2 默认归属线程

QObject 默认归属于创建它的线程：

```text
GUI thread 创建 RemoteClient
  → RemoteClient 属于 GUI thread

GUI thread 创建 ScreenStreamWorker
  → 初始也属于 GUI thread
```

项目随后移动 worker：

```cpp
this->m_screenStreamWorker->moveToThread(
    this->m_screenStreamThread);
```

`moveToThread()` 的唯一参数是目标 `QThread*`。调用成功后，worker 及其 QObject children 的 affinity 一起迁移到该线程。

### 5.3 为什么 worker 创建时不能有 GUI parent

QObject 有 parent 时，parent 负责其所有权，并要求 parent 与 child 属于同一线程。

项目这样创建 worker：

```cpp
new ScreenStreamWorker{}
```

没有传 `RemoteClient` 作为 parent，因此可以移动到 worker thread。

如果写成：

```cpp
new ScreenStreamWorker{remoteClient}
```

对象带有 GUI thread parent，`moveToThread()` 会失败，线程模型不成立。

### 5.4 child 怎样跟随 worker

`ScreenStreamWorker` 构造 timer 时传入 `this`：

```cpp
m_timeoutTimer{new QTimer{this}}
```

worker 移动时，timer 作为 child 一起移动。

`QTcpSocket` 则在 worker 已经开始接收 queued callback 后创建：

```cpp
this->m_socket = new QTcpSocket{this};
```

因此 socket 从创建开始就属于 worker thread。

---

## 6. `QThread` 对象不等于它管理的执行线程

### 6.1 项目使用 worker-object 模式

关键结构：

```cpp
QThread* const thread{new QThread{this}};
Worker* const worker{new Worker{}};
worker->moveToThread(thread);
thread->start();
```

这里有两个不同对象：

1. `QThread` QObject
   - 在 GUI thread 创建。
   - 自身 affinity 仍在 GUI thread。
   - 负责管理另一条执行线程。
2. `Worker` QObject
   - 初始在 GUI thread 创建。
   - 移动后属于新执行线程。
   - queued slot、timer 和 socket 都在该执行线程工作。

### 6.2 为什么不把业务 slot 写在 `QThread` 子类中

`QThread` 对象本身仍生活在创建它的 GUI thread。把普通 queued slot 写在 `QThread` 子类上，slot 不会因为 `run()` 在新线程执行就自动迁移。

项目把业务放在 `ScreenStreamWorker`、`ControlStreamWorker` 和 `FileDownloadWorker` 中，线程边界更明确：

```text
QThread 负责事件循环生命周期
Worker 负责网络和业务状态
RemoteClient 负责 GUI 入口与结果过滤
```

### 6.3 `QThread::finished` 与 worker 删除

项目连接：

```cpp
connect(
    thread,
    &QThread::finished,
    worker,
    &QObject::deleteLater);
```

四个实参分别是：

1. `thread`：signal sender。
2. `&QThread::finished`：线程事件循环结束时发出的 signal。
3. `worker`：接收删除请求的 QObject context。
4. `&QObject::deleteLater`：延迟删除函数。

Qt 在线程结束阶段仍处理 deferred-delete 事件，因此这是 worker-object 模式常用的收尾连接。

---

## 7. 跨线程 signal/slot 的四种连接语义

### 7.1 `QObject::connect()` 的五个位置参数

带显式连接类型的常见形式：

```cpp
QObject::connect(
    sender,
    signal,
    context,
    function,
    connectionType);
```

参数作用：

1. `sender`
   - 发出 signal 的 QObject。
2. `signal`
   - 指向具体 signal 的成员函数指针。
3. `context`
   - 决定 callback 生命周期，并为 queued connection 提供目标 thread affinity。
4. `function`
   - signal 到达后执行的 slot 或 lambda。
5. `connectionType`
   - 决定立即调用、排队调用还是阻塞等待。

返回值是 `QMetaObject::Connection`，可用于之后精确断开该连接。项目多数连接依赖 QObject 生命周期自动断开，不保存返回值。

### 7.2 `Qt::AutoConnection`

默认类型。signal 发出时，Qt 比较当前执行线程与 receiver/context 的 affinity：

- 相同：表现为 direct call。
- 不同：表现为 queued call。

判断发生在 signal 发出时，而不是 `connect()` 建立时。

### 7.3 `Qt::DirectConnection`

slot 立即在 signal 发出线程执行。

如果 sender 和 receiver 分属不同线程，直接执行可能在错误线程访问 receiver 成员，因此项目跨线程业务不使用它。

### 7.4 `Qt::QueuedConnection`

Qt 把调用包装成事件，投递到 context 所在线程：

```text
GUI thread 发起调用
  → callback 进入 worker event queue
  → GUI 立即继续
  → worker thread 事件循环取出 callback
  → worker 成员函数在正确线程执行
```

这是项目从 `RemoteClient` 调用三个 worker 的主要方式。

### 7.5 `Qt::BlockingQueuedConnection`

它也把 callback 投递到 receiver thread，但 sender thread 会阻塞，直到 callback 返回。

风险：

1. GUI thread 使用它会冻结界面。
2. receiver 又等待 GUI 时会形成跨线程死锁。
3. sender 与 receiver 在同一线程时会直接死锁。

项目业务请求不使用它。析构时需要等待 worker 完全退出，项目使用 queued shutdown 加 `QThread::wait()`，把“在哪个线程清资源”和“谁等待完成”分开。

---

## 8. `QMetaObject::invokeMethod()`：把函数投递到对象线程

项目常用形式：

```cpp
bool const queued{QMetaObject::invokeMethod(
    worker,
    [worker, host, port] {
        worker->requestFrame(host, port, generation);
    },
    Qt::QueuedConnection)};
```

### 8.1 三个参数

1. **`worker` context**
   - 指定 callback 的目标对象。
   - 其 thread affinity 决定 queued callback 在哪个线程执行。
   - context 被销毁后，尚未执行的调用不会继续访问它。
2. **lambda/function**
   - 真正要执行的代码。
   - 跨线程排队时，捕获值必须在执行时仍然有效。
   - 项目复制 host、port、generation 和必要 payload，避免引用 GUI 临时变量。
3. **`Qt::QueuedConnection`**
   - 指定异步排队。
   - 调用线程不等待 worker 执行完成。

返回值为 `bool`：成功排入目标调用上下文时为 `true`，无法排队时为 `false`。

### 8.2 为什么不能直接调用 worker

错误写法：

```cpp
this->m_screenStreamWorker->requestFrame(
    host,
    port,
    generation);
```

普通 C++ 调用永远在当前线程执行。即使对象已 `moveToThread()`，这行代码仍会在 GUI thread 进入 `requestFrame()`，随后错误地操作 worker thread 的 timer 和 socket。

正确写法不是“因为 Qt 自动加锁”，而是把执行权交给目标事件循环。

### 8.3 为什么捕获裸 worker 指针仍可接受

项目保证：

```text
RemoteClient 存活
  → worker thread 与 worker 存活
  → 可以继续投递业务调用

RemoteClient 析构
  → 不再接受外部业务调用
  → queued 投递 worker shutdown
  → wait 等待线程结束
  → worker 通过 deleteLater 销毁
```

此外，`invokeMethod()` 的 context 就是该 worker；worker 已失效时，调用不会正常排入其事件循环。

---

## 9. 异步 `QTcpSocket` 的最小模型

### 9.1 `connectToHost()` 的四个参数

函数常用原型：

```cpp
void connectToHost(
    QString const& hostName,
    quint16 port,
    QIODevice::OpenMode openMode = QIODevice::ReadWrite,
    QAbstractSocket::NetworkLayerProtocol protocol =
        QAbstractSocket::AnyIPProtocol);
```

参数作用：

1. `hostName`
   - 主机名或 IP 地址字符串。
2. `port`
   - 目标 TCP 端口。
3. `openMode`
   - socket 的读写模式；项目省略，使用 `ReadWrite`。
4. `protocol`
   - IPv4、IPv6 或自动选择；项目省略，使用 `AnyIPProtocol`。

函数没有“连接成功”返回值。调用后 socket 进入主机解析或连接状态，最终通过 signal 报告：

```text
connected：连接建立成功
errorOccurred：连接或传输发生错误
disconnected：连接已经断开
```

### 9.2 `write()` 的参数与返回值

项目调用：

```cpp
qint64 const acceptedBytes{socket->write(bytes)};
```

- `bytes`：要追加到 Qt 写缓冲区的字节数组。
- 返回非负值：Qt 接受进入本地写缓冲区的字节数。
- 返回 `-1`：调用失败。

返回值不表示远端已经收到，也不表示服务端已经处理完成。

### 9.3 `readyRead` 与 `readAll()`

`readyRead` 没有参数，只表示“当前有新字节可读”。

`readAll()` 没有参数，返回当前 Qt 读缓冲区中的全部可读字节。

项目必须继续使用阶段二、阶段三建立的 TCP 累积模型：

```cpp
this->m_buffer.append(this->m_socket->readAll());
auto const packet{
    remote_control::Packet::tryParse(this->m_buffer)};
```

一次 `readyRead` 可能只有半个 Packet，也可能包含多个 Packet。

### 9.4 四个 signal 的职责

1. `connected()`
   - TCP 连接已建立，可以发送协议首包。
2. `readyRead()`
   - 新字节已经进入 Qt 读缓冲区。
3. `errorOccurred(_error)`
   - `_error` 是 `QAbstractSocket::SocketError`，指出 socket 错误类别。
4. `disconnected()`
   - 连接已经进入未连接状态。

同一次远端关闭可能先触发 `errorOccurred(RemoteHostClosedError)`，再触发 `disconnected()`。失败入口必须幂等，不能报告两次最终结果。

---

## 10. 正常断开、中止连接和延迟删除

### 10.1 `disconnectFromHost()`

没有参数。

作用：请求正常断开。Qt 会先尝试发送本地写缓冲区中尚未写出的数据，然后进入关闭状态，最终发出 `disconnected()`。

`OneShotRequest` 在成功收到完整响应后使用它，因为请求已经完成，不需要立即丢弃尚未送出的本地数据。

### 10.2 `abort()`

没有参数。

作用：立即中止连接并丢弃待写数据。

项目在以下场景使用：

- 请求失败。
- endpoint 已经过期。
- worker 主动关闭持久连接。
- 下载取消。
- shutdown 清理。

### 10.3 `deleteLater()`

没有参数。

它不在当前调用栈立即 `delete this`，而是向对象所属线程投递 deferred-delete 事件。

为什么适合 QObject：

1. 避免正在执行 signal/slot 时立即销毁对象。
2. 让析构发生在对象 affinity 所在线程。
3. 让 Qt 先退出当前回调，再执行删除。

但是 `deleteLater()` 也不是“任何时候都绝对安全”。如果嵌套事件循环在当前回调尚未退出时处理 deferred-delete，就仍需要项目的 `CallbackScope` 进一步保护，第 17 节将专门解释。

### 10.4 `disconnect()` 只断 signal，不关闭网络

项目 reset socket 时先调用：

```cpp
QObject::disconnect(socket, nullptr, receiver, nullptr);
```

四个参数分别表示：

1. `socket`：只匹配这个 sender。
2. 第一个 `nullptr`：匹配它的全部 signal。
3. `receiver`：只移除指向这个 receiver 的连接。
4. 第二个 `nullptr`：匹配 receiver 的全部 slot/functor。

它只阻止 teardown 期间继续进入旧 callback；真正关闭连接仍由 `abort()` 完成。

---

## 11. 客户端的四种连接模型

项目不是“每种请求都开线程”，也不是“全部请求共用一个 socket”。它按业务响应模式选择四种连接生命周期。

### 11.1 单请求短连接

实现：`OneShotRequest`。

用于：

- `TestConnection`。
- `ListDrives`。
- `ListDirectory`。
- `RunFile`。
- `DeleteFile`。

生命周期：

```text
创建一个 QObject 和一个专用 QTcpSocket
  → 异步连接
  → 发送一个首包
  → 收集单包或多包响应
  → 报告一次最终结果
  → 断开并 deleteLater
```

它位于 GUI thread，但全程使用异步 socket，不同步等待网络。

### 11.2 单下载专用连接

实现：常驻 `FileDownloadWorker`。

每次下载创建一个专用 socket 和一个 `QSaveFile`；连接结束后，worker thread 继续存活，可接收下一次下载。

为什么不放在 `OneShotRequest`：

- 下载持续时间可能很长。
- 本地文件写入不应占用 GUI thread。
- 下载需要独立 progress 和 cancel generation。
- `QSaveFile` 应始终由同一个 worker thread 使用。

### 11.3 屏幕持久连接

实现：`ScreenStreamWorker`。

连接可以跨多帧复用，但一次只发送一个 `WatchScreen` 请求：

```text
Idle（没有帧请求）
  → FramePending（已有一帧在途）
  → 收到并解码一帧
  → Idle（允许请求下一帧）
```

下一帧由 GUI timer 在上一帧完成后安排，避免客户端无限堆积画面请求。

### 11.4 控制持久连接

实现：`ControlStreamWorker`。

连接先执行 `ControlChannel` 握手，再按顺序发送鼠标、锁定和解锁命令。只有一个命令等待响应，其余命令留在有界队列。

### 11.5 客户端连接模型不等于服务端 role 名称

例如目录请求在客户端由 `OneShotRequest` 管理，但服务端会把 `ListDirectory` 分类为 `FileTransfer`，因为服务端需要分批生产目录项。

因此要分开两个问题：

```text
客户端对象何时创建和销毁？
服务端连接进入哪个业务 phase？
```

---

## 12. `RemoteClient` 是 GUI 与网络实现之间的边界

### 12.1 它不直接处理所有网络字节

`RemoteClient` 的职责是：

1. 保存当前 host 和 port。
2. 为短请求创建 `OneShotRequest`。
3. 把屏幕、控制和下载调用投递到对应 worker。
4. 保存四种 generation。
5. 过滤过期结果。
6. 把仍有效的结果重新发成 GUI signal。
7. 析构时停止三个 worker thread。

协议解析和资源状态分别留在具体 request/worker 中，避免一个巨型客户端类同时维护所有连接。

### 12.2 `setEndpoint(_host, _port)` 参数

接口：

```cpp
void setEndpoint(
    QString const& _host,
    quint16 _port);
```

- `_host`：后续请求使用的服务器主机名或 IP 地址。
- `_port`：后续请求使用的服务器 TCP 端口。

它只更新客户端目标，不同步等待旧请求全部结束。旧异步结果由 generation 过滤；三个常驻 worker 则收到关闭或取消请求。

### 12.3 GUI 只调用业务接口

GUI 调用：

```text
testConnection：测试服务器连接
requestDriveList：请求磁盘列表
requestDirectoryListing：请求目录内容
openRemoteFile：请求服务端打开文件
deleteRemotePath：请求删除远程路径
downloadRemoteFile：下载远程文件
requestScreenFrame：请求一帧远程画面
sendMouseEvent：发送远程鼠标事件
lockRemote / unlockRemote：锁定或解锁远程界面
```

GUI 不直接持有 worker socket，也不直接调用 `readAll()`。这样 thread affinity 和结果过滤集中在 `RemoteClient` 边界内。

---

## 13. 三个 worker 怎样创建、移动和启动

### 13.1 构造顺序

`RemoteClient` 构造时创建：

```cpp
m_screenStreamThread{new QThread{this}},
m_screenStreamWorker{new ScreenStreamWorker{}},
m_controlStreamThread{new QThread{this}},
m_controlStreamWorker{new ControlStreamWorker{}},
m_fileDownloadThread{new QThread{this}},
m_fileDownloadWorker{new FileDownloadWorker{}}
```

所有 `QThread` 以 `RemoteClient` 为 parent，最终由 GUI 对象所有权释放。

三个 worker 没有 parent，之后分别移动到对应线程。

worker 构造函数会先创建 `QTimer{this}`。timer 是 worker 的 child，因此 `moveToThread()` 会把 worker 和 timer 一起迁移；各 worker 的 socket 则在 queued 业务调用中延迟创建，此时代码已经运行在目标线程。

```text
GUI thread 构造 worker 与 child timer
  → moveToThread 同时迁移二者
  → worker thread 开始事件循环
  → queued 业务入口延迟创建 socket
```

所以规则不是“所有 QObject 都必须在移动后创建”，而是：带 parent 的 child 必须与 parent 保持同一 affinity，活动 socket/timer 只能从它们所属线程启动和操作。

### 13.2 `qRegisterMetaType<T>()`

项目调用：

```cpp
qRegisterMetaType<remote_control::Command>();
```

函数没有运行时参数；模板参数 `remote_control::Command` 指定要注册到 Qt meta-object system 的 C++ 类型。

跨线程 queued signal 需要 Qt 能复制参数。注册后，`Command` 可以安全出现在跨线程 signal/slot 参数中。

### 13.3 正确顺序

```text
创建 thread 和 worker
  → moveToThread：指定 worker 执行线程
  → 连接 finished：线程结束后延迟删除 worker
  → 连接 worker result：结果回到 RemoteClient
  → start：启动线程与事件循环
```

先建立连接再 start，可以避免线程刚开始工作时生命周期回调尚未准备好。

### 13.4 worker result 为什么回到 GUI thread

worker signal 的 context 是 `RemoteClient`：

```cpp
connect(worker, &Worker::result, this, [this](...) {
    emit this->resultForUi(...);
});
```

worker 与 `RemoteClient` 属于不同线程，默认 `AutoConnection` 在发出时表现为 queued connection。lambda 最终在 `RemoteClient` 所属 GUI thread 执行，因此可以安全更新其 generation 和 GUI-facing state。

---

## 14. `stopWorkerThread()`：先让 worker 自己清资源，再等待线程

项目 helper：

```cpp
template <typename Worker>
void stopWorkerThread(
    QThread* const _thread,
    Worker* const _worker);
```

### 14.1 两个参数

1. `_thread`
   - 管理目标 worker 执行线程的 `QThread*`。
   - 调用者最终对它执行 `wait()`。
2. `_worker`
   - 生活在目标线程中的 worker QObject。
   - 它必须先在自己的线程执行 `shutdown()`。

### 14.2 关键实现

```cpp
if (!_thread->isRunning())
{
    return;
}

bool const shutdownQueued{QMetaObject::invokeMethod(
    _worker,
    [_worker] {
        _worker->shutdown();
        QThread::currentThread()->quit();
    },
    Qt::QueuedConnection)};

if (!shutdownQueued)
{
    _thread->quit();
}
_thread->wait();
```

### 14.3 `isRunning()`、`currentThread()`、`quit()` 和 `wait()`

- `isRunning()`：没有参数；线程已经启动且尚未结束时返回 `true`。
- `QThread::currentThread()`：没有参数；返回当前正在执行 callback 的线程对象。
- `quit()`：没有参数；请求当前线程事件循环以返回码 0 退出。
- `wait()`：项目不传参数，调用线程一直等待目标线程完全结束；成功结束时返回 `true`。

### 14.4 为什么 shutdown 与 quit 放在同一个 queued callback

错误的两段式顺序：

```text
从 GUI thread 直接 worker.shutdown
  → 跨线程操作 socket

或先 thread.quit
  → worker event loop 停止
  → queued shutdown 再也没有机会执行
```

项目把两步放在目标线程的同一个 callback：

```text
worker thread 执行 shutdown
  → timer 停止
  → abort socket，并通过 deleteLater 延迟销毁
  → 临时文件取消
  → 当前 worker thread 请求自己的 event loop 退出
  → GUI/destructor thread 调用 wait 等待结束
```

### 14.5 为什么 `finished → deleteLater` 仍然有效

项目还连接了：

```cpp
connect(
    thread,
    &QThread::finished,
    worker,
    &QObject::deleteLater);
```

`finished()` 没有参数，在 managed thread 即将结束时发出。此时普通 event 已不再处理，但 Qt 明确保留 deferred-deletion event 的处理，因此这个惯用连接可以销毁生活在该线程的 worker。

这不是“quit 后 queued callback 仍会照常执行”。普通 shutdown callback 必须在 `quit()` 前运行；`finished → deleteLater` 依赖的是 Qt 对 deferred deletion 的专门保证。

### 14.6 queued 失败的 fallback

`invokeMethod()` 返回 `false` 时，项目直接 `_thread->quit()`，保证线程收尾不会永久卡住。

正常路径仍应让 worker 自己清理资源；fallback 只处理目标调用上下文已经不可用的异常边界。

---
## 15. `OneShotRequest`：一次请求只管理一次完整往返

连接测试、磁盘列表、目录列表、运行文件和删除文件都属于短请求：发出一个命令，收到完整结果后就结束。项目没有让这些请求共用长连接，而是为每次操作创建一个 `OneShotRequest`。

### 15.1 构造函数与七个参数

关键接口：

```cpp
OneShotRequest(
    RemoteClient* _client,
    QString const& _host,
    quint16 _port,
    quint64 _generation,
    remote_control::Command _command,
    QByteArray _payload,
    QString _context);
```

各参数的作用如下。

1. `_client`
   - 接收结果的 `RemoteClient`。
   - 同时作为 `QObject` parent，保证客户端销毁时不会遗留短请求对象。
2. `_host`
   - 本次请求使用的服务器主机名或 IP 地址。
   - 它是发起请求时取得的副本，后续修改界面中的地址不会改变本次连接目标。
3. `_port`
   - 本次请求使用的 TCP 端口。
4. `_generation`
   - 请求启动时捕获的 endpoint generation。
   - 回调到达时用它判断结果是否仍属于当前服务器。
5. `_command`
   - 本次请求的协议命令，例如 `ListDrives` 或 `DeleteFile`。
   - 响应中的命令必须与它一致。
6. `_payload`
   - 命令携带的二进制数据。
   - 没有参数的命令传空数组；路径类命令传 UTF-8 编码后的路径。
7. `_context`
   - 供结果信号和错误信息使用的业务上下文。
   - 它可以是路径，也可以是“连接测试”这类操作名称，不参与协议解析。

构造函数还完成三件事：

```cpp
this->m_socket.setParent(this);
this->m_timeoutTimer->setSingleShot(true);
this->m_timeoutTimer->setInterval(RequestInactivityTimeoutMs);
```

- socket 和 timer 都归请求对象所有。
- 它们与请求对象位于同一个 GUI thread，不需要额外 worker thread。
- timer 是单次触发的 inactivity timeout；每次连接成功或读到新数据都会重新开始计时。

### 15.2 `start()` 为什么只负责启动连接

关键实现：

```cpp
void start()
{
    this->m_timeoutTimer->start();
    this->m_socket.connectToHost(this->m_host, this->m_port);
}
```

`start()` 没有参数，因为连接所需信息已经在构造时保存。它不会同步等待连接结果：

```text
调用 start
  → 启动超时计时
  → 请求异步连接
  → 立即返回 GUI event loop
  → connected signal 到达，表示连接成功
  → onConnected 开始发送协议包
```

连接建立后才发送数据：

```cpp
void onConnected()
{
    CallbackScope const scope{this};
    this->m_timeoutTimer->start();

    remote_control::Packet const packet{
        this->m_command,
        this->m_payload};
    this->m_socket.write(packet.serialize());
}
```

这里的 `QTcpSocket::write(QByteArray const& data)` 只有一个参数：

- `data`：需要加入 socket 发送缓冲区的字节。

返回值表示接受写入缓冲区的字节数，负数表示立即失败。它不表示对端已经收到，更不表示命令已经执行完成。

### 15.3 五类操作如何复用同一个对象

创建连接测试请求：

```cpp
auto* const request{new OneShotRequest{
    this,
    this->m_host,
    this->m_port,
    this->m_endpointGeneration,
    remote_control::Command::TestConnection,
    {},
    tr("Connection test")}};

request->start();
```

创建目录请求时只有 command、payload 和 context 不同：

```cpp
auto* const request{new OneShotRequest{
    this,
    this->m_host,
    this->m_port,
    this->m_endpointGeneration,
    remote_control::Command::ListDirectory,
    remote_control::encodeUtf8(_path),
    _path}};
```

复用的不是“结果处理细节”，而是共同的传输生命周期：连接、发送、增量读取、超时、断开与自清理。

---

## 16. `OneShotRequest` 的解析状态与完成边界

短连接不等于一次 `readyRead` 就能得到一个完整响应。TCP 仍然可能拆包、合包，目录列表还可能由多个协议包组成。

### 16.1 `readyRead` 只表示当前有字节可读

关键循环：

```cpp
this->m_buffer.append(this->m_socket.readAll());

while (true)
{
    auto const packet{
        remote_control::Packet::tryParse(this->m_buffer)};
    if (!packet.has_value())
    {
        break;
    }

    this->handlePacket(packet.value());
    if (this->hasFinished())
    {
        break;
    }
}
```

这段代码需要按四步理解：

1. `readAll()` 取出 Qt 当前已经收到的全部字节。
2. 新字节追加到旧缓冲区，保留上次未组成完整包的尾部。
3. `tryParse()` 每次最多取出一个完整协议包，并从缓冲区移除已消费字节。
4. 没有完整包时退出，等待下一次 `readyRead`。

`QTcpSocket::readAll()` 没有参数，返回当前可读的全部字节。它不保证返回一个完整应用层协议包。

### 16.2 收到包后先验证 command

```cpp
if (_packet.command != this->m_command)
{
    this->fail(tr("Received an unexpected command."));
    return;
}
```

`handlePacket(remote_control::Packet const& _packet)` 的参数：

- `_packet`：已经通过长度与协议格式检查的响应包。

即使 TCP 连接正常，只要响应 command 与请求不一致，这次业务往返就已经失去对应关系，不能把 payload 猜测成当前命令的结果。

### 16.3 单包结果与多包结果的完成条件不同

连接测试、磁盘列表、运行文件和删除文件在一个有效响应后即可完成：

```cpp
this->markFinished();
emit this->m_client->connectionTested(
    true,
    tr("Connection succeeded."));
this->m_socket.disconnectFromHost();
```

目录列表则需要持续积累 entry，直到收到协议定义的结束包：

```text
目录 entry 1
  → 保存，不完成

目录 entry 2
  → 保存，不完成

目录结束包 terminating packet
  → 标记完成
  → 一次性发出完整列表
```

如果连接在 terminating packet 到达前断开，不能把已有条目当成成功结果，否则界面会展示一个无法判断是否完整的目录。

### 16.4 为什么必须先 `markFinished()` 再发结果 signal

安全顺序：

```cpp
this->markFinished();
emit this->m_client->driveListFinished(...);
this->m_socket.disconnectFromHost();
```

结果 signal 连接的 UI slot 可能立即执行，并可能弹出对话框或触发新的操作。发 signal 前先进入 `Finished`，后续到达的 socket error、disconnect 或 timeout 才不会再次报告结果。

`markFinished()` 的职责非常集中：

```cpp
this->m_state = RequestState::Finished;
this->m_timeoutTimer->stop();
```

它把“业务结果已经唯一确定”与“socket 是否已经物理断开”分开了。

### 16.5 四个请求状态

```cpp
enum class RequestState
{
    Active,
    Finished,
    CleanupDeferred,
    DeletionScheduled,
};
```

- `Active`：仍可解析和产生业务结果。
- `Finished`：结果已确定，只剩传输清理。
- `CleanupDeferred`：已经请求清理，但当前仍在 Qt callback 调用栈中。
- `DeletionScheduled`：已经调用 `deleteLater()`，不能再次安排删除。

这里没有用多个 `bool`，因为四个状态是互斥的。状态枚举能直接阻止“既完成又仍 active”一类矛盾组合。

### 16.6 旧 endpoint 的结果为什么静默丢弃

回调开始和处理每个包之前都会检查 generation：

```cpp
if (!this->isCurrentGeneration())
{
    this->discardStaleResult();
    return;
}
```

`discardStaleResult()` 不发失败 signal，而是：

```text
标记完成
  → abort 旧连接
  → 安排对象删除
```

用户已经切换到新服务器时，“旧服务器请求失败”不再是当前界面的有效错误。静默丢弃避免旧结果覆盖新状态，也避免无意义的错误提示。

---

## 17. `CallbackScope`：防止嵌套事件循环中的过早删除

`deleteLater()` 通常适合清理 `QObject`，但短请求还有一个更隐蔽的条件：发出的 UI signal 可能在当前 callback 尚未返回时进入嵌套 event loop。

### 17.1 嵌套 event loop 从哪里出现

常见场景是 UI slot 弹出 modal dialog：

```text
进入 OneShotRequest::onReadyRead
  → emit requestFinished，通知 UI
  → UI slot 显示 modal dialog
  → dialog 内部启动 nested event loop
  → 当前 onReadyRead 调用栈尚未退出
```

如果对象清理完全依赖“稍后删除”的直觉，就可能在复杂事件嵌套中让 deferred-delete event 比预期更早被处理。项目因此显式记录 callback 深度。

### 17.2 RAII guard 的参数与行为

```cpp
class CallbackScope final
{
public:
    explicit CallbackScope(OneShotRequest* _request)
        : m_request{_request}
    {
        ++this->m_request->m_callbackDepth;
    }

    ~CallbackScope()
    {
        --this->m_request->m_callbackDepth;
        if (this->m_request->m_callbackDepth == 0 &&
            this->m_request->m_state ==
                RequestState::CleanupDeferred)
        {
            this->m_request->requestDeletion();
        }
    }

private:
    OneShotRequest* m_request{nullptr};
};
```

构造函数只有一个参数：

- `_request`：当前进入 Qt callback、需要记录调用深度的请求对象。

每个 socket/timer callback 一开始都在栈上创建 guard。无论函数从哪个分支返回，析构函数都会减少深度。

### 17.3 `requestDeletion()` 不是立即 `delete`

```cpp
void requestDeletion()
{
    if (this->m_state == RequestState::DeletionScheduled)
    {
        return;
    }

    if (this->m_callbackDepth > 0)
    {
        this->m_state = RequestState::CleanupDeferred;
        return;
    }

    this->m_state = RequestState::DeletionScheduled;
    this->deleteLater();
}
```

它没有参数，处理当前请求自身的删除调度。

三个分支分别保证：

1. 已安排删除时保持幂等。
2. callback 尚未完全退栈时只记录意图。
3. 深度归零后才投递 deferred-delete event。

这里不能改为 `delete this`。socket signal 正在调用该对象的方法时立即销毁自身，会让后续栈帧继续访问已经释放的成员。

### 17.4 应掌握的通用结论

`QObject` 生命周期不能只看“谁是 parent”，还要看三个边界：

- 当前对象属于哪个线程。
- 当前是否位于该对象的方法调用栈中。
- 发出的 signal 是否可能运行可重入代码或 nested event loop。

`CallbackScope` 解决的是第三个边界带来的延迟清理问题，不替代 parent ownership，也不替代 generation 过滤。

---

## 18. generation：给异步结果附上业务时代编号

异步任务开始后，用户可以修改地址、关闭远程屏幕或发起下一次下载。socket 回调仍可能晚到，因此项目使用单调递增的 generation 判断结果是否仍属于当前业务状态。

### 18.1 generation 不是什么

generation 不是：

- TCP 连接编号；
- 操作系统 thread ID；
- 时间戳；
- 用于互斥的锁；
- 要发送给服务端的协议字段。

它只是客户端本地的逻辑版本号：任务启动时复制当前值，回调时与最新值比较。

### 18.2 endpoint generation 的检查函数

```cpp
bool isEndpointGenerationCurrent(
    quint64 _generation) const noexcept
{
    return _generation == this->m_endpointGeneration;
}
```

参数：

- `_generation`：异步操作启动时捕获的 endpoint generation。

返回 `true` 表示操作仍属于当前 host/port；返回 `false` 表示用户已经切换 endpoint。

### 18.3 为什么捕获值，而不是回调时重新读取

正确做法：

```cpp
quint64 const generation{
    this->m_screenStreamGeneration};

QMetaObject::invokeMethod(
    this->m_screenStreamWorker,
    [worker = this->m_screenStreamWorker,
     host,
     port,
     generation] {
        worker->requestFrame(host, port, generation);
    },
    Qt::QueuedConnection);
```

lambda 按值捕获 host、port 和 generation，三者共同描述“提交这一刻的任务”。如果 worker 执行时才访问 `RemoteClient` 的最新字段，旧任务会错误地伪装成新任务。

### 18.4 四类 generation 各自保护什么

项目使用四个互不替代的编号：

1. endpoint generation
   - 保护一次性请求和下载所属的服务器地址。
2. download generation
   - 区分同一 endpoint 上先后发起的下载。
3. screen stream generation
   - 区分先后打开或关闭的屏幕帧流。
4. control stream generation
   - 区分先后建立的控制命令流。

如果只保留 endpoint generation，在同一服务器上连续下载 A、B 时，A 的晚到进度仍会被误认为当前结果。不同业务生命周期必须有各自的 generation。

### 18.5 generation 为什么没有原子类型

`RemoteClient` 的 generation 字段只在 GUI thread 中修改和比较。worker 发回 signal 后，连接到 `RemoteClient` 的 lambda 也在 GUI thread 执行。

因此安全性来自 thread affinity 和 queued delivery，而不是 `std::atomic`。如果将来在 worker thread 直接读取这些字段，当前设计前提就被破坏，不能仅靠“读一个整数通常没问题”来解释。

---

## 19. generation 的返回门：先过滤，再交给 GUI

第 18 节说明了 generation 怎样随任务一起提交。本节只看返回方向：worker 不判断“自己是否仍然有效”，而是把启动时收到的 generation 原样带回，最终由 GUI thread 中的 `RemoteClient` 决定是否转发。

### 19.1 worker 只回传任务身份

屏幕 worker 的结果信号可概括为：

```cpp
void frameReady(
    quint64 _generation,
    QImage const& _image);
```

- `_generation`：本帧请求启动时收到的 screen stream generation。
- `_image`：已经解码完成、可交给 GUI 的屏幕图像。

worker 不读取 `RemoteClient` 的当前字段。这样它只维护自己的 socket 和 state，不与 GUI state 发生跨线程共享。

### 19.2 screen 与 control 使用各自的单级检查

屏幕结果回到 `RemoteClient` 后：

```cpp
if (_generation == this->m_screenStreamGeneration)
{
    emit this->screenFrameReady(_image);
}
```

control result 使用相同模式，只比较 `_generation` 与 `m_controlStreamGeneration`。

一次旧结果到达时，没有“修正成新 generation”的过程：比较失败就不向 GUI 继续发 signal。

### 19.3 one-shot 与下载的检查范围不同

一次性请求只需要确认 endpoint：

```text
OneShotRequest 捕获 endpoint generation
  → callback 调用 isEndpointGenerationCurrent
  → 相同才解释为当前界面结果
```

下载同时依赖 endpoint 和下载任务自身，因此使用联合检查：

```cpp
bool isDownloadGenerationCurrent(
    quint64 _endpointGeneration,
    quint64 _downloadGeneration) const noexcept
{
    return this->isEndpointGenerationCurrent(
               _endpointGeneration) &&
        _downloadGeneration == this->m_downloadGeneration;
}
```

- `_endpointGeneration`：下载启动时对应的服务器版本。
- `_downloadGeneration`：下载启动时对应的下载任务版本。

同一 endpoint 上连续发起下载时，只有 download generation 能区分先后任务；切换 endpoint 时，两个编号都会失效。

### 19.4 过滤结果不等于清理资源

generation check 只阻止旧结果更新 GUI，不会自动停止旧 socket、timer 或临时文件：

```text
generation 不匹配
  → 不向 GUI 转发结果

queued close / cancel / shutdown
  → 在 worker thread 清理旧资源
```

因此后面的三个 worker 都要同时回答两个问题：如何完成自身状态机，以及如何把 generation 随结果送回过滤边界。

---

## 20. `ScreenStreamWorker`：一条连接上始终只有一帧在途

屏幕帧需要持续请求，但项目没有一次性发送大量请求。`ScreenStreamWorker` 复用一个 socket，并严格保持 one request in flight。

### 20.1 `requestFrame()` 的三个参数

```cpp
void requestFrame(
    QString const& _host,
    quint16 _port,
    quint64 _generation);
```

- `_host`：本帧所属服务器的主机名或 IP 地址。
- `_port`：本帧连接使用的 TCP 端口。
- `_generation`：本次 screen stream 的逻辑版本，随结果原样返回。

worker 不读取 `RemoteClient`，所需值全部由 queued lambda 按值传入。

### 20.2 三个状态

```cpp
enum class ScreenStreamState
{
    Idle,
    FramePending,
    ShuttingDown,
};
```

- `Idle`：可以接受下一帧请求。
- `FramePending`：已有一帧等待连接、响应或解码。
- `ShuttingDown`：终止状态，不再接受任务。

入口首先拒绝重复请求：

```cpp
if (this->m_state ==
        ScreenStreamState::ShuttingDown ||
    this->m_state ==
        ScreenStreamState::FramePending)
{
    return;
}
```

这不是性能限制，而是协议对应关系的前提。若允许两帧同时在途，仅凭 command 无法区分响应属于哪次请求。

### 20.3 endpoint 变化时重建 socket

worker 保存当前 host/port。新请求目标不同，不能继续复用旧连接：

```text
新 host/port 与已保存 endpoint 不同
  → resetSocket：丢弃旧连接与旧缓冲区
  → 保存新 endpoint
  → 创建新 QTcpSocket
  → 异步连接
```

目标相同且 socket 已连接时，则直接发送下一帧请求，省去反复 TCP handshake。

### 20.4 帧请求的发送条件

```cpp
void sendFrameRequest()
{
    remote_control::Packet const request{
        remote_control::Command::WatchScreen};

    QByteArray const bytes{request.serialize()};
    if (bytes.isEmpty() ||
        this->m_socket->write(bytes) < 0)
    {
        this->failRequest(
            tr("Failed to send the screen request."),
            true);
        return;
    }
}
```

`sendFrameRequest()` 没有参数，它使用 worker 已保存的 socket 和当前 generation。

只有进入 `FramePending` 后才会执行该函数。连接成功与复用现有连接最终都汇合到这里。

timeout 在 `requestFrame()` 进入 `FramePending` 后立即启动，因此同时限制 TCP connection setup 和本帧 response 的总等待时间，而不是等到写出请求后才开始计时。

### 20.5 收到帧后的完整顺序

```text
readyRead 通知已有网络数据
  → readAll 追加缓冲区
  → tryParse 取完整 WatchScreen 包
  → 验证响应 command
  → 从 PNG payload 解码 QImage
  → 状态改为 Idle
  → 停止 timeout
  → 发出 frameReady，交付图像
  → 发出 requestFinished，释放在途槽位
```

即使解码失败，也必须发出 `requestFinished`。这个信号不是“成功信号”，而是“本帧已经不再占用 in-flight 槽位”。

### 20.6 `failRequest()` 的两个参数

```cpp
void failRequest(
    QString const& _message,
    bool _abortConnection);
```

- `_message`：向上层报告的失败原因。
- `_abortConnection`：是否立即丢弃当前 socket。

协议错乱、PNG 解码失败、socket error 或超时后传 `true`，因为当前连接不再可信。`onDisconnected()` 传 `false`，因为 socket 已经断开，不需要再次 `abort()`。

失败路径也要完成三件事：只报告一次、释放 `FramePending`、通知调度器可以决定下一步。

项目使用的 PNG 解码调用是：

```cpp
image.loadFromData(packet->payload, "PNG");
```

`QImage::loadFromData()` 在这里有两个参数：

- `packet->payload`：包含完整 PNG 文件字节的输入数据。
- `"PNG"`：显式指定输入格式，避免依赖格式自动探测。

---

## 21. `RemoteScreenWindow`：完成一帧后再安排下一帧

worker 只保证“一次最多处理一帧”，真正的持续拉帧节奏由 `RemoteScreenWindow` 控制。

### 21.1 构造函数建立的信号链

```cpp
RemoteScreenWindow(
    RemoteClient* _client,
    QWidget* _parent);
```

- `_client`：负责屏幕请求和控制命令的客户端门面。
- `_parent`：该对话框的 Qt parent，用于窗口层级和生命周期管理。

核心连接关系：

```text
frame timer timeout：到达下一帧调度时刻
  → requestNextFrame：窗口提交下一帧
  → RemoteClient::requestScreenFrame：记录 pending
  → ScreenStreamWorker::requestFrame：worker 发网络请求

worker 发出 frameReady：图像已解码
  → RemoteClient 过滤 generation
  → RemoteScreenWidget::setImage

worker 发出 requestFinished：本帧已结束
  → RemoteClient 过滤 generation
  → RemoteScreenWindow::scheduleNextFrame
```

注意：下一帧由 `requestFinished` 驱动，而不是由 `frameReady` 驱动。失败帧没有 image，但仍然需要释放调度器。

### 21.2 第一帧为什么立即请求

```cpp
void showEvent(QShowEvent* _event)
{
    QDialog::showEvent(_event);
    this->m_frameRequestTimer->stop();
    this->m_frameRequestElapsed.invalidate();
    this->requestNextFrame();
}
```

`showEvent(QShowEvent* _event)` 的参数：

- `_event`：Qt 传入的窗口显示事件；先交给基类处理，再启动屏幕请求。

首次显示不等待 33 ms，让窗口尽快获得第一张图。后续帧才进入固定的最小间隔调度。

### 21.3 33 ms 是最小提交间隔，不是固定等待时间

```cpp
void requestNextFrame()
{
    if (!this->isVisible())
    {
        return;
    }

    this->m_frameRequestElapsed.start();
    this->m_client->requestScreenFrame();
}
```

`QElapsedTimer::start()` 没有参数，从当前时刻开始计时。计时点位于请求提交前，因此网络、服务端截图、PNG 编码、传输和客户端解码都计入本帧耗时。

完成后计算剩余等待时间：

```cpp
qint64 const elapsedMs{
    this->m_frameRequestElapsed.isValid()
        ? this->m_frameRequestElapsed.elapsed()
        : MinimumWatchFrameIntervalMs};

int const delayMs{
    elapsedMs >= MinimumWatchFrameIntervalMs
        ? 0
        : MinimumWatchFrameIntervalMs -
              static_cast<int>(elapsedMs)};

this->m_frameRequestTimer->start(delayMs);
```

相关函数：

- `isValid()`：没有参数，判断计时器是否已经开始且未被 invalidated。
- `elapsed()`：没有参数，返回从 `start()` 到当前的毫秒数。
- `QTimer::start(int msec)`：`msec` 是本次 timeout 前等待的毫秒数；这里传计算出的剩余时间。

两种情况：

```text
本帧耗时 10 ms
  → 再等 23 ms
  → 相邻请求至少间隔 33 ms

本帧耗时 80 ms
  → delay = 0
  → event loop 下一次可调度时立即请求
```

因此上限约为 30 FPS，慢网络下不会为了追赶帧率并发补发请求。

### 21.4 关闭窗口时为什么同时停止两个 stream

```cpp
void closeEvent(QCloseEvent* _event)
{
    this->m_frameRequestTimer->stop();
    this->m_frameRequestElapsed.invalidate();
    this->m_screenWidget->cancelPendingMouseMove();
    this->m_client->stopScreenStream();
    this->m_client->stopControlStream();
    QDialog::closeEvent(_event);
}
```

`closeEvent(QCloseEvent* _event)` 的参数：

- `_event`：Qt 传入的关闭事件，清理本窗口相关异步工作后交给基类。

关闭顺序阻止三个“重新启动入口”：

1. frame timer 不能再发起屏幕请求。
2. mouse timer 不能在关闭后补发鼠标移动。
3. 两个 stream generation 递增，晚到回调不能更新窗口或延续旧控制流。

---

## 22. `RemoteScreenWidget`：先在 GUI 层压缩鼠标移动

鼠标移动事件的频率可能远高于网络命令的完成速度。项目在 GUI 层先做一次 16 ms 合并，控制 worker 中再做队列合并。

### 22.1 开启无按键移动跟踪

```cpp
RemoteScreenWidget::RemoteScreenWidget(QWidget* _parent)
    : QWidget{_parent},
      m_moveEventTimer{new QTimer{this}}
{
    this->setMouseTracking(true);
    this->m_moveEventTimer->setSingleShot(true);
    this->m_moveEventTimer->setInterval(16);
}
```

构造参数：

- `_parent`：承载远程画面的父 widget。

`QWidget::setMouseTracking(bool enable)` 的参数：

- `enable`：传 `true` 后，即使没有按下鼠标按钮，也会收到 mouse move event。

`QTimer::setInterval(int msec)` 的参数：

- `msec`：timer 的触发间隔；项目设为 16 ms，约对应 60 次每秒的 GUI 侧提交上限。

### 22.2 move event 只保存最新位置

```cpp
void mouseMoveEvent(QMouseEvent* _event)
{
    this->m_pendingMoveEvent = this->makeMouseEvent(
        remote_control::MouseAction::Move,
        remote_control::MouseButton::None,
        mouseEventPosition(_event));
    this->m_hasPendingMoveEvent = true;

    if (!this->m_moveEventTimer->isActive())
    {
        this->m_moveEventTimer->start();
    }

    QWidget::mouseMoveEvent(_event);
}
```

`mouseMoveEvent(QMouseEvent* _event)` 的参数：

- `_event`：本次移动的 widget 坐标、按钮状态等输入信息。

timer 已启动时，新 move 不增加 timer，也不增加队列，只覆盖 `m_pendingMoveEvent`。16 ms 内出现 20 个位置，最终只发送第 20 个。

### 22.3 为什么按下和释放前要先 flush move

```cpp
void mousePressEvent(QMouseEvent* _event)
{
    this->flushPendingMoveEvent();
    emit this->mouseEventCreated(
        this->makeMouseEvent(
            remote_control::MouseAction::Press,
            this->toProtocolButton(_event->button()),
            mouseEventPosition(_event)));
}
```

`mousePressEvent(QMouseEvent* _event)` 的参数：

- `_event`：包含按下位置和具体 Qt mouse button 的事件。

如果不先 flush，顺序可能变成：

```text
用户移动到 B 点
  → move 暂存在 timer 中
用户立即在 B 点按下
  → press 先发送
timer 随后发送 move
```

服务端看到的是“在旧位置按下，再移动到 B”，与用户操作相反。press、release 和 double click 都先发送最新待处理 move，维持输入时序。

### 22.4 widget 坐标怎样变成远程屏幕坐标

```cpp
QPoint mapToRemote(QPoint const& _point) const
{
    int const remoteX{
        _point.x() * this->m_screenImage.width() /
        this->width()};
    int const remoteY{
        _point.y() * this->m_screenImage.height() /
        this->height()};
    return {remoteX, remoteY};
}
```

参数：

- `_point`：鼠标在当前 widget 中的位置。

当前绘制逻辑把远程图像拉伸到整个 widget，所以 X、Y 分别按宽高比例换算。若以后改为保持宽高比并出现黑边，绘制区域和坐标映射必须一起修改，否则点击位置会偏移。

### 22.5 关闭时是取消，而不是 flush

```cpp
void cancelPendingMouseMove()
{
    this->m_moveEventTimer->stop();
    this->m_hasPendingMoveEvent = false;
}
```

该函数没有参数。窗口关闭代表用户结束控制，此时不应再补发最后一次移动；清除 pending 标记还能防止旧 timer callback 在下一次打开窗口时污染新控制流。

---

## 23. `ControlStreamWorker`：连接成功后还要完成控制通道握手

鼠标、锁定和解锁都走持久控制连接。TCP connected 只说明传输连接建立，服务端还不知道该连接要进入 control-channel 模式。

### 23.1 两个公开发送入口

鼠标入口：

```cpp
void sendMouseEvent(
    QString const& _host,
    quint16 _port,
    remote_control::MouseEventPacket const& _event,
    quint64 _generation);
```

- `_host`：目标服务器主机名或 IP 地址。
- `_port`：目标 TCP 端口。
- `_event`：已经换算为远程屏幕坐标的协议鼠标事件。
- `_generation`：本次 control stream 的版本号。

锁定/解锁入口：

```cpp
void sendCommand(
    QString const& _host,
    quint16 _port,
    remote_control::Command _command,
    QString const& _context,
    quint64 _generation);
```

- `_host`：目标服务器地址。
- `_port`：目标服务器端口。
- `_command`：只允许 `LockMachine` 或 `UnlockMachine`。
- `_context`：供完成或失败信号显示的操作名称。
- `_generation`：本次 control stream 的版本号。

其他文件类命令继续使用 `OneShotRequest`。不同连接模型不会因为“都能写一个 packet”就混在一起。

### 23.2 五个控制连接状态

```cpp
enum class ControlStreamState
{
    Disconnected,
    Connecting,
    Handshaking,
    Ready,
    ShuttingDown,
};
```

有效状态迁移：

```text
Disconnected（没有连接）
  → Connecting（正在建立 TCP 连接）
  → Handshaking（正在确认控制通道）
  → Ready（可以逐条发送控制命令）

任一活动状态发生连接级错误
  → Disconnected（连接与队列已清理）

任一状态执行 shutdown
  → ShuttingDown（终止状态，不再接收命令）
```

`ShuttingDown` 是终止状态。若 shutdown 后仍接受命令，worker 会在析构期间重新创建 socket。

### 23.3 handshake 发什么

```cpp
void sendHandshake()
{
    remote_control::Packet const handshake{
        remote_control::Command::ControlChannel};

    QByteArray const bytes{handshake.serialize()};
    if (bytes.isEmpty() ||
        this->m_socket->write(bytes) < 0)
    {
        this->failAllCommands(
            tr("Failed to open the control channel."));
        return;
    }

    this->m_timeoutTimer->start();
}
```

`sendHandshake()` 没有参数，使用当前 socket 发送 `ControlChannel` 命令。

只有收到 command 匹配且 status 表示成功的响应后，状态才能从 `Handshaking` 进入 `Ready`：

```cpp
if (response->command !=
        remote_control::Command::ControlChannel ||
    !success)
{
    this->failAllCommands(message);
    return;
}

this->m_state = ControlStreamState::Ready;
this->sendNextCommand();
```

握手失败时不能继续发送已经排队的鼠标命令，因为服务端仍可能把该连接当成普通 one-shot connection。

### 23.4 timeout 覆盖哪些阶段

同一个 single-shot timer 覆盖：

- TCP connection setup；
- control-channel handshake；
- 当前命令响应。

进入 `Ready` 且既无 in-flight command 又无 queued command 时停止 timer。持久连接空闲不是错误，不能因为 15 秒没命令就报告超时。

---

## 24. 控制命令队列：严格一问一答，只合并相邻 move

控制连接可以复用，但协议响应只携带 command，没有独立 request ID。项目因此保持一个 in-flight command，并对等待队列设置明确上限。

### 24.1 `enqueueCommand()` 的三个参数

```cpp
void enqueueCommand(
    QString const& _host,
    quint16 _port,
    ControlCommand _command);
```

- `_host`：命令目标服务器地址。
- `_port`：命令目标 TCP 端口。
- `_command`：待排队的完整控制项，包含 command、payload、context 和 generation。

host/port 改变时，worker 先清空旧队列并关闭旧 socket，再保存新 endpoint。不同服务器的命令绝不能共用同一响应流。

### 24.2 `sendNextCommand()` 的三个发送门槛

```cpp
if (this->m_state != ControlStreamState::Ready ||
    this->m_inFlightCommand.has_value() ||
    this->m_pendingCommands.isEmpty())
{
    return;
}
```

`sendNextCommand()` 没有参数。只有同时满足以下条件才发送：

1. handshake 已成功，状态为 `Ready`。
2. 当前没有命令等待响应。
3. 队列中至少有一个命令。

发送时先从 queue 移入 `m_inFlightCommand`，再写 socket：

```cpp
this->m_inFlightCommand =
    this->m_pendingCommands.dequeue();

remote_control::Packet const request{
    this->m_inFlightCommand->command,
    this->m_inFlightCommand->payload};

this->m_socket->write(request.serialize());
```

这样即使 `write()` 后很快收到响应，也已经有明确的匹配对象。

### 24.3 响应必须匹配当前 in-flight command

```cpp
if (this->m_state != ControlStreamState::Ready ||
    !this->m_inFlightCommand.has_value() ||
    response->command !=
        this->m_inFlightCommand->command)
{
    this->failAllCommands(
        tr("Unexpected control response."));
    return;
}
```

验证通过后：

```text
取出刚完成的 in-flight command
  → 清空 in-flight 槽位，恢复可发送状态
  → 发 completed 或 failed signal，报告业务结果
  → sendNextCommand，尝试发送队首命令
```

必须先清空槽位再发 signal。上层 slot 可能同步或间接提交新命令，状态要先处于可解释的一致点。

### 24.4 为什么只能合并相邻 move-only event

判断函数：

```cpp
bool isMouseMoveOnly(
    ControlCommand const& _command);
```

参数：

- `_command`：要检查 command 类型和固定尺寸 payload 的控制项。

只有 command 为 `MouseEvent`、payload 尺寸正确、action 为 `Move` 且 button 为 `None` 时才返回 `true`。

合并规则：

```cpp
if (isMouseMoveOnly(_command) &&
    !this->m_pendingCommands.isEmpty() &&
    isMouseMoveOnly(
        this->m_pendingCommands.back()))
{
    this->m_pendingCommands.back() =
        std::move(_command);
}
```

可以合并：

```text
Move A → Move B → Move C
结果：只保留 Move C
```

不能跨越：

```text
Move A → Press → Move B
结果：三者顺序都保留
```

如果跨过 press 合并，服务端可能在错误坐标按下按钮。

### 24.5 队列为什么限制为 128

当服务端响应变慢时，无界队列会持续占用内存，还会让几秒前的鼠标输入在恢复后继续回放。项目最多保留 128 个待发送命令；超过上限时拒绝新命令并发出失败结果。

GUI 层 16 ms throttle 负责减少生产速度，worker 层 move coalescing 负责处理跨线程排队和网络背压，两层解决的是不同位置的积压。

### 24.6 连接级失败为什么清空所有命令

```cpp
void failAllCommands(QString const& _message);
```

参数：

- `_message`：应用到当前 in-flight command 和所有 queued command 的连接级失败原因。

函数先把队列移动到局部变量，再 reset socket，最后逐个发失败 signal。旧连接断开后，无法确定服务端是否已经执行了某个没有收到响应的命令，因此项目不会自动在新连接上重放。

---

## 25. `FileDownloadWorker`：用 `QSaveFile` 保证失败不污染目标文件

下载同时包含网络读取和磁盘写入，放在独立 worker thread 后，GUI 不会被大文件写盘阻塞。当前实现一次只允许一个 active download。

### 25.1 GUI 提交下载时先更新任务编号

`RemoteClient` 的入口：

```cpp
void downloadRemoteFile(
    QString const& _remotePath,
    QString const& _localPath);
```

- `_remotePath`：服务端要读取的文件路径。
- `_localPath`：下载成功后提交的本地目标路径。

投递前先取得本次任务身份：

```cpp
++this->m_downloadGeneration;

quint64 const endpointGeneration{
    this->m_endpointGeneration};
quint64 const downloadGeneration{
    this->m_downloadGeneration};

QMetaObject::invokeMethod(
    this->m_fileDownloadWorker,
    [worker = this->m_fileDownloadWorker,
     host,
     port,
     _remotePath,
     _localPath,
     endpointGeneration,
     downloadGeneration] {
        worker->startDownload(
            host,
            port,
            _remotePath,
            _localPath,
            endpointGeneration,
            downloadGeneration);
    },
    Qt::QueuedConnection);
```

递增发生在 queued 投递之前，所以第二次点击会立即使第一次下载的 UI 结果过期。当前 worker 不会用第二次下载自动取消第一次下载：第二个请求会得到“已有下载进行中”的失败结果，第一次下载继续在 worker 中收尾。界面应避免重复启动；endpoint 变化才走专门的取消路径。

### 25.2 `startDownload()` 的六个参数

```cpp
void startDownload(
    QString const& _host,
    quint16 _port,
    QString const& _remotePath,
    QString const& _localPath,
    quint64 _endpointGeneration,
    quint64 _downloadGeneration);
```

- `_host`：远程服务器主机名或 IP 地址。
- `_port`：下载连接的 TCP 端口。
- `_remotePath`：服务端要打开的文件路径。
- `_localPath`：客户端下载成功后要替换的目标路径。
- `_endpointGeneration`：下载所属的 endpoint 版本。
- `_downloadGeneration`：下载任务自身版本。

worker 的状态只有三种：

```cpp
enum class DownloadState
{
    Idle,
    Downloading,
    ShuttingDown,
};
```

`Downloading` 状态收到新请求时会拒绝新任务，而不是并发创建第二个 socket 和输出文件。

### 25.3 为什么在 worker thread 中创建 `QSaveFile`

```cpp
this->m_outputFile =
    std::make_unique<QSaveFile>(this->m_localPath);

if (!this->m_outputFile->open(QIODevice::WriteOnly))
{
    this->fail(tr("Unable to write the local file."));
    return;
}
```

`QSaveFile(QString const& name)` 的参数：

- `name`：最终目标文件路径。`QSaveFile` 先在目标附近写临时文件，成功 `commit()` 后才替换目标。

`open(QIODevice::OpenMode mode)` 的参数：

- `mode`：文件打开模式；这里用 `WriteOnly` 表示只写。

对象在 worker callback 中创建，也只在 worker thread 中读写和销毁。不能在 GUI thread 创建后再把裸指针交给 worker 使用。

项目没有调用 `setDirectWriteFallback(true)`，因此无法创建临时文件时 `open()` 会失败，而不是直接覆盖现有目标文件。

`QSaveFile::setDirectWriteFallback(bool _enabled)` 的参数：

- `_enabled`：传 `true` 允许无法创建临时文件时直接写目标；传 `false` 保持原子替换要求。项目保留默认的 `false`。

这条前提使“失败不污染原目标文件”的结论成立。

### 25.4 第一个 payload 是固定宽度文件大小

```cpp
if (this->m_expectedFileSize < 0)
{
    if (_payload.size() !=
        static_cast<int>(sizeof(qint64)))
    {
        this->fail(tr("Invalid download header."));
        return;
    }

    QDataStream stream{_payload};
    stream.setByteOrder(QDataStream::LittleEndian);
    stream >> this->m_expectedFileSize;
}
```

`processPacket(QByteArray const& _payload)` 的参数：

- `_payload`：一个已经解析完成的 `DownloadFile` 响应 payload。

`QDataStream::setByteOrder(ByteOrder byteOrder)` 的参数：

- `byteOrder`：整数的字节序；这里必须与协议规定的 little-endian 一致。

头部处理规则：

- payload 长度不是 `sizeof(qint64)`：协议错误。
- 文件大小小于 0：服务端无法读取远程文件。
- 文件大小等于 0：立即提交空文件，不再等待数据包。
- 文件大小大于 0：后续 payload 都作为文件数据。

### 25.5 数据写入要检查越界和短写

```cpp
if (this->m_writtenBytes + _payload.size() >
    this->m_expectedFileSize)
{
    this->fail(tr("Received excess download data."));
    return;
}

if (!this->m_outputFile ||
    this->m_outputFile->write(_payload) !=
        _payload.size())
{
    this->fail(tr("Failed to write local file."));
    return;
}
```

`QIODevice::write(QByteArray const& data)` 的参数：

- `data`：要写入文件的完整数据块。

返回值必须等于 payload 长度。只判断“返回值不是负数”会把短写误认为成功，最终得到截断文件。

### 25.6 只有字节数精确相等时才 `commit()`

```cpp
this->m_writtenBytes += _payload.size();

if (this->m_writtenBytes ==
    this->m_expectedFileSize)
{
    this->completeSuccessfully();
}
```

`QSaveFile::commit()` 没有参数。它关闭临时文件并把结果提交到目标路径，成功返回 `true`。

失败路径调用 `cancelWriting()`：

```cpp
if (this->m_outputFile &&
    this->m_outputFile->isOpen())
{
    this->m_outputFile->cancelWriting();
}
```

`cancelWriting()` 没有参数，标记当前保存操作不能提交。随后销毁 `QSaveFile`，临时内容不会作为完整目标文件保留。

### 25.7 取消下载的两个参数

```cpp
void cancelActiveDownload(
    quint64 _endpointGeneration,
    quint64 _downloadGeneration);
```

- `_endpointGeneration`：调用取消时已经更新后的 endpoint 版本。
- `_downloadGeneration`：调用取消时已经更新后的下载版本。

worker 把这两个新值写入当前任务后再走统一 `fail()`。这样取消完成信号属于当前版本，可以关闭 UI 进度；旧下载数据回调仍因旧版本而被过滤。

### 25.8 为什么每次下载都重建 socket

屏幕和控制流有明确的长期复用协议，下载则是一项完整文件事务。完成或失败后 `resetSocket()`，下一次下载从全新的连接和空解析缓冲区开始，避免前一文件的尾部数据污染下一任务。

worker thread 会继续存在；重建的是每次事务的 socket 与 `QSaveFile`，不是整个线程。

---

## 26. endpoint 切换的完整时序

`setEndpoint()` 不只是保存两个字段。它是一次客户端业务时代切换，必须同时终止旧 endpoint 上的四类异步结果。

### 26.1 两个参数

```cpp
void setEndpoint(
    QString const& _host,
    quint16 _port);
```

- `_host`：后续请求使用的新服务器主机名或 IP 地址。
- `_port`：后续请求使用的新 TCP 端口。

host 或 port 任一变化才执行 invalidation；重复设置相同 endpoint 不应无故断开现有 stream。

### 26.2 代码顺序

```cpp
bool const endpointChanged{
    this->m_host != _host ||
    this->m_port != _port};

bool const screenRequestWasPending{
    endpointChanged &&
    this->hasPendingScreenFrame()};

if (endpointChanged)
{
    ++this->m_endpointGeneration;
    ++this->m_downloadGeneration;
    this->stopScreenStream();
    this->stopControlStream();

    quint64 const endpointGeneration{
        this->m_endpointGeneration};
    quint64 const downloadGeneration{
        this->m_downloadGeneration};

    QMetaObject::invokeMethod(
        this->m_fileDownloadWorker,
        [worker = this->m_fileDownloadWorker,
         endpointGeneration,
         downloadGeneration] {
            worker->cancelActiveDownload(
                endpointGeneration,
                downloadGeneration);
        },
        Qt::QueuedConnection);
}

this->m_host = _host;
this->m_port = _port;
```

按职责拆开看：

1. endpoint generation 递增，使旧 one-shot 和旧下载结果失效。
2. download generation 递增，结束旧下载身份。
3. `stopScreenStream()` 递增 screen generation，并投递关闭 socket。
4. `stopControlStream()` 递增 control generation，并投递关闭 socket。
5. 向下载 worker 投递取消，清理临时文件和下载 socket。
6. 最后保存新 host/port，后续提交使用新 endpoint。

整个过程不从 GUI thread 直接操作 worker socket。

### 26.3 为什么 pending screen frame 要单独释放

旧 worker 最终可能发出旧 generation 的 `requestFinished`，但 `RemoteClient` 会把它过滤掉。若 UI 调度器只等这个信号，`m_screenFramePending` 和下一帧调度可能永久停住。

因此切换前先记录是否有 pending frame，完成 generation 切换后主动发出一次当前 UI 可见的完成通知：

```cpp
if (screenRequestWasPending)
{
    emit this->screenFrameRequestFinished();
}
```

它不表示旧帧成功，只表示旧 in-flight 槽位对 GUI 调度器已经结束。若远程窗口仍可见，下一次 timer 会使用新 endpoint 请求帧。

### 26.4 一组完整编号示例

切换前：

```text
endpoint generation = 7
download generation = 12
screen generation = 4
control generation = 9
```

切换后：

```text
endpoint generation = 8
download generation = 13
screen generation = 5
control generation = 10
```

旧回调处理结果：

```text
旧 one-shot (E7)
  → 丢弃

旧下载进度 (E7, D12)
  → 丢弃

取消完成 (E8, D13)
  → 若尚未开始下一次下载，则接受并关闭当前进度状态

旧屏幕帧 (S4)
  → 丢弃

旧控制命令结果 (C9)
  → 丢弃
```

### 26.5 切换 endpoint 的本质

关闭 socket 只能阻止未来通信，不能撤回已经排入 event queue 的回调。generation 只能过滤业务结果，不能释放 socket 和临时文件。两者必须配合：

```text
资源清理 resource cleanup
  → 终止旧连接和旧文件事务

结果失效 generation invalidation
  → 阻止晚到结果污染当前 UI
```

---

## 27. `RemoteControlHostServices`：传输层只依赖能力接口

阶段八、阶段九中的 IOCP dispatcher 负责识别命令和组织响应，但它不应该直接包含 QWidget、GDI 或 Shell 代码。`RemoteControlHostServices` 把“协议需要什么”与“Windows 怎样完成”隔开。

### 27.1 六项主机能力

接口提供以下操作：

1. `localDriveRoots()`
   - 没有参数。
   - 返回允许远程浏览的本地磁盘根目录。
2. `isFilePathAllowed(QString const& _path)`
   - `_path`：远程客户端提交的文件系统路径。
   - 返回该路径是否符合本机访问策略。
3. `openFile(QString const& _path)`
   - `_path`：已经存在且通过策略检查的本地文件路径。
   - 请求 Windows 按文件关联打开它。
4. `sendMouseEvent(MouseEventPacket const& _event)`
   - `_event`：从 control stream 解码出的鼠标坐标、action 与 button。
   - 将事件转换为 Windows 全局输入。
5. `captureScreenPng()`
   - 没有参数。
   - 返回当前主屏幕的 PNG 字节，失败时返回空数组。
6. `requestScreenLock(bool _locked)`
   - `_locked`：`true` 请求锁定，`false` 请求解锁。
   - 返回请求是否被执行上下文接受。

dispatcher 只针对这些语义编程，因此协议测试可以替换为 fake host services，不必真的移动鼠标或锁住屏幕。

### 27.2 实现对象的构造参数

```cpp
WindowsRemoteControlHostServices(
    ScreenLockService& _screenLockService);
```

- `_screenLockService`：GUI thread 中已经存在的锁屏服务引用。

实现对象不创建锁屏窗口，只保存引用。这样 server startup 仍然拥有 UI 服务的生命周期，host services 只是适配边界。

### 27.3 普通平台调用可以直接委托

例如鼠标事件只做协议类型转换：

```cpp
bool sendMouseEvent(
    remote_control::MouseEventPacket const& _event)
{
    return WindowsPlatformIntegration::sendGlobalMouseEvent(
        QPoint{_event.x, _event.y},
        static_cast<remote_control::MouseAction>(
            _event.action),
        static_cast<remote_control::MouseButton>(
            _event.button));
}
```

这里不触碰 QWidget；底层输入函数还使用 process-wide mutex 串行化并发调用，所以可从 control worker 调用。

### 27.4 GUI-only 操作必须封送到 GUI thread

锁屏服务拥有 `ScreenLockWindow`，不能从 IOCP thread 或 bounded worker thread 直接调用：

```cpp
bool requestScreenLock(bool _locked)
{
    ScreenLockService* const service{
        &this->m_screenLockService};

    return QMetaObject::invokeMethod(
        service,
        [service, _locked] {
            if (_locked)
            {
                service->lockScreen();
            }
            else
            {
                service->unlockScreen();
            }
        },
        Qt::QueuedConnection);
}
```

这里三个实参的作用与第 8 节一致：

- `service`：决定 callback 应进入哪个线程；它属于 GUI thread。
- lambda：真正调用 GUI 服务的工作。
- `Qt::QueuedConnection`：只投递，不让 IOCP/worker thread 等待 UI 执行。

### 27.5 返回 `true` 代表什么

这里返回的是 `invokeMethod()` 是否成功接受投递，不是“屏幕已经可见地锁住”。完整时序是：

```text
IOCP/worker thread 调用 requestScreenLock
  → queued callback 投递成功
  → 函数返回 true
  → dispatcher 可发送 accepted status
  → GUI event loop 稍后执行 lockScreen
```

如果协议要求“锁屏实际完成后才响应”，就需要设计异步 completion signal 和关联 ID，不能把 connection type 改成 `BlockingQueuedConnection` 来伪造同步语义。

---

## 28. GDI 截图：DC、DIB 与 PNG 的所有权链

`capturePrimaryScreenPng()` 使用 GDI 把主屏幕复制到可直接访问像素的 DIB，再用 `QImage` 编码为 PNG。关键不只是调用 API，还要保证每个 handle 和像素指针在正确时机有效。

### 28.1 总体流程

```text
取得主屏幕尺寸
  → GetDC 获取 screen DC
  → 确保 compatible memory DC 与 DIB 存在
  → BitBlt 把屏幕复制到 DIB
  → 建议 GdiFlush，完成 GDI/CPU 同步
  → ReleaseDC 释放 screen DC
  → QImage 临时引用 DIB 像素
  → 编码 PNG 到 QByteArray
```

`GdiFlush()` 是依据 Microsoft 文档补出的加固边界，当前源码尚未调用。项目捕获的是 primary screen，不是所有显示器组成的 virtual desktop。

### 28.2 `GetSystemMetrics()` 与 `GetDC()`

```cpp
int const width{GetSystemMetrics(SM_CXSCREEN)};
int const height{GetSystemMetrics(SM_CYSCREEN)};
HDC const screenDc{GetDC(nullptr)};
```

`GetSystemMetrics(int nIndex)` 的参数：

- `nIndex`：要查询的系统指标；`SM_CXSCREEN` 是主屏幕宽度，`SM_CYSCREEN` 是主屏幕高度。

`GetDC(HWND hWnd)` 的参数：

- `hWnd`：目标窗口 handle；传 `nullptr` 表示取得整个屏幕的 device context。

返回的 `HDC` 不归 C++ RAII 自动管理，必须与 `ReleaseDC()` 配对；`ReleaseDC()` 应在调用 `GetDC()` 的同一线程执行。

### 28.3 `CreateCompatibleDC()`

```cpp
HDC const memoryDc{
    CreateCompatibleDC(screenDc)};
```

`CreateCompatibleDC(HDC hdc)` 的参数：

- `hdc`：兼容性参考 DC；项目传 screen DC，让 memory DC 与屏幕设备格式兼容。

返回的是内存 DC，不显示在屏幕上。它最终用 `DeleteDC(memoryDc)` 释放。

`DeleteDC(HDC hdc)` 的参数：

- `hdc`：由 `CreateCompatibleDC()` 创建、需要删除的 memory DC。

不能用 `DeleteDC()` 释放 `GetDC()` 获得的 screen DC。

### 28.4 `CreateDIBSection()` 的六个参数

项目先准备 32-bit、top-down 的 `BITMAPINFO`：

```cpp
BITMAPINFO bitmapInfo{};
bitmapInfo.bmiHeader.biSize =
    sizeof(BITMAPINFOHEADER);
bitmapInfo.bmiHeader.biWidth = width;
bitmapInfo.bmiHeader.biHeight = -height;
bitmapInfo.bmiHeader.biPlanes = 1;
bitmapInfo.bmiHeader.biBitCount = 32;
bitmapInfo.bmiHeader.biCompression = BI_RGB;

void* pixelData{nullptr};
HBITMAP const bitmap{CreateDIBSection(
    screenDc,
    &bitmapInfo,
    DIB_RGB_COLORS,
    &pixelData,
    nullptr,
    0)};
```

`CreateDIBSection()` 的参数：

1. `hdc`
   - 提供颜色兼容信息的 DC；项目传 screen DC。
2. `pbmi`
   - 指向 `BITMAPINFO`，描述宽、高、位深和压缩方式。
3. `usage`
   - 颜色表解释方式；`DIB_RGB_COLORS` 表示值就是 RGB 颜色。
4. `ppvBits`
   - 输出参数，成功后获得可直接读写的像素内存地址。
5. `hSection`
   - 可选 file-mapping handle；项目传 `nullptr`，由系统分配存储。
6. `offset`
   - 在 file mapping 中的字节偏移；未使用 mapping 时传 0。

`biHeight` 使用负值表示 top-down DIB，第一行像素就是图像顶部，不需要额外上下翻转。

### 28.5 `SelectObject()` 必须保存旧对象

```cpp
HGDIOBJ const previousObject{
    SelectObject(memoryDc, bitmap)};
```

`SelectObject(HDC hdc, HGDIOBJ h)` 的参数：

- `hdc`：要修改的 memory DC。
- `h`：要选入该 DC 的 GDI object；这里是 DIB bitmap。

返回值是原先选中的 object。释放 bitmap 前必须先把 `previousObject` 选回去，否则 bitmap 仍被 DC 选中，直接 `DeleteObject()` 的行为不可靠。

### 28.6 `BitBlt()` 的九个参数

```cpp
BOOL const copied{BitBlt(
    memoryDc,
    0,
    0,
    width,
    height,
    screenDc,
    0,
    0,
    SRCCOPY | CAPTUREBLT)};
```

各参数依次表示：

1. `hdcDest`：目标 DC，这里是选入 DIB 的 memory DC。
2. `x`：目标左上角 X，项目从 0 开始。
3. `y`：目标左上角 Y，项目从 0 开始。
4. `cx`：复制宽度，即主屏幕宽度。
5. `cy`：复制高度，即主屏幕高度。
6. `hdcSrc`：源 DC，这里是 screen DC。
7. `x1`：源区域左上角 X，项目从 0 开始。
8. `y1`：源区域左上角 Y，项目从 0 开始。
9. `rop`：raster operation；`SRCCOPY` 复制源像素，`CAPTUREBLT` 尝试包含 layered windows。

返回 0 表示复制失败，不能继续把旧 DIB 内容当成新截图发送。

当前项目在 `BitBlt()` 返回后直接通过 `pixelData` 编码。Microsoft 对 `CreateDIBSection()` 的说明要求：在 CPU 直接访问 DIB bits 前，应确保 GDI 已完成对 bitmap 的绘制，可调用 `GdiFlush()` 加固这一边界。

`GdiFlush()` 没有参数；返回非零表示当前调用线程的 GDI batch 已提交。项目源码目前没有这一步，阅读时应把它视为可补强点，而不是误以为 `pixelData` 的所有权能自动完成 GDI/CPU 同步。

### 28.7 `ReleaseDC()`、`DeleteObject()` 与释放顺序

`ReleaseDC(HWND hWnd, HDC hDC)` 的参数：

- `hWnd`：与 `GetDC()` 使用的窗口一致；项目传 `nullptr`。
- `hDC`：本次取得的 screen DC。

`DeleteObject(HGDIOBJ ho)` 的参数：

- `ho`：要删除的 GDI object；这里是 `HBITMAP`。

可复用资源最终按依赖顺序清理：

```text
把 previousObject 选回 memory DC
  → DeleteDC(memoryDc)
  → DeleteObject(bitmap)
  → 清空非 owning pixelData 指针
```

### 28.8 `QImage` 只临时借用 DIB 像素

```cpp
QImage const image{
    static_cast<uchar*>(pixelData),
    width,
    height,
    width * static_cast<int>(sizeof(quint32)),
    QImage::Format_RGB32};
```

五个构造参数：

- `data`：DIB 像素起始地址；`QImage` 不取得这块内存的所有权。
- `width`：图像像素宽度。
- `height`：图像像素高度。
- `bytesPerLine`：每行字节数；32-bit 像素即 `width * 4`。
- `format`：像素解释格式，这里是 `Format_RGB32`。

真正拥有像素存储的是 `HBITMAP`，`pixelData` 和这个 `QImage` 都只是引用。必须在 DIB 被复用或删除前完成 PNG 编码，不能把该 `QImage` 返回给其他线程长期保存。

项目在当前函数内完成编码：

```cpp
QByteArray pngData;
QBuffer buffer{&pngData};

if (!buffer.open(QIODevice::WriteOnly) ||
    !image.save(&buffer, "PNG"))
{
    return {};
}
```

`QBuffer(QByteArray* byteArray)` 的参数：

- `byteArray`：作为内存设备底层存储的字节数组；编码结果写入 `pngData`。

`QImage::save(QIODevice* device, const char* format, int quality = -1)` 的参数：

- `device`：接收编码结果的已打开输出设备；这里是 `QBuffer`。
- `format`：图像格式名称；这里明确指定 `PNG`。
- `quality`：编码质量；项目省略，使用默认值 `-1`，由格式插件选择默认策略。

### 28.9 为什么使用 `thread_local` capture context

```cpp
ScreenCaptureContext& screenCaptureContext()
{
    thread_local ScreenCaptureContext context;
    return context;
}
```

函数没有参数，返回当前调用线程独享的 capture context。相同尺寸下复用 memory DC 和 DIB，避免每帧反复创建大型 GDI 资源；不同 capture worker 不共享 handle，也就不需要围绕截图缓冲区加锁。

---

## 29. Windows 全局鼠标输入：先定位，再注入按钮记录

客户端已经把 widget 坐标转换为截图坐标，服务端再把协议 action/button 转换为 Windows input flag。

### 29.1 平台入口的三个参数

```cpp
bool sendGlobalMouseEvent(
    QPoint const& _position,
    remote_control::MouseAction _action,
    remote_control::MouseButton _button);
```

- `_position`：Windows screen coordinate 中的绝对位置。
- `_action`：`Move`、`Press`、`Release`、`Click` 或 `DoubleClick`。
- `_button`：左、中、右按钮；纯移动时为 `None`。

项目只截取 primary screen，所以客户端传回的坐标也以主屏幕左上角为原点。

### 29.2 为什么整个操作要加 process-wide mutex

不同 control connection 可能由不同服务端 worker 并发调用：

```text
线程 A：SetCursorPos(A)
线程 B：SetCursorPos(B)
线程 A：SendInput(LeftDown)
```

没有锁时，A 的点击会发生在 B 的位置。项目用一个进程级 `QMutex` 把“移动位置 + 注入按钮”作为不可交错的逻辑操作。

### 29.3 `SetCursorPos()` 的两个参数

纯移动直接调用：

```cpp
SetCursorPos(_position.x(), _position.y());
```

参数：

- `X`：目标 screen coordinate 的水平位置。
- `Y`：目标 screen coordinate 的垂直位置。

返回非零表示成功。button 为 `None` 时只允许 `Move`，其他 action/button 组合属于无效协议输入。

### 29.4 action 怎样变成 `INPUT` 数组

```text
Press（按下）
  → DOWN

Release（释放）
  → UP

Click（单击）
  → DOWN, UP

DoubleClick（双击）
  → DOWN, UP, DOWN, UP
```

项目最多准备四个 `INPUT`。每项设置：

```cpp
inputs[index].type = INPUT_MOUSE;
inputs[index].mi.dwFlags = flags[index];
```

左、中、右按钮分别映射到对应的 `MOUSEEVENTF_*DOWN` 和 `MOUSEEVENTF_*UP`。

### 29.5 `SendInput()` 的三个参数

先调用 `SetCursorPos()` 定位，再发送按钮记录：

```cpp
UINT const sentCount{SendInput(
    requestedCount,
    inputs.data(),
    static_cast<int>(sizeof(INPUT)))};
```

参数：

1. `cInputs`
   - `INPUT` 数组中的记录数量。
2. `pInputs`
   - 指向第一条 `INPUT` 的指针。
3. `cbSize`
   - 单个结构体大小，必须传 `sizeof(INPUT)`。

返回值是实际成功插入系统输入流的记录数。只有它等于请求数量时，整个命令才算成功。

### 29.6 为什么不把坐标也放进 `INPUT`

项目已经用 `SetCursorPos()` 设置绝对位置，`INPUT` 只携带 button transition，因此不需要处理 `MOUSEEVENTF_ABSOLUTE` 的 0 到 65535 坐标归一化及 virtual desktop 标志。

这也使“移动”和“按钮”逻辑更直观，但必须保留前述 mutex，防止二者被其他线程拆开。

### 29.7 权限边界

`SendInput()` 受 Windows UIPI 限制，通常只能向相同或更低 integrity level 的应用注入输入。返回 0 时系统不一定明确指出是 UIPI 阻止，因此“代码参数正确”不等于“可以控制任意高权限窗口”。

---

## 30. 模拟锁屏：保存并恢复桌面状态，而不是调用系统会话锁

项目的锁屏由无边框置顶窗口、隐藏 cursor、限制 cursor 区域和隐藏 taskbar 共同实现。它是 remote-control UI 功能，不是 Windows security boundary，也不等同于 `LockWorkStation()`。

### 30.1 平台函数的参数

```cpp
bool setSystemUiLocked(bool _locked);
```

- `_locked`：`true` 应用模拟锁屏状态，`false` 恢复此前桌面状态。

函数返回 cursor confinement 是否成功应用或恢复。重复 lock 和重复 unlock 都按幂等操作处理。

### 30.2 为什么先保存旧状态

锁定前记录：

- 当前进程是否已经处于模拟锁定。
- taskbar 是否存在。
- taskbar 原先是否可见。
- cursor 原先是否已被其他程序限制。
- 原始 cursor clipping rectangle。

解锁不能简单执行“显示 taskbar + 解除 cursor 限制”，因为锁定前 taskbar 可能本来就隐藏，cursor 也可能已有其他限制。正确原则是恢复旧值，而不是恢复假设中的默认值。

### 30.3 `GetClipCursor()` 与 `ClipCursor()`

读取原状态：

```cpp
RECT oldBounds{};
GetClipCursor(&oldBounds);
```

`GetClipCursor(LPRECT lpRect)` 的参数：

- `lpRect`：输出指针，接收当前 cursor clipping rectangle。

锁定时使用当前位置附近的 1 × 1 rectangle：

```cpp
RECT const replacement{
    cursorPosition.x(),
    cursorPosition.y(),
    cursorPosition.x() + 1,
    cursorPosition.y() + 1};

ClipCursor(&replacement);
```

`ClipCursor(const RECT* lpRect)` 的参数：

- `lpRect`：传 rectangle 指针时把 cursor 限制在该区域；传 `nullptr` 时解除限制。

1 × 1 rectangle 使 cursor 几乎无法移动。它是系统级共享状态，所以程序必须在解锁和退出时恢复。

### 30.4 `FindWindowW()` 怎样找到 taskbar

```cpp
HWND const taskbar{
    FindWindowW(L"Shell_TrayWnd", nullptr)};
```

两个参数：

- `lpClassName`：目标窗口类名；Windows 主 taskbar 使用 `Shell_TrayWnd`。
- `lpWindowName`：目标窗口标题；传 `nullptr` 表示不按标题筛选。

返回 `nullptr` 表示没有找到。项目会记录“是否找到”，避免解锁时凭空操作一个不存在的 handle。

### 30.5 `ShowWindow()` 的返回值不是成功标志

```cpp
ShowWindow(taskbar, SW_HIDE);
ShowWindow(taskbar, SW_SHOW);
```

两个参数：

- `hWnd`：要改变显示状态的窗口 handle。
- `nCmdShow`：显示命令；项目使用 `SW_HIDE` 或 `SW_SHOW`。

返回值表示调用前窗口是否可见，不是本次操作是否成功。因此项目先用 `IsWindowVisible()` 保存原状态，再调用 `ShowWindow()`，而不把返回 0 当成隐藏失败。

`IsWindowVisible(HWND hWnd)` 的参数：

- `hWnd`：要查询的窗口 handle。

### 30.6 Qt cursor override 与失败回滚

```cpp
QApplication::setOverrideCursor(Qt::BlankCursor);

if (ClipCursor(&replacement) != TRUE)
{
    QApplication::restoreOverrideCursor();
    restoreTaskbar();
    clearSavedState();
    return false;
}
```

`setOverrideCursor(QCursor const& cursor)` 的参数：

- `cursor`：覆盖应用程序 cursor 的样式；项目使用 blank cursor。

`restoreOverrideCursor()` 没有参数，弹出最近一次 override。平台限制 cursor 失败时，必须立即撤销已隐藏的 cursor 和 taskbar，不能留下“函数返回失败但桌面仍被部分修改”的状态。

### 30.7 `ScreenLockWindow` 负责可见覆盖层

平台状态成功后，窗口依次执行：

```text
showFullScreen：显示全屏覆盖层
  → raise：提升到同层窗口前方
  → activateWindow：请求激活窗口
  → setFocus：取得键盘焦点
  → grabKeyboard：抓取应用内键盘输入
```

锁定期间 close event 被忽略，focus 丢失时尝试重新激活。`Ctrl+C` 作为本地恢复入口，通过 signal 回到 `ScreenLockService::unlockScreen()`，确保 timer、平台状态和公开 lock state 一起更新。

这些措施提高了普通交互下的覆盖效果，但不能抵抗 secure desktop、系统快捷键、其他高权限进程或进程被终止。学习时应把它称为 simulated lock 或 lock overlay，而不是系统安全锁。

---

## 31. Shell 打开文件、管理员检测与 UAC 重启

Windows shell 能按文件关联打开本地文件，也能通过 `runas` verb 请求 UAC elevation。管理员检测则使用 access token membership，而不是根据用户名猜测。

### 31.1 `openLocalFile()` 的参数与前置条件

```cpp
bool openLocalFile(QString const& _path);
```

- `_path`：要交给 Windows shell 打开的本地文件路径。

远程 command 进入这里前应先经过 `isLocalFilePath()` 和业务权限检查。平台函数负责执行，不替代输入授权策略。

### 31.2 `ShellExecuteW()` 的六个参数

```cpp
HINSTANCE const result{ShellExecuteW(
    nullptr,
    L"open",
    nativePath,
    nullptr,
    nullptr,
    SW_SHOWNORMAL)};
```

参数依次是：

1. `hwnd`
   - 可作为错误 UI owner 的窗口 handle；项目传 `nullptr`。
2. `lpOperation`
   - shell verb；打开文件使用 `open`，请求提升使用 `runas`。
3. `lpFile`
   - 要打开的文件，或要启动的 executable path。
4. `lpParameters`
   - 启动 executable 时传给它的 command-line arguments；打开普通文件时传 `nullptr`。
5. `lpDirectory`
   - 默认工作目录；项目传 `nullptr`。
6. `nShowCmd`
   - 新窗口显示方式；项目使用 `SW_SHOWNORMAL`。

返回类型出于历史兼容看起来像 `HINSTANCE`，但这里不能把它当作可关闭的 instance handle。转为整数后大于 32 才表示 shell 接受请求，小于等于 32 是错误码。

### 31.3 `isRunningAsAdmin()` 检查什么

该函数没有参数。它先构造 Windows built-in Administrators group SID，再调用：

```cpp
CheckTokenMembership(
    nullptr,
    adminGroup,
    &isAdmin);
```

`CheckTokenMembership()` 的三个参数：

1. `TokenHandle`
   - 要检查的 access token；传 `nullptr` 时检查当前 thread 的 impersonation token，若没有则使用当前 process token。
2. `SidToCheck`
   - 要查询 membership 的 SID；这里是 Administrators group SID。
3. `IsMember`
   - 输出 `BOOL`，接收该 SID 是否在有效 token 中启用。

临时创建的 SID 最后必须用 `FreeSid(PSID pSid)` 释放：

- `pSid`：此前分配的 SID 指针。

此检查回答“当前 process token 是否已具备管理员权限”，不回答“当前用户能否在 UAC 对话框中同意提升”。

### 31.4 `relaunchElevated()` 的两个参数

```cpp
bool relaunchElevated(
    QStringList const& _arguments,
    QString* _errorMessage = nullptr);
```

- `_arguments`：传给提升后新进程的独立 argument 列表。
- `_errorMessage`：可选输出指针；失败时写入可显示给用户的信息，调用者不需要时可传 `nullptr`。

实现取得当前 executable path，把每个 argument 独立 quoting，再调用：

```text
调用 ShellExecuteW
  operation = runas：请求 UAC elevation
  file = current executable：重新启动当前程序
  parameters = quoted arguments：传递保持边界的参数串
```

独立 quoting 很重要。把带空格的路径直接用空格连接，会让新进程收到错误的 argument 边界。

### 31.5 elevation 是启动新进程，不是改变当前进程权限

`runas` 成功只表示 Windows 接受了启动请求。当前进程不会原地变成 elevated process，也没有获得新进程 handle。用户取消 UAC 时调用返回失败。

因此 server 的 handover 流程是：

```text
旧进程请求 runas 启动新进程
  → 旧进程准备退出
  → 新进程等待旧 PID 结束
  → 新进程再绑定原端口
```

---

## 32. 等待旧进程与当前用户启动项

提升后的新 server 如果立刻监听，旧进程可能仍占用端口。项目使用带 timeout 的 process wait 完成 handover，并通过 `QSettings` 管理当前用户的 startup registry entry。

### 32.1 `waitForProcessExit()` 的三个参数

```cpp
bool waitForProcessExit(
    quint32 _processId,
    int _timeoutMs,
    QString* _errorMessage = nullptr);
```

- `_processId`：需要等待的旧 Windows process ID，0 无效。
- `_timeoutMs`：最多等待的毫秒数；项目使用有界 timeout。
- `_errorMessage`：可选错误输出，失败或超时时供 UI 显示。

这个函数在 server startup、创建 listening server 之前执行。它不会在正常 GUI 交互期间长期阻塞 event loop。

### 32.2 `OpenProcess()` 的三个参数

```cpp
HANDLE const process{OpenProcess(
    SYNCHRONIZE,
    FALSE,
    static_cast<DWORD>(_processId))};
```

参数：

1. `dwDesiredAccess`
   - 所需 access right；这里只等待退出，所以请求 `SYNCHRONIZE`。
2. `bInheritHandle`
   - 新 child process 是否继承 handle；项目传 `FALSE`。
3. `dwProcessId`
   - 要打开的 process ID。

如果调用失败且 `GetLastError()` 为 `ERROR_INVALID_PARAMETER`，项目把它视为旧进程已经先行结束。这是成功完成 handover，而不是需要报错的异常。

### 32.3 `WaitForSingleObject()` 的两个参数

```cpp
DWORD const waitResult{WaitForSingleObject(
    process,
    timeout)};
```

- `hHandle`：具有 `SYNCHRONIZE` 权限的 process handle。
- `dwMilliseconds`：最大等待毫秒数。

主要结果：

- `WAIT_OBJECT_0`：process 已退出，handover 可继续。
- `WAIT_TIMEOUT`：限定时间内没有退出。
- `WAIT_FAILED`：等待调用失败，应读取错误并停止启动。

无论结果如何，都要调用 `CloseHandle(HANDLE hObject)`：

- `hObject`：由 `OpenProcess()` 返回的 process handle。

关闭 handle 不会终止目标 process，只释放当前进程持有的 kernel object reference。

### 32.4 为什么不能无限等待

如果旧进程卡死，传 `INFINITE` 会让新 server 永久停在启动阶段，用户只能从 Task Manager 处理。有限 timeout 能把失败转成明确消息，也保证不会出现两个进程都不再提供服务却一直等待的状态。

### 32.5 startup entry 的三个公开操作

```cpp
bool installStartupEntry(
    QString* _errorMessage = nullptr);

bool removeStartupEntry(
    QString* _errorMessage = nullptr);

bool startupEntryExists();
```

- 两个 `_errorMessage` 都是可选输出指针，用于返回用户可读的 registry 写入错误。
- `startupEntryExists()` 没有参数，只查询当前值是否存在。

注册位置是当前用户的 Run key：

```text
HKEY_CURRENT_USER
  \Software
  \Microsoft
  \Windows
  \CurrentVersion
  \Run
```

使用 `HKEY_CURRENT_USER` 通常不需要管理员权限，影响的也是当前登录用户。

### 32.6 `QSettings` 怎样映射 registry

```cpp
QSettings settings{
    startupRegistryPath(),
    QSettings::NativeFormat};

settings.setValue(
    startupValueName(),
    startupCommand());
settings.sync();
```

`QSettings(QString const& fileName, Format format)` 的参数：

- `fileName`：这里不是普通文件，而是 native registry path。
- `format`：`NativeFormat` 表示使用 Windows registry backend。

`setValue(QString const& key, QVariant const& value)` 的参数：

- `key`：Run key 下的 value name，例如 server 应用名。
- `value`：登录后要执行的 command line。

`sync()` 没有参数，把内存中的修改写到 backend；随后检查 `status()`，不能把“调用过 setValue”直接当成写入成功。

### 32.7 startup command 为什么要引用 executable path

```text
错误：
C:\Program Files\Remote Control\Server.exe

正确：
"C:\Program Files\Remote Control\Server.exe"
```

没有双引号时，Windows 可能把第一个空格前的片段误认为 executable。项目统一通过 argument quoting helper 生成 startup command。

install、remove 和 elevate 都是一次性 startup action：处理完成后进程退出，不继续创建 listening server。这样命令行管理操作不会意外启动第二个后台服务实例。

---

## 33. 映射到项目源码

以下位置都相对于源项目根目录：

> `D:\CodeRepository\claude\remote_control`

### 33.1 第一遍：只看架构和对象归属

1. **客户端架构说明**
   - 位置：`docs/ClientArchitecture.md:1`
   - 观察：GUI thread、三个 worker thread、四种连接模型和四类 generation。
2. **客户端公开边界**
   - 位置：`include/client/RemoteClient.h:15`
   - 观察：UI 能调用哪些方法、结果怎样通过 signal 返回。
3. **worker 类型声明**
   - 位置：`include/client/ScreenStreamWorker.h:1`
   - 位置：`include/client/ControlStreamWorker.h:1`
   - 位置：`include/client/FileDownloadWorker.h:1`
   - 观察：每个 worker 的 state、socket、timer 和 generation 字段。

第一遍先回答“对象属于哪个线程”，不要立即陷入每个 packet 分支。

### 33.2 第二遍：跟踪客户端四种连接模型

1. **一次性请求**
   - 位置：`src/client/RemoteClient.cpp:73`
   - 观察：`OneShotRequest` 的 parent、timer、增量解析、完成状态和 `CallbackScope`。
2. **worker 创建与总停机**
   - 位置：`src/client/RemoteClient.cpp:463`
   - 观察：`moveToThread()`、`started → initialize` 和 destructor 中的停止顺序。
3. **endpoint 与 generation**
   - 位置：`src/client/RemoteClient.cpp:587`
   - 观察：四类 generation 何时递增、pending screen frame 怎样释放。
4. **下载投递**
   - 位置：`src/client/RemoteClient.cpp:676`
   - 观察：host、port、path 和两级 generation 怎样按值捕获。
5. **屏幕投递与停止**
   - 位置：`src/client/RemoteClient.cpp:700`
   - 观察：one-in-flight gate、screen generation 与 queued close。
6. **控制流投递**
   - 位置：`src/client/RemoteClient.cpp:760`
   - 观察：鼠标事件如何进入 control worker，回调如何在 GUI thread 过滤。

### 33.3 第三遍：跟踪三个 worker 状态机

1. **屏幕 worker**
   - 位置：`src/client/ScreenStreamWorker.cpp:27`
   - 观察：`Idle → FramePending → Idle`、socket 复用和 `requestFinished`。
2. **控制 worker**
   - 位置：`src/client/ControlStreamWorker.cpp:30`
   - 观察：endpoint 变化、handshake、one in flight、queue 和 move coalescing。
3. **下载 worker**
   - 位置：`src/client/FileDownloadWorker.cpp:35`
   - 观察：单 active download、两级 generation、size header、精确写入，以及未启用 direct-write fallback 的 `QSaveFile`。

阅读每个 callback 时固定记录四项：

```text
进入 callback 时允许哪些 state
  → 修改了哪些 state
  → 是否停止或重启 timer
  → 发出了哪些带 generation 的 signal
```

### 33.4 第四遍：把 GUI 调度接到 worker

1. **远程屏幕窗口**
   - 位置：`src/client/RemoteScreenWindow.cpp:18`
   - 观察：首帧、33 ms 最小间隔和 close event。
2. **远程屏幕 widget**
   - 位置：`src/client/RemoteScreenWidget.cpp:30`
   - 观察：坐标换算、16 ms move throttle 与输入顺序。

这一遍要从 signal 出发反向寻找 sender 和 receiver，确认每个 slot 最终在哪个线程执行。

### 33.5 第五遍：从协议能力进入 Windows 实现

1. **平台无关 host interface**
   - 位置：`server_transport/include/RemoteControlHostServices.h:9`
   - 观察：dispatcher 可见的六类主机能力及线程约束。
2. **Windows host adapter**
   - 位置：`src/server/WindowsRemoteControlHostServices.cpp:11`
   - 观察：协议结构转换与 GUI lock request 的 queued marshal。
3. **截图资源上下文**
   - 位置：`src/server/WindowsPlatformIntegration.cpp:123`
   - 观察：DIB ownership、`thread_local` reuse、PNG 编码边界，以及当前未调用 `GdiFlush()` 的加固点。
4. **全局鼠标输入**
   - 位置：`src/server/WindowsPlatformIntegration.cpp:286`
   - 观察：process-wide mutex、action mapping、`SetCursorPos()` 和 `SendInput()`。
5. **模拟锁屏状态**
   - 位置：`src/server/WindowsPlatformIntegration.cpp:357`
   - 观察：原状态保存、部分失败回滚和幂等恢复。
6. **Shell 与进程能力**
   - 位置：`src/server/WindowsPlatformIntegration.cpp:445`
   - 观察：open、runas、token membership、bounded wait 和 startup entry。
7. **锁屏 GUI 服务**
   - 位置：`src/server/ScreenLockService.cpp:14`
   - 位置：`src/server/ScreenLockWindow.cpp:28`
   - 观察：GUI-only 操作、状态变更 signal 和本地恢复入口。
8. **一次性 startup actions**
   - 位置：`src/server/ServerMain.cpp:76`
   - 观察：elevate、startup management 和 wait-for-pid 为什么早于 server listen。

---

## 34. 常见错误与直接后果

遇到异常时，优先按线程、网络状态、generation、GDI 资源和系统 API 五类边界定位。

### 34.1 线程归属与退出边界

1. **先创建活动 socket，再移动 worker**
   - socket 可能仍属于 GUI thread，worker slot 随后跨线程操作它。
   - 应在 worker 到达目标线程后创建 thread-bound resource，或保证 parent/child 整体合法移动且尚未启动活动操作。
2. **把 `QThread` object 当成 managed thread**
   - `QThread` 的 queued slot 仍可能在创建它的 GUI thread 执行。
   - 业务 slot 应放在 move 到目标线程的 worker object 上。
3. **从 GUI thread 直接调用 worker 普通方法**
   - 普通调用在调用线程立即执行，不会因为对象属于 worker thread 就自动排队。
   - 跨线程业务入口使用 queued signal/slot 或 `invokeMethod()`。
4. **先 `quit()`，再投递 shutdown**
   - event loop 已停止后，普通 queued cleanup 没有执行机会。
   - 同一个 worker-thread callback 内先清资源，再 quit，最后由 owner thread wait。
5. **把 `BlockingQueuedConnection` 当成通用同步工具**
   - 同线程使用会死锁；跨线程使用也会让调用线程停等 receiver。
   - 只有确实需要同步结果、能够证明线程不同且不存在反向等待时才考虑它。
6. **在 callback 中 `delete this`**
   - 当前 signal/callback 栈或 nested event loop 仍可能访问已释放对象。
   - 先进入完成状态，再由 callback-depth guard 安排 `deleteLater()`。

### 34.2 网络请求与状态机边界

1. **屏幕帧只在成功时发 completion**
   - 一次 timeout、断连或 PNG 解码失败就会永久占住 in-flight 槽位。
   - success 和 failure 都发 `requestFinished`，图像结果与请求结束分开表达。
2. **固定每 33 ms 无条件提交屏幕请求**
   - 慢网络下请求持续堆积，延迟和内存一起增长。
   - 当前帧结束后才安排下一帧，33 ms 只是最小提交间隔。
3. **把 `write()` 成功当成命令完成**
   - 它只证明 Qt 接受字节进入本地 buffer，对端可能尚未收到或执行。
   - 业务完成必须等待 command 匹配的 response。
4. **control response 不匹配时继续猜测**
   - 当前 in-flight command 与响应失去对应关系，后续 queue 也不再可信。
   - 关闭该连接并失败所有未完成命令，不在新连接上自动重放。

### 34.3 generation、背压与下载边界

1. **只用 endpoint generation 过滤所有业务**
   - 同一服务器上的旧下载、旧 screen stream 或旧 control stream 仍可能污染新状态。
   - endpoint、download、screen 和 control 使用符合各自生命周期的编号。
2. **worker 执行时才读取最新 generation**
   - 旧任务会错误携带新编号，绕过 stale-result gate。
   - 提交时按值捕获 endpoint、业务参数和 generation。
3. **无限积累鼠标 move**
   - 网络恢复后会回放过时轨迹，cursor 明显滞后。
   - GUI throttle、worker 相邻 move coalescing 与 bounded queue 同时存在。
4. **跨越 button command 合并 move**
   - press/release 对应坐标会改变，远程点击落在错误位置。
   - 只能替换队尾连续的 move-only command。
5. **下载一开始就 truncate 目标文件**
   - timeout、断连或进程退出后，原文件已丢失，只留下部分数据。
   - 使用默认禁止 direct-write fallback 的 `QSaveFile`，收齐数据后才 commit。

### 34.4 GDI 与全局输入边界

1. **删除仍选入 DC 的 bitmap**
   - `DeleteObject()` 可能失败并造成 GDI resource 泄漏。
   - 先用 `SelectObject()` 恢复 previous object，再删除 memory DC 和 bitmap。
2. **把借用 DIB 的 `QImage` 传到其他线程**
   - DIB 复用或销毁后，image 会访问被覆盖或悬空的像素。
   - 在 DIB 有效期内完成编码；跨线程只传 owning bytes 或 deep copy。
3. **忽略 GDI 绘制与 CPU 访问的同步边界**
   - CPU 可能在 GDI batch 尚未完成时读取 DIB bits。
   - 直接访问 `pixelData` 前用 `GdiFlush()` 加固同步。
4. **让多个线程交错定位与按钮注入**
   - 另一个 connection 可在 `SetCursorPos()` 与 `SendInput()` 之间移动 cursor。
   - process-wide mutex 覆盖一个逻辑事件的完整定位和注入过程。

### 34.5 Windows UI、Shell 与 handle 边界

1. **把 simulated lock 当作 Windows 安全锁**
   - overlay 无法提供身份认证、secure desktop 或不可绕过保证。
   - 明确它只是可恢复的 UI 限制，并恢复进入前的 taskbar 与 cursor 状态。
2. **把 `ShowWindow()` 返回 0 当成失败**
   - 该值描述调用前是否可见，本来隐藏的窗口也会返回 0。
   - 原状态用 `IsWindowVisible()` 读取，不用返回值推断本次成功。
3. **把 `ShellExecuteW()` 返回值当成 process handle**
   - 它是历史兼容值，不能用于 `CloseHandle()` 或等待。
   - 这里只转成 `INT_PTR` 与 32 比较；需要 handle 时改用能返回 process handle 的启动 API。
4. **handover 无限等待旧进程**
   - 新 server 可能永久卡在启动，遗漏 `CloseHandle()` 还会泄漏 kernel handle。
   - 使用有限 timeout，并在所有等待结果后关闭 process handle。

---

## 35. 阶段练习与验收

按顺序完成以下任务。每题先独立推演，再核对完成判定和参考答案；只需阅读源码与书写时序，不需要新增或运行示例项目。

### 35.1 任务一：判断对象归属与代码执行线程

**任务**

已知：

```text
RemoteClient 在 GUI thread 创建
QThread 对象也在 GUI thread 创建
ScreenStreamWorker move 到 screen thread
worker 的 QTcpSocket 在 screen thread 创建
```

分别判断以下代码在哪个线程执行：

1. GUI button slot 调用 `RemoteClient::requestScreenFrame()`。
2. `invokeMethod(worker, lambda, QueuedConnection)` 中的 lambda。
3. worker 的 `QTcpSocket::readyRead` slot。
4. worker 发出 `frameReady` 后，连接到 `RemoteClient` context 的 lambda。
5. `QThread::started` 连接到 worker initialization slot。
6. `RemoteClient` destructor 调用 `QThread::wait()`。

**完成判定**

- [ ] 不把 `QThread` object 所在线程与 managed thread 混为一谈。
- [ ] 能用 receiver/context affinity 判断 queued callback 的执行线程。
- [ ] 知道 `wait()` 阻塞调用它的 destructor thread，不会自动进入 worker thread。
- [ ] 知道 socket callback 必须在 socket 所属线程执行。

**参考答案**

1. GUI thread。
2. screen thread，因为 context object 是 screen worker。
3. screen thread，socket 和 worker 都属于该线程。
4. GUI thread，因为 context 是 `RemoteClient`。
5. worker 所属线程；thread 启动后 queued delivery 到 worker。
6. GUI/destructor thread；它等待 screen thread 结束。

一句话判断法：

> 普通函数调用看当前调用线程；queued delivery 看 receiver/context 的 thread affinity。

### 35.2 任务二：推演短请求的拆包、完成与删除

**任务**

一个目录请求依次收到：

```text
readyRead 1：entry A 的前半包
readyRead 2：entry A 后半包 + entry B 完整包
readyRead 3：terminating packet
```

第三次 `readyRead` 发结果 signal 时，UI slot 打开 modal dialog。请写出：

1. 每次读取后 buffer 和 parser 应做什么。
2. 哪一刻可以进入 `Finished`。
3. 为什么要在 emit 前 `markFinished()`。
4. dialog 产生 nested event loop 时，`CallbackScope` 怎样延迟清理。
5. 如果 terminating packet 前断开，应报告什么结果。

**完成判定**

- [ ] 第一次读取不会把半包当错误，也不会丢弃。
- [ ] 第二次读取能连续取出两个完整 entry。
- [ ] 只有 terminating packet 能证明目录完整。
- [ ] emit 前状态已不再 `Active`。
- [ ] callback depth 归零后才安排 `deleteLater()`。
- [ ] 提前断开是失败，不返回部分目录成功。

**参考答案**

```text
readyRead 1
  → append 半包
  → tryParse 无完整 packet
  → 保留 buffer，等待

readyRead 2
  → append 新字节
  → parse entry A
  → parse entry B
  → buffer 暂时为空
  → 请求仍 Active

readyRead 3
  → parse terminating packet
  → markFinished，停止 timer
  → emit 完整目录
  → 请求断开 socket
  → callback 未退栈时只记 CleanupDeferred
  → CallbackScope 析构后安排 deleteLater
```

terminating packet 前断开说明响应不完整，应报告目录流提前结束。

### 35.3 任务三：填写四类 generation 账本

**任务**

这些任务开始前的值：

```text
endpoint = 3
download = 7
screen = 5
control = 9
```

当前有：

```text
one-shot 携带 E3
发起下载 A 后，当前 download = 8，A 携带 (E3, D8)
屏幕帧携带 S5，仍 pending
控制命令携带 C9，仍 in flight
```

用户切换 endpoint，取消结果先到达，然后才在新 endpoint 发起下载 B。写出：

1. `setEndpoint()` 后四个当前值。
2. 取消结果携带的两个值。
3. 下载 B 携带的两个值。
4. 上述五类晚到结果分别接受还是丢弃。

**完成判定**

- [ ] endpoint 与 download 各增加一次。
- [ ] `stopScreenStream()` 和 `stopControlStream()` 分别增加对应 generation。
- [ ] 取消结果使用切换后的当前两级 generation。
- [ ] 下载 B 再次增加 download generation。
- [ ] 旧 one-shot、旧下载、旧帧和旧控制结果全部被过滤。
- [ ] 能说明 pending frame 为什么需要 UI 侧即时 completion。

**参考答案**

切换后：

```text
endpoint = 4
download = 9
screen = 6
control = 10
```

下载 A 已把 D7 增加到 D8；endpoint 切换再把它增加到 D9。取消结果携带 `(E4, D9)`，在题设的到达顺序下被接受。

随后下载 B 再增加一次，携带 `(E4, D10)`。

```text
one-shot E3       → 丢弃
下载 A (E3, D8)   → 丢弃
旧屏幕 S5         → 丢弃
旧控制 C9         → 丢弃
取消 (E4, D9)     → 接受
下载 B (E4, D10)  → 接受
```

旧 S5 completion 会被过滤，所以 `setEndpoint()` 要主动释放 GUI 看到的 pending slot，允许新 S6 帧继续调度。

### 35.4 任务四：计算屏幕帧调度时间

**任务**

最小 frame request interval 为 33 ms。依次发生：

1. 第一帧成功，用时 12 ms。
2. 第二帧成功，用时 48 ms。
3. 第三帧 PNG 解码失败，用时 8 ms。
4. 第四帧提交后，窗口在响应前关闭。

分别写出每次完成后 timer delay，并说明第四帧怎样收尾。

**完成判定**

- [ ] 第一帧完成后等待 21 ms。
- [ ] 第二帧完成后 delay 为 0，而不是并发补帧。
- [ ] 第三帧失败后仍有 completion，并等待 25 ms。
- [ ] 关闭后 timer 停止、pending move 取消、screen/control stream generation 失效。
- [ ] 旧第四帧结果不能重新启动窗口调度。

**参考答案**

```text
第一帧：33 - 12 = 21 ms
第二帧：耗时已超过 33 ms，delay = 0
第三帧：33 - 8 = 25 ms
```

第三帧没有 `frameReady`，但有 `requestFinished`。第四帧关闭时，窗口先阻止本地 timer 和 mouse timer，再让两个 worker 关闭连接；晚到结果因 generation 不匹配被丢弃。

### 35.5 任务五：压缩控制队列但保持输入顺序

**任务**

control channel 已为 `Ready`，当前有一个 lock command in flight。随后按顺序入队：

```text
Move A
Move B
Press Left
Move C
Move D
Unlock
```

回答：

1. 最终 pending queue 中保留什么。
2. lock response 成功后先发送什么。
3. 若收到的 response command 与 in-flight command 不一致，如何处理。
4. 为什么不能把 `Move B` 与 `Move D` 合并。

**完成判定**

- [ ] `Move A` 被 `Move B` 替换。
- [ ] `Move C` 被 `Move D` 替换。
- [ ] `Press Left` 和 `Unlock` 保持原顺序。
- [ ] lock 完成后只发送队首，不并发发送全部命令。
- [ ] response mismatch 使当前连接失去对应关系，清理并失败所有未完成命令。

**参考答案**

最终 queue：

```text
Move B
Press Left
Move D
Unlock
```

lock 响应完成并清空 in-flight slot 后，先发送 `Move B`。每个响应完成后才发送下一个。

`Move B` 与 `Move D` 中间有 `Press Left`。跨越它合并会改变按下发生的坐标，所以只能合并队尾连续 move-only command。

---

### 35.6 任务六：判断下载何时可以提交

**任务**

服务端声明文件大小为 10 bytes。分析两组数据：

```text
路径 A：4 bytes，3 bytes，4 bytes
路径 B：4 bytes，3 bytes，3 bytes
```

再分析一个 size header 为 0 的空文件。分别说明 `writtenBytes`、`commit()`、`cancelWriting()` 和目标文件结果。

**完成判定**

- [ ] 路径 A 在第三块写入前发现 7 + 4 大于 10。
- [ ] 路径 A 不 commit，临时写入被取消，原目标不被部分内容替换。
- [ ] 路径 B 只有在 4 + 3 + 3 精确等于 10 后 commit。
- [ ] 每次 `write()` 都要求返回完整 payload 长度。
- [ ] 空文件在 header 后立即 commit，不等待不存在的数据包。
- [ ] 知道原目标可保留的前提是没有启用 direct-write fallback。

**参考答案**

```text
路径 A
  → 写 4，written = 4
  → 写 3，written = 7
  → 下一块会到 11，先判 overflow
  → fail
  → cancelWriting
  → 不 commit

路径 B
  → written 依次为 4、7、10
  → 精确等于 expected size
  → commit

空文件
  → expected size = 0
  → progress 可报告 0 / 0
  → 立即 commit
```

上述结果以项目保留 `QSaveFile` 默认的 `directWriteFallback == false` 为前提；若主动启用直接写回退，就不再具备相同的原子替换保证。

### 35.7 任务七：排列 GDI 截图资源顺序

**任务**

把以下动作排成一次首次截图与最终销毁的推荐顺序；其中 `GdiFlush()` 是依据官方文档补充的加固点，当前项目源码尚未调用：

```text
BitBlt
CreateCompatibleDC
CreateDIBSection
DeleteDC
DeleteObject(bitmap)
GdiFlush
GetDC(nullptr)
PNG encode
QImage 借用 pixelData
ReleaseDC(nullptr, screenDc)
SelectObject(memoryDc, bitmap)
SelectObject(memoryDc, previousObject)
```

并回答：

1. 为什么 `biHeight` 为负数。
2. 谁拥有 `pixelData`。
3. 为什么不能把借用像素的 `QImage` 留到下一帧。
4. `GdiFlush()` 应位于哪个边界，解决什么问题。
5. 相同尺寸的下一帧可以省略哪些创建步骤。

**完成判定**

- [ ] screen DC 用 `ReleaseDC()`，memory DC 用 `DeleteDC()`。
- [ ] bitmap 删除前恢复 previous object。
- [ ] `BitBlt()` 后、CPU 读取 `pixelData` 前完成 GDI batch flush。
- [ ] PNG 在 DIB 仍有效且未被下一帧覆盖时完成。
- [ ] `HBITMAP` 拥有像素存储，裸指针与 `QImage` 都不 owning。
- [ ] 知道 context 可复用 memory DC、DIB 和 selected bitmap。

**参考答案**

首次捕获：

```text
GetDC(nullptr)
  → CreateCompatibleDC
  → CreateDIBSection
  → SelectObject(memoryDc, bitmap)
  → BitBlt
  → GdiFlush
  → ReleaseDC(nullptr, screenDc)
  → QImage 借用 pixelData
  → PNG encode
```

context 最终销毁：

```text
SelectObject(memoryDc, previousObject)
  → DeleteDC(memoryDc)
  → DeleteObject(bitmap)
```

负高度创建 top-down DIB，第一行像素直接对应图像顶部。

像素存储属于 bitmap；下一帧 `BitBlt()` 会覆盖它，因此借用 image 不能跨帧保留。

`GdiFlush()` 位于 GDI 绘制与 CPU 读取 DIB bits 之间。屏幕尺寸不变时，下一帧复用 memory DC 与 DIB，只重新取得 screen DC、复制、flush 和编码。

### 35.8 任务八：推演并发鼠标输入与模拟锁屏恢复

**任务**

先分析没有 mutex 的交错：

```text
线程 A：SetCursorPos(100, 100)
线程 B：SetCursorPos(500, 500)
线程 A：SendInput(LeftDown, LeftUp)
```

再分析模拟锁屏前的状态：

```text
taskbar 原本隐藏
cursor 原本被另一个程序限制在 RECT R
```

回答：

1. A 的 click 实际可能落在哪里。
2. process-wide mutex 应覆盖哪一段。
3. unlock 后 taskbar 和 cursor clipping 应恢复成什么。
4. `ShowWindow(taskbar, SW_HIDE)` 返回 0 能否证明失败。
5. 为什么该功能不能称为 Windows 安全锁。

**完成判定**

- [ ] 能指出 A 的 click 可能落在 B 的位置。
- [ ] mutex 同时覆盖定位与按钮注入。
- [ ] unlock 恢复 taskbar 隐藏状态和原 rectangle R。
- [ ] 不把 `ShowWindow()` 返回值当成成功标志。
- [ ] 能区分 UI overlay 与 secure desktop/session lock。

**参考答案**

没有 mutex 时，A 在定位后被 B 改写 cursor，点击可能发生在 `(500, 500)`。锁必须包住一个命令的 `SetCursorPos()` 与随后完整 `SendInput()`。

unlock 不能显示本来就隐藏的 taskbar，也不能无条件 `ClipCursor(nullptr)`；应恢复隐藏状态和 rectangle R。`ShowWindow()` 返回 0 只说明调用前窗口不可见。

模拟锁屏只是本进程维护的 overlay 和系统 UI 临时调整，无法提供 Windows authentication 或 secure desktop 的安全保证。

### 35.9 任务九：推演 elevation handover

**任务**

旧 server 进程 PID 为 4200，用户请求 elevation。新进程收到 `--wait-for-pid 4200`。分别推演：

1. 新进程调用 `OpenProcess()` 前旧进程已经退出，错误为 `ERROR_INVALID_PARAMETER`。
2. 成功取得 process handle，3 秒内旧进程退出。
3. 成功取得 process handle，但直到 timeout 仍未退出。

每条路径说明是否继续启动 server、何时 `CloseHandle()`，以及为什么 `ShellExecuteW()` 的成功返回值不能用于等待。

**完成判定**

- [ ] `ERROR_INVALID_PARAMETER` 被视为旧 PID 已消失，可以继续。
- [ ] `WAIT_OBJECT_0` 后关闭 handle 并继续绑定端口。
- [ ] `WAIT_TIMEOUT` 后关闭 handle、报告错误并停止启动。
- [ ] 不把 `ShellExecuteW()` 返回值当 process handle。
- [ ] 不使用无限等待。

**参考答案**

```text
路径 1
  → 旧进程已不存在
  → 无有效 handle 需要关闭
  → wait 视为成功
  → 继续启动

路径 2
  → OpenProcess 成功
  → WaitForSingleObject 返回 WAIT_OBJECT_0
  → CloseHandle
  → 继续启动

路径 3
  → OpenProcess 成功
  → 返回 WAIT_TIMEOUT
  → CloseHandle
  → 显示 handover timeout
  → 不绑定端口
```

`ShellExecuteW()` 在本项目中只给出大于 32 的成功指示，不提供可等待的新 process handle。

### 35.10 任务十：最终全链路推演

**任务**

闭卷推演下面的完整场景：

```text
用户打开远程屏幕
  → 第一帧请求成功
  → 鼠标快速移动后按下左键
  → 同时开始下载文件
  → 屏幕第二帧和下载都未完成时切换 endpoint
  → 新 endpoint 请求一帧
  → 用户关闭远程屏幕窗口
  → 用户退出客户端
```

推演中必须包含：

- GUI thread、screen thread、control thread 和 download thread 的边界。
- screen one-in-flight 与 33 ms 调度。
- 16 ms move throttle 和 control queue 顺序。
- endpoint、download、screen、control generation 的变化。
- 旧帧、旧下载和旧控制结果的处理。
- 三个 worker 的 shutdown、thread quit 和 wait。

**完成判定**

- [ ] 第一帧由 GUI queued 投递到 screen worker，结果再回 GUI。
- [ ] 下一帧只在第一帧 `requestFinished` 后调度。
- [ ] 按键前 flush 最新 move，worker 不跨按键合并 move。
- [ ] 下载在 worker thread 创建 socket 和 `QSaveFile`。
- [ ] endpoint 切换使四类旧业务结果失效，并投递资源清理。
- [ ] pending screen scheduler 被主动释放，可向新 endpoint 请求。
- [ ] 窗口关闭取消 frame timer 和 pending move，并停止 screen/control stream。
- [ ] 客户端 destructor 先让每个 worker 在自身线程 shutdown，再 quit 和 wait。
- [ ] 不从 GUI thread 直接删除或操作 worker socket。

**参考答案与解释**

```text
GUI 显示窗口
  → requestScreenFrame 捕获 host/port/S0
  → queued 到 screen thread
  → worker 建立或复用 socket
  → 发送一帧请求，状态 FramePending
  → 响应解码后状态 Idle
  → frameReady(S0) 与 requestFinished(S0)
  → GUI 过滤 S0，显示图像并计算下一帧 delay

GUI 连续收到 move
  → 16 ms 内只保留最新位置
  → press 前 flush 最新 move
  → 两个事件 queued 到 control thread
  → control connection 完成 handshake
  → move 与 press 按 one-in-flight 顺序发送

GUI 发起下载
  → D 递增
  → 捕获 E/D 与两个路径
  → queued 到 download thread
  → worker 创建 QSaveFile 和 socket

用户切换 endpoint
  → E、D 递增
  → screen generation 递增并 queued close
  → control generation 递增并 queued close
  → download worker queued cancel
  → 旧 E/D、旧 S、旧 C 结果全部不能更新当前 UI
  → GUI 释放旧 pending frame scheduler
  → 新帧捕获新 endpoint 与新 S

用户关闭远程窗口
  → 停 frame timer
  → 取消 pending move
  → 再次结束当前 screen/control generation
  → queued 关闭两个长连接

用户退出客户端
  → 对 screen worker 投递 shutdown + current thread quit
  → wait screen thread
  → 对 control worker 执行同样顺序
  → 对 download worker 取消临时写入、reset socket、quit
  → wait download thread
  → 所有 worker-owned QObject 资源均在 owner thread 收尾
```

完成任务十后，应能独立解释：为什么异步客户端的正确性同时依赖 event loop、thread affinity、状态机、generation、bounded queue 和所有权顺序，而不是只依赖“网络 API 是异步的”。

---

## 36. 从客户端操作到 Windows 能力的最终闭环

阶段十把前九个阶段已经完成的服务端闭环接到了真实客户端和主机平台能力上。此时可以从用户动作出发，而不是从某个孤立 API 出发理解系统。

### 36.1 屏幕画面链

```text
RemoteScreenWindow 调度一帧
  → RemoteClient 捕获 endpoint 与 screen generation
  → queued 到 ScreenStreamWorker
  → QTcpSocket 发送 WatchScreen
  → 服务端 IOCP 接收并 dispatch
  → screen task 调用 host services
  → GDI 复制 primary screen 到 DIB
  → worker 内编码 PNG
  → 服务端有序发送响应
  → 客户端 worker 增量解析并解码 QImage
  → GUI generation gate
  → RemoteScreenWidget 绘制
  → requestFinished 安排下一帧
```

这条链同时包含 Qt event loop、客户端 worker thread、服务端 IOCP、bounded task worker、GDI resource 和两端协议状态。

### 36.2 鼠标控制链

```text
RemoteScreenWidget 捕获本地事件
  → 坐标换算到远程截图尺寸
  → 16 ms move throttle
  → RemoteClient 捕获 control generation
  → queued 到 ControlStreamWorker
  → handshake 后进入 one-in-flight queue
  → 服务端 control connection 解析 MouseEvent
  → host services 转换协议枚举
  → process-wide mutex
  → SetCursorPos
  → SendInput
  → status response
  → 客户端完成当前 command，再发下一个
```

队列顺序保证协议对应，mutex 保证 Windows 全局输入步骤不被其他 connection 交错；两者不能互相替代。

### 36.3 文件下载链

```text
GUI 选择 remote path 与 local path
  → endpoint/download generation 快照
  → queued 到 FileDownloadWorker
  → worker 创建 QSaveFile 与 QTcpSocket
  → 服务端文件 task 读取并分包
  → 客户端先解析 qint64 size header
  → 每块数据检查 overflow 和 short write
  → 精确收齐 declared bytes
  → QSaveFile commit
  → GUI generation gate 接受最终结果
```

网络成功与本地文件事务成功是两个完成点。收到最后一个 packet 后仍要以 `commit()` 结果决定最终状态。

### 36.4 生命周期链

```text
endpoint 变化
  → generation invalidation
  → queued close/cancel
  → 晚到结果丢弃

窗口关闭
  → 停止本地生产入口
  → 结束相关 stream

客户端退出
  → worker thread 内 shutdown resource
  → 当前 worker event loop quit
  → owner thread wait
```

这里与阶段九的服务端停机原则一致：先阻止新工作，清理旧工作时保留其执行上下文，最后才退出 worker。

### 36.5 十个阶段怎样连接起来

1. **阶段一**定义 TCP byte stream 与项目协议边界。
2. **阶段二**建立 Win32 handle、错误码和同步 Winsock 基础。
3. **阶段三**理解 `OVERLAPPED` 代表一笔异步 operation。
4. **阶段四**建立最小 IOCP completion loop。
5. **阶段五**把 `AcceptEx()` 与 server startup 接入 IOCP。
6. **阶段六**贯通最短 `TestConnection` 业务链。
7. **阶段七**加入连接状态、有序发送和背压。
8. **阶段八**把阻塞文件、截图等工作隔离到 bounded task pool。
9. **阶段九**完成取消、关闭、pending 对账与安全停机。
10. **阶段十**完成 Qt 客户端调度、跨线程生命周期与 Windows 平台能力。

阶段十没有重新发明协议或 IOCP，而是消费前面已经建立的可靠服务端语义。

### 36.6 最终应保持的系统不变量

- GUI thread 不直接操作 worker-owned socket、timer 或 output file。
- worker-owned QObject resource 在 worker thread 创建、使用和清理。
- 每个异步结果都能判断自己是否仍属于当前业务 generation。
- screen stream 同时最多一帧在途。
- control stream 同时最多一个 command 等待响应。
- 鼠标 move 可以丢弃中间位置，但不能跨越 button/lock command 改变顺序。
- 下载只有 declared byte count 精确满足且 `commit()` 成功才算完成。
- endpoint 切换同时处理 resource cleanup 与 stale-result filtering。
- GDI handle、borrowed pixel pointer 和 Qt image wrapper 的所有权不混淆。
- simulated lock 总是恢复进入前的桌面状态，并且不被描述成安全边界。
- elevation handover 使用有限等待，不让两个 server 争抢同一端口。
- 客户端和服务端都先关闭新工作入口，最后才结束负责收尾的线程或 event loop。

可以用一句话概括阶段十：

> Qt event loop 负责调度，thread affinity 规定执行位置，状态机限制并发形态，generation 淘汰旧结果，平台层在明确的资源与权限边界内完成 Windows 操作。

---

## 37. 官方资料与项目资料

### 37.1 Qt 官方资料

- [Threads and QObjects](https://doc.qt.io/qt-6/threads-qobject.html)：thread affinity、event-driven object 和跨线程约束。
- [QThread](https://doc.qt.io/qt-6/qthread.html)：managed thread、event loop、`quit()` 与 `wait()`。
- [Qt::ConnectionType](https://doc.qt.io/qt-6/qt.html#ConnectionType-enum)：direct、queued 和 blocking queued 语义。
- [QMetaObject](https://doc.qt.io/qt-6/qmetaobject.html)：`invokeMethod()` 与 queued invocation。
- [QAbstractSocket](https://doc.qt.io/qt-6/qabstractsocket.html)：异步连接、socket state 和 signal。
- [QSaveFile](https://doc.qt.io/qt-6/qsavefile.html)：临时写入、`commit()` 和安全替换目标文件。

### 37.2 Microsoft 官方资料

- [GetDC](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getdc)
- [CreateCompatibleDC](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-createcompatibledc)
- [CreateDIBSection](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-createdibsection)
- [BitBlt](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-bitblt)
- [GdiFlush](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-gdiflush)
- [SetCursorPos](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setcursorpos)
- [SendInput](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-sendinput)
- [ClipCursor](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-clipcursor)
- [ShowWindow](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-showwindow)
- [ShellExecuteW](https://learn.microsoft.com/en-us/windows/win32/api/shellapi/nf-shellapi-shellexecutew)
- [CheckTokenMembership](https://learn.microsoft.com/en-us/windows/win32/api/securitybaseapi/nf-securitybaseapi-checktokenmembership)
- [OpenProcess](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-openprocess)
- [WaitForSingleObject](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-waitforsingleobject)

### 37.3 项目资料

- `docs/ClientArchitecture.md`
- `include/client/RemoteClient.h`
- `src/client/RemoteClient.cpp`
- `src/client/ScreenStreamWorker.cpp`
- `src/client/ControlStreamWorker.cpp`
- `src/client/FileDownloadWorker.cpp`
- `src/client/RemoteScreenWindow.cpp`
- `src/client/RemoteScreenWidget.cpp`
- `server_transport/include/RemoteControlHostServices.h`
- `src/server/WindowsRemoteControlHostServices.cpp`
- `src/server/WindowsPlatformIntegration.cpp`
- `src/server/ScreenLockService.cpp`
- `src/server/ScreenLockWindow.cpp`
- `src/server/ServerMain.cpp`
