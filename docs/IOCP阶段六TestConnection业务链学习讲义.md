# IOCP 阶段六：`TestConnection` 最短业务链学习讲义

> 前置知识：阶段五已经完成异步接入、accepted socket 初始化、IOCP 关联、`ConnectionContext` 创建和第一次 `postReceive()`。
> 贯穿项目：`D:\CodeRepository\claude\remote_control`。
> 学习范围：只跟踪一个空 payload 的 `TestConnection` 请求，从客户端创建连接到服务端发送完成并以 `RequestComplete` 正常关闭。状态机并发、有序发送队列、背压和安全停机分别在后续阶段展开。

## 1. 阶段六学习主线

阶段五结束在：

```text
AcceptEx completion
  → accepted socket 更新 context
  → accepted socket 关联 completion port
  → 创建 ConnectionContext
  → postReceive
```

阶段六继续解决：

> receive completion 带回的原始字节，怎样变成一个 `TestConnection` 请求；服务端又怎样生成响应，并确保响应发送完成后再关闭连接？

前五阶段的结论在这里直接使用：

| 已掌握结论 | 阶段六中的用途 |
| --- | --- |
| receive operation 只处理 `[0, transferredBytes)` | 只把本次真正收到的字节追加到连接级 buffer |
| receive 零字节表示对端正常关闭 | 在解析 Packet 前先结束当前连接 |
| completion worker 恢复 `IoOperation` 所有权 | 从 operation 取得 connection 和本次 receive buffer |
| `ConnectionContext` 活过单次 operation | 半个 Packet 可以跨多次 receive 保存 |
| send operation 拥有完整发送字节和 `sendOffset` | 响应序列化后可以安全处理部分发送 |
| accepted socket 已关联 IOCP | response 的 `WSASend` completion 会回到同一组 worker |

常用术语：

| 术语 | 含义 |
| --- | --- |
| request | 客户端发给服务端的一次协议请求。 |
| response | 服务端针对 request 返回的协议结果。 |
| Packet | 项目定义的一帧协议数据，由 header、length、command、payload 和 checksum 组成。 |
| wire bytes | Packet 序列化后真正写入 TCP 字节流的 bytes。 |
| receive chunk | 某一次 `WSARecv()` completion 实际带回的连续字节范围。 |
| connection receive buffer | `ConnectionContext::receiveBuffer`，保存尚未解析完的跨 operation 字节。 |
| frame | 一个完整 Packet 序列化后的连续 wire bytes；本讲义中的“消费一个 frame”就是从 buffer 中移除一个完整 Packet。 |
| in-flight send | 已提交但尚未完成全部 bytes 的 send operation，此时 operation 的发送 buffer 仍必须有效。 |
| 半包 | 一次 receive 只得到一个 Packet 的前一部分，剩余部分以后到达。 |
| 粘包 | 一次 receive 同时得到一个以上连续 Packet 的字节。 |
| parse | 根据协议布局把 wire bytes 还原为 `Packet`。 |
| `OneShot` | 当前连接只处理一个短请求，发送最终响应后关闭。 |
| final response | 当前连接的最后一个响应；它必须发送完成后才能正常关闭 socket。 |
| `RequestComplete` | 项目表示“一次性请求正常完成”的关闭原因，不是错误。 |

完整主线可以分成三段：

```text
客户端发送：
RemoteClient::testConnection
  → OneShotRequest::start
  → QTcpSocket::connectToHost
  → connected
  → Packet(TestConnection, empty payload).serialize
  → QTcpSocket::write

服务端处理：
AcceptEx completion
  → postReceive
  → WSARecv completion
  → handleReceiveCompletion
  → 把实际字节追加到 ConnectionContext::receiveBuffer
  → processReceivedPackets
  → Packet::tryParse
  → handleInitialPacket
  → AwaitingRequest 变为 OneShot
  → 创建空 payload 的 TestConnection response
  → sendFinalPacket
  → enqueuePacket / enqueueBytes
  → postSend
  → WSASend completion
  → handleSendCompletion
  → closeConnection(RequestComplete)

客户端确认：
response bytes 到达 Qt socket
  → readyRead
  → readAll
  → Packet::tryParse
  → 确认 response command
  → 报告连接测试成功
```

三段之间不是一个线程中的连续函数调用。尤其不能根据日志假设“服务端 send completion、服务端关闭、客户端 `readyRead`”具有固定的全局先后；只能依赖每一端自己的控制条件。

建议分五个学习单元推进：

1. **认识最短请求（第 4～8 节）**
   - 解决的问题：客户端怎样生成一个 10-byte `TestConnection` Packet。
   - 学完自检：能闭卷写出请求的十个十六进制 byte。
2. **把字节还原为 Packet（第 9～13 节）**
   - 解决的问题：receive chunk、持久 buffer、半包和 `tryParse()` 如何配合。
   - 学完自检：能推演 6 bytes 与 4 bytes 分两次到达的过程。
3. **从 Packet 生成响应（第 14～15 节）**
   - 解决的问题：首包怎样把连接分类为 `OneShot` 并创建 response。
   - 学完自检：能解释 `handleInitialPacket()` 返回 `false` 为什么不是请求失败。
4. **发送完成后关闭（第 16～18 节）**
   - 解决的问题：单响应路径怎样提交 send，并在真正完成后关闭。
   - 学完自检：能说明 response bytes 由谁持有，以及为什么不能在 `postSend()` 后立即关闭。
5. **映射项目并综合验收（第 19～21 节）**
   - 解决的问题：对象、线程、源码和失败入口如何连成一条业务链。
   - 学完自检：能不看代码复述客户端与服务端完整闭环。

---

## 2. 知识范围

### 2.1 核心内容

- `Command::TestConnection` 的请求和响应语义。
- `Packet` 的 header、length、command、payload 和 checksum。
- little-endian wire format。
- `Packet::serialize()`。
- `Packet::tryParse()`。
- TCP 字节流中的半包和粘包。
- operation 级 `storage` 与 connection 级 `receiveBuffer` 的区别。
- `handleReceiveCompletion()` 追加实际完成字节。
- `processReceivedPackets()` 的循环和返回值语义。
- 首包把连接从 `AwaitingRequest` 分类为 `OneShot`。
- `handleInitialPacket()` 创建空 payload response。
- `sendFinalPacket()` 的简单单响应路径。
- `closeAfterSend` 为什么不能替换为立即 `closesocket()`。
- `handleSendCompletion()` 以 `RequestComplete` 正常关闭。
- 客户端 `QTcpSocket` 的连接、写入、读取和响应确认边界。

### 2.2 后续内容

| 主题 | 后续阶段 |
| --- | --- |
| 完整 `ConnectionStateMachine`、CAS 和非法迁移 | 阶段七 |
| 多个响应的有序发送、`sendQueue`、发送竞态和背压 | 阶段七 |
| 文件、截图、shell 等阻塞任务池 | 阶段八 |
| `CancelIoEx()`、pending I/O drain、并发关闭和安全停机 | 阶段九 |

本阶段观察发送状态时只采用一个 response、没有已有 send、容量足够的路径。这样可以看懂 `TestConnection` 的业务闭环，而不需要先掌握阶段七的通用队列算法。

---

## 3. 学习完成标准

完成本阶段后，应能够：

1. 解释 `TestConnection` 请求和响应为什么都使用空 payload。
2. 根据协议布局计算空 payload Packet 的总大小为 10 bytes。
3. 写出 `TestConnection` request 的完整十六进制 bytes。
4. 解释 TCP 为什么可能产生半包和粘包。
5. 区分 receive operation 的 `storage` 与 connection 的 `receiveBuffer`。
6. 说明 `QByteArray::append()` 两个参数在 receive completion 中的作用。
7. 解释 `Packet::tryParse()` 为什么以非 const 引用接收 buffer。
8. 推演完整 Packet、半包、连续两个 Packet 和非法前缀下的 buffer 变化。
9. 解释 `processReceivedPackets()` 返回 `true` 和 `false` 对下一次 receive 的影响。
10. 解释首个 `TestConnection` Packet 为什么把连接分类为 `OneShot`。
11. 解释 `handleInitialPacket()` 成功安排最终响应后为什么仍返回 `false`。
12. 说明 response 从 `Packet` 到 `QByteArray` 再到 send operation 的所有权变化。
13. 推演简单路径中 `queuedSendBytes` 的 `0 → 10 → 0` 和 `sendPending` 的 `false → true → false`。
14. 解释为什么必须等最终 `WSASend` completion 后再关闭连接，并区分服务端发送完成与客户端解析成功。
15. 说明 final response 发生部分发送时，为什么必须复用阶段四的结论继续发送剩余 bytes。
16. 区分 receive worker、send worker 和客户端 Qt event loop 的线程边界。
17. 按源码位置连续跟踪客户端和服务端完整链路。
18. 区分 `RequestComplete` 正常关闭与 I/O、协议、容量类失败关闭。

建议投入 6～10 小时。

---

## 4. `TestConnection` 是什么

项目把连接测试定义为一个协议命令：

```cpp
enum class Command : quint16
{
    TestConnection = 1981,
};
```

这条命令的最小语义分为两个方向：

1. **客户端 → 服务端**
   - command：`TestConnection`。
   - payload：空。
   - 结果：请求服务端证明协议链可用。
2. **服务端 → 客户端**
   - command：`TestConnection`。
   - payload：空。
   - 结果：表示请求已经被识别并正常响应。

它验证的不只是 TCP 三次握手：

```text
客户端能够序列化并发送协议 Packet
  + 服务端能够接收、缓存和解析 Packet
  + 服务端能够路由 command 并生成 response
  + response 能够完成 WSASend
  + 客户端能够解析并确认 response command
```

### 4.1 一条连接只完成一次短请求

```text
AwaitingRequest
  → 收到首个 TestConnection Packet
  → OneShot
  → 发送最终 response
  → Closing
  → Closed
```

本阶段只观察这四个状态名称和先后关系。状态转换的并发实现放在阶段七。

### 4.2 当前路径的简化条件

为了先看清业务主线，当前连接满足：

- 客户端只发送一个合法 `TestConnection` request。
- request payload 为空。
- 服务端发送一个空 payload response。
- response 入队前没有其他 send。
- 发送容量足够。
- 没有与停机、超时或其他关闭线程竞争。

这些条件与项目正常的单次连接测试完全对应。

---

## 5. 一次请求涉及哪些对象和线程

### 5.1 客户端对象

```text
RemoteClient
  └─ 创建 OneShotRequest
       ├─ 拥有 QTcpSocket
       ├─ 保存 command = TestConnection
       ├─ 保存 empty payload
       └─ 保存尚未解析的 response buffer
```

`OneShotRequest` 通过 Qt event loop 接收 `connected`、`readyRead`、`disconnected` 和错误通知。它不会阻塞等待网络结果。

### 5.2 服务端对象

```text
ConnectionContext
  ├─ 保存 connected socket
  ├─ 保存 ConnectionPhase
  ├─ 拥有跨 receive 的 receiveBuffer
  └─ 保存最小发送状态

receive IoOperation
  ├─ 拥有本次 8192-byte storage
  └─ 持有 ConnectionContext 强引用

send IoOperation
  ├─ 拥有 response 的完整 wire bytes
  ├─ 保存 sendOffset
  └─ 持有同一 ConnectionContext 强引用
```

### 5.3 线程切换

```text
客户端 Qt event loop
  → 发起 connect
  → connected callback 写 request

服务端 completion worker A
  → 处理 receive completion
  → 解析 request
  → 创建 response 并提交 send

服务端 completion worker B
  → 处理 send completion
  → 正常关闭 connection

客户端 Qt event loop
  → readyRead callback 解析 response
  → 报告连接测试成功
```

worker A 和 worker B 可以是同一线程，也可以是不同线程。阶段四已经说明：connection 不固定绑定某个 completion worker。

`TestConnection` 处理很轻量，服务端直接在 receive completion worker 中完成命令判断和 response 创建，不进入阻塞任务池。

---

## 6. 客户端创建并发送请求

客户端入口：

```cpp
void RemoteClient::testConnection()
{
    auto* const request{new OneShotRequest{
        this,
        this->m_host,
        this->m_port,
        this->m_endpointGeneration,
        remote_control::Command::TestConnection,
        {},
        tr("Connection test")}};

    request->start();
}
```

`testConnection()` 没有参数。它使用 `RemoteClient` 已保存的 host、port 和 endpoint generation。

`OneShotRequest` 构造函数的七个参数：

| 参数 | 当前实参 | 作用 |
| --- | --- | --- |
| `_client` | `this` | 接收最终成功或失败结果，并作为 QObject parent 管理 request。 |
| `_host` | `this->m_host` | 目标服务端 host name 或 IP 字符串。 |
| `_port` | `this->m_port` | 目标 TCP 端口。 |
| `_generation` | `this->m_endpointGeneration` | 标识请求创建时使用的是哪一代 endpoint。 |
| `_command` | `Command::TestConnection` | 指定当前短请求命令。 |
| `_payload` | `{}` | 空 `QByteArray`，符合连接测试协议。 |
| `_context` | `"Connection test"` 的本地化文本 | 保存请求用途标签；不进入 wire Packet。 |

### 6.1 `start()` 发起异步连接

```cpp
void OneShotRequest::start()
{
    this->m_timeoutTimer->start();
    this->m_socket.connectToHost(this->m_host, this->m_port);
}
```

`start()` 没有参数。它启动当前 request 的超时计时，并让内部 `QTcpSocket` 发起连接。

项目只传入前两个实参。下面是 Qt 6 文档中的原型：

```cpp
void connectToHost(
    QString const& hostName,
    quint16 port,
    QIODeviceBase::OpenMode openMode = QIODeviceBase::ReadWrite,
    QAbstractSocket::NetworkLayerProtocol protocol = QAbstractSocket::AnyIPProtocol);
```

Qt 5 中 open mode 的类型名写作 `QIODevice::OpenMode`；本项目同时支持 Qt 5 和 Qt 6，但当前调用的四个参数含义以及两个默认值相同。

四个参数的作用：

| 参数 | 当前值 | 作用 |
| --- | --- | --- |
| `hostName` | `this->m_host` | 要解析并连接的主机名或地址。 |
| `port` | `this->m_port` | 服务端监听端口。 |
| `openMode` | 使用默认 `ReadWrite` | 连接成功后既允许读，也允许写。 |
| `protocol` | 使用默认 `AnyIPProtocol` | 允许 Qt 根据 host 选择 IPv4 或 IPv6。 |

调用立即返回。连接建立后，Qt 发出 `connected` signal，再进入 `onConnected()`。

### 6.2 连接成功后写入 Packet

```cpp
void OneShotRequest::onConnected()
{
    remote_control::Packet const packet{this->m_command, this->m_payload};
    this->m_socket.write(packet.serialize());
}
```

`Packet` 构造参数：

| 参数 | 当前值 | 作用 |
| --- | --- | --- |
| `_command` | `Command::TestConnection` | 写入 Packet 的 command 字段。 |
| `_payload` | 空 `QByteArray` | 写入 Packet 的 payload 字段。 |

`serialize()` 没有参数，返回完整 wire bytes。

这里调用的是 `QIODevice::write(QByteArray const& data)`：

| 参数 | 当前值 | 作用 |
| --- | --- | --- |
| `data` | `packet.serialize()` 的返回值 | 把完整 request bytes 交给 `QTcpSocket` 的写缓冲区。 |

`write()` 返回接受写入的 byte 数，失败时返回负值。项目的完整 request 对象还通过错误 signal 和超时处理失败；本阶段沿成功路径继续。

---

## 7. `Packet` 数据模型

与阶段六有关的接口：

```cpp
class Packet
{
public:
    static constexpr quint16 Header{0xFEFF};

    Packet(Command _command, QByteArray _payload = {});

    [[nodiscard]] quint16 checksum() const noexcept;
    [[nodiscard]] QByteArray serialize() const;
    [[nodiscard]] static std::optional<Packet> tryParse(QByteArray& _buffer);

    Command command{Command::TestConnection};
    QByteArray payload;
};
```

### 7.1 构造函数参数

| 参数 | 作用 |
| --- | --- |
| `_command` | 当前 Packet 表示哪个请求或响应。 |
| `_payload` | command 对应的原始 payload bytes；默认值为空。 |

`TestConnection` request 和 response 都使用：

```cpp
remote_control::Packet{remote_control::Command::TestConnection}
```

第二个参数省略后使用空 payload。

### 7.2 `checksum()`

`checksum()` 没有参数。它把 payload 中每个 byte 的无符号值累加到 16-bit 结果中。

```text
TestConnection payload 大小 = 0
  → 没有 byte 参与累加
  → checksum = 0
```

checksum 用于协议帧校验，不提供身份认证或密码学完整性。

### 7.3 `serialize()`

`serialize()` 没有参数。它读取当前对象的 `command` 和 `payload`，返回 little-endian wire bytes；payload 超过协议上限时返回空数组。

序列化得到的 `QByteArray` 拥有自己的 bytes。随后客户端 `QTcpSocket` 或服务端 send operation 使用这些 bytes。

---

## 8. Packet wire format 与十个 request bytes

项目协议布局按 wire offset 依次为：

1. **偏移 0：Header**
   - 大小：2 bytes。
   - `TestConnection` 当前值：`0xFEFF`。
2. **偏移 2：Length**
   - 大小：4 bytes。
   - `TestConnection` 当前值：`4`。
3. **偏移 6：Command**
   - 大小：2 bytes。
   - `TestConnection` 当前值：`1981`。
4. **偏移 8：Payload**
   - 大小：0 bytes。
   - `TestConnection` 当前值：空。
5. **偏移 8：Checksum**
   - 大小：2 bytes。
   - `TestConnection` 当前值：`0`。

### 8.1 `Length` 表示什么

```text
Length
  = Command 大小
  + Payload 大小
  + Checksum 大小

  = 2 + N + 2
  = N + 4
```

空 payload 时：

```text
Length = 2 + 0 + 2 = 4
```

整个 Packet 的大小：

```text
Header 2
  + Length field 4
  + Length 4
  = 10 bytes
```

### 8.2 little-endian

多 byte 整数的低位 byte 先进入 wire：

```text
Header  0xFEFF  → FF FE
Length  4       → 04 00 00 00
Command 1981    → BD 07
Checksum 0      → 00 00
```

所以完整 request 是：

```text
offset   00 01 02 03 04 05 06 07 08 09
field     H  H  L  L  L  L  C  C  S  S
bytes    FF FE 04 00 00 00 BD 07 00 00
```

其中：

- `H`：Header。
- `L`：Length。
- `C`：Command。
- `S`：Checksum。

服务端 response 使用相同 command 和空 payload，因此 response 的十个 bytes 与 request 相同。

### 8.3 Packet 边界不等于 TCP 读取边界

客户端一次 `write(10 bytes)`，不保证服务端某一次 receive 恰好得到 10 bytes：

```text
一次 write 10 bytes
  ├─ 可能一次 receive 得到 10
  ├─ 可能两次 receive 得到 6 + 4
  └─ 也可能与其他 write 合并后一次得到更多 bytes
```

TCP 提供可靠、有序的 byte stream，但不保存应用的 Packet 边界。

---

## 9. 为什么需要 connection 级 `receiveBuffer`

阶段三到阶段五使用的 receive operation：

```text
receive operation A
  └─ storage A：只保存本次 WSARecv 的 bytes

receive operation B
  └─ storage B：保存下一次 WSARecv 的 bytes
```

如果 request 被拆成 `6 + 4` bytes：

```text
operation A 完成：6 bytes
  → operation A 即将销毁

operation B 完成：4 bytes
  → 需要与前 6 bytes 合并
```

所以 `ConnectionContext` 拥有持久 buffer：

```cpp
QByteArray receiveBuffer;
```

数据流：

```text
operation A storage 的实际 6 bytes
  → append 到 connection.receiveBuffer
  → tryParse：不完整，保留 6 bytes

operation B storage 的实际 4 bytes
  → append 到同一个 receiveBuffer
  → tryParse：现在有完整 10 bytes
```

对象职责：

| 对象 | 生命周期 | 保存什么 |
| --- | --- | --- |
| `IoOperation::storage` | 一次 receive operation | Windows 本次写入的原始 bytes |
| `ConnectionContext::receiveBuffer` | 整条连接 | 尚未组成完整 Packet 的累计 bytes |
| `Packet::payload` | 一个解析完成的 Packet | 已从 wire frame 中提取的 payload |

不能把 operation 的 `storage` 直接当作“一定完整的一条消息”。

---

## 10. `handleReceiveCompletion()` 追加实际字节

项目核心代码：

```cpp
void RemoteControlTransport::Impl::handleReceiveCompletion(
    std::unique_ptr<IoOperation> _operation,
    bool _success,
    DWORD _transferredBytes)
{
    std::shared_ptr<ConnectionContext> const connection{_operation->connection};

    if (!_success || _transferredBytes == 0 || connection->state.isTerminal())
    {
        this->closeConnection(connection,
                              _success && _transferredBytes == 0
                                  ? ConnectionCloseReason::PeerDisconnected
                                  : ConnectionCloseReason::IoFailure);
        return;
    }

    connection->receiveBuffer.append(
        _operation->storage.constData(),
        static_cast<int>(_transferredBytes));

    if (!this->processReceivedPackets(connection))
    {
        return;
    }

    static_cast<void>(this->postReceive(connection));
}
```

### 10.1 handler 的三个参数

| 参数 | 作用 |
| --- | --- |
| `_operation` | 已由 completion worker 恢复唯一所有权的 receive operation，包含本次 storage 和 connection 强引用。 |
| `_success` | `GetQueuedCompletionStatus()` 是否报告 receive 成功。 |
| `_transferredBytes` | 本次 receive 真正完成的 byte 数，不是 storage 容量。 |

### 10.2 `QByteArray::append()` 的两个参数

当前使用的重载可以理解为：

```cpp
QByteArray& append(char const* data, qsizetype size);
```

| 参数 | 当前值 | 作用 |
| --- | --- | --- |
| `data` | `_operation->storage.constData()` | 本次 operation buffer 的首地址。 |
| `size` | `_transferredBytes` 转成项目兼容的整数类型 | 只复制本次实际完成范围。 |

如果 storage 容量是 8192，但 completion 只有 10 bytes，只能追加前 10 bytes。追加完成后，`receiveBuffer` 拥有自己的副本，当前 receive operation 才可以销毁。

### 10.3 `processReceivedPackets()` 的返回值控制下一次 receive

```text
返回 true
  → 当前连接仍需要更多 request bytes
  → postReceive(connection)

返回 false
  → 当前处理链已经结束或进入最终响应/关闭路径
  → 不再提交下一次 receive
```

对半包而言，返回 `true`；对已经识别并安排最终响应的 `TestConnection`，返回 `false`。

---

## 11. `Packet::tryParse()` 的契约

函数声明：

```cpp
[[nodiscard]] static std::optional<Packet> tryParse(QByteArray& _buffer);
```

唯一参数：

| 参数 | 输入 | 输出变化 |
| --- | --- | --- |
| `_buffer` | 当前连接尚未解析的累计 bytes | 丢弃无效前缀；成功时移除一个完整 Packet；半包时保留仍需等待的 bytes |

返回值：

| 返回值 | 含义 |
| --- | --- |
| 包含 `Packet` | 已找到并校验一个完整 Packet，同时已从 `_buffer` 移除它的 bytes。 |
| `std::nullopt` | 当前没有可返回的完整有效 Packet；可能是半包，也可能是在重新同步后仍需更多 bytes。 |

### 11.1 为什么参数不是 `QByteArray const&`

parser 不只读取 buffer，还会消费 bytes：

```text
解析前：request A bytes + request B bytes
  → 返回 Packet A
解析后：buffer 中只剩 request B bytes
```

因此 `_buffer` 必须是可修改引用。调用者不需要另行维护“已经消费多少 bytes”的 offset。

### 11.2 parser 的处理顺序

```text
1. 寻找 FF FE header
2. 丢弃 header 前的无效前缀
3. 不足 10 bytes：保留并返回 nullopt
4. 读取并校验 Length
5. 完整 frame 尚未到齐：保留并返回 nullopt
6. 读取 Command、Payload、Checksum
7. checksum 不匹配：跳过当前 header 并重新寻找
8. 校验成功：移除一个完整 frame 并返回 Packet
```

这说明 `tryParse()` 同时承担：

- 查找 Packet 起点。
- 判断 frame 是否完整。
- 校验声明长度。
- 校验 payload checksum。
- 从累计 buffer 中消费一个 Packet。

### 11.3 一次调用只返回一个 Packet

即使 buffer 中已经有两个完整 Packet，一次 `tryParse()` 也只返回第一个。调用方通过循环继续解析剩余 bytes。

---

## 12. 半包、粘包与重新同步

### 12.1 半包：`6 + 4`

第一次 completion：

```text
receiveBuffer = FF FE 04 00 00 00
size = 6 < 10
tryParse → nullopt
processReceivedPackets → true
postReceive
```

第二次 completion：

```text
append BD 07 00 00
receiveBuffer = FF FE 04 00 00 00 BD 07 00 00
tryParse → Packet(TestConnection)
receiveBuffer = empty
```

半包不是协议错误，只表示需要下一次 receive。

### 12.2 粘包：buffer 中有两个完整 Packet

```text
receiveBuffer = Packet A 的 10 bytes + Packet B 的 10 bytes

第一次 tryParse
  → 返回 Packet A
  → buffer 剩 Packet B

第二次 tryParse
  → 返回 Packet B
  → buffer 为空
```

通用协议处理循环能够连续取出多个 Packet。但 `TestConnection` 属于 `OneShot`：第一个 Packet 安排最终 response 后，handler 要求停止继续 receive，因此同一连接不会把第二个 request 当作另一个一次性业务继续处理。

### 12.3 header 自身也可能被拆开

假设一次 receive 最后只到达 `FF`：

```text
... noise FF
```

parser 会丢弃前面的 noise，但保留末尾 `FF`。下一次 receive 如果以 `FE` 开头，就可以恢复完整 header `FF FE`。

### 12.4 非法长度或 checksum

parser 不信任 wire 中的 length：

- length 小于 command 与 checksum 的最小总和 `4`，当前 header 无效。
- length 超过协议最大值，当前 header 无效。
- checksum 不匹配，当前 frame 无效。

这些情况下 parser 跳过当前 header 并继续寻找下一个 `FF FE`，而不是按错误 length 访问越界内存。

### 12.5 两层大小限制

| 限制 | 当前值 | 作用 |
| --- | --- | --- |
| `Packet::MaximumPayloadSize` | 64 MiB | 通用 Packet payload 的协议上限 |
| `MaximumIncomingBufferBytes` | 1 MiB | 服务端单连接尚未处理请求 bytes 的更严格上限 |

`TestConnection` request 只有 10 bytes。服务端请求方向不需要允许截图或下载响应那样的大 payload，因此使用更小的连接级输入边界。

---

## 13. `processReceivedPackets()` 怎样驱动解析

沿着 `TestConnection` 分支，可以把核心代码缩成：

```cpp
bool RemoteControlTransport::Impl::processReceivedPackets(
    std::shared_ptr<ConnectionContext> const& _connection)
{
    if (_connection->receiveBuffer.size() > MaximumIncomingBufferBytes)
    {
        this->closeConnection(_connection, ConnectionCloseReason::ProtocolViolation);
        return false;
    }

    while (!_connection->state.isTerminal())
    {
        auto const packet{remote_control::Packet::tryParse(_connection->receiveBuffer)};
        if (!packet.has_value())
        {
            break;
        }

        if (!this->handleInitialPacket(_connection, packet.value()))
        {
            return false;
        }
    }

    return _connection->state.phase() == ConnectionPhase::AwaitingRequest;
}
```

项目完整函数还会根据已经分类的 phase 路由持久连接 Packet；本节只看初始 `TestConnection`。

### 13.1 参数

| 参数 | 作用 |
| --- | --- |
| `_connection` | 提供累计 `receiveBuffer`、当前 phase 和关闭入口；函数执行期间由共享引用保持存活。 |

### 13.2 返回值不是简单的“业务成功或失败”

| 场景 | 返回值 | `handleReceiveCompletion()` 的下一步 |
| --- | --- | --- |
| 当前只有半包，phase 仍是 `AwaitingRequest` | `true` | 再提交一次 receive |
| 完整 `TestConnection` 已安排 final response | `false` | 不再 receive，等待 send completion |
| buffer 超限或协议分支关闭连接 | `false` | 不再 receive |

因此：

> `false` 的准确含义是“当前 receive 链不要继续”，不一定表示 `TestConnection` 业务失败。

### 13.3 `std::optional` 的使用

```cpp
if (!packet.has_value())
{
    break;
}

this->handleInitialPacket(_connection, packet.value());
```

- `has_value()` 没有参数，表示 optional 当前是否包含 `Packet`。
- `value()` 没有参数，取得已存在的 `Packet` 引用。
- 只有确认 `has_value()` 为 `true` 后才调用 `value()`。

---

## 14. 首个 Packet 把连接分类为 `OneShot`

连接刚建立时：

```text
connection.state = AwaitingRequest
```

第一个完整 Packet 决定当前连接的业务类别。`TestConnection` 使用：

```cpp
this->m_connectionRegistry.tryClassify(
    _connection,
    ConnectionPhase::OneShot);
```

`tryClassify()` 的两个参数：

| 参数 | 当前值 | 作用 |
| --- | --- | --- |
| `_connection` | 当前连接上下文 | 指定要分类的已注册连接。 |
| `_phase` | `ConnectionPhase::OneShot` | 指定首包选择的一次性请求类别。 |

返回值：

- `true`：连接仍处于 `AwaitingRequest`，成功变为 `OneShot`。
- `false`：连接不存在、已经分类或不能完成当前转换；调用方关闭连接。

本阶段只使用一条转换：

```text
AwaitingRequest → OneShot
```

不需要先理解 `compare_exchange`、角色配额和并发关闭实现；这些是阶段七的主题。

### 14.1 为什么只允许首包分类一次

连接一旦是 `OneShot`，它的生命周期目标已经确定：

```text
解析一个短请求
  → 发送最终 response
  → 关闭
```

不能在同一条连接上再把它重新分类为文件传输或屏幕流。

---

## 15. 创建 `TestConnection` response

沿着当前 command，`handleInitialPacket()` 的关键逻辑是：

```cpp
bool RemoteControlTransport::Impl::handleInitialPacket(
    std::shared_ptr<ConnectionContext> const& _connection,
    remote_control::Packet const& _packet)
{
    if (!this->m_connectionRegistry.tryClassify(
            _connection, ConnectionPhase::OneShot))
    {
        this->closeConnection(_connection, ConnectionCloseReason::ProtocolViolation);
        return false;
    }

    if (_packet.command != remote_control::Command::TestConnection)
    {
        this->closeConnection(_connection, ConnectionCloseReason::ProtocolViolation);
        return false;
    }

    remote_control::Packet const response{
        remote_control::Command::TestConnection};
    this->sendFinalPacket(_connection, response);
    return false;
}
```

实际项目同一分支还支持 `ListDrives`；上面只保留 `TestConnection` 路径。

### 15.1 两个参数

| 参数 | 作用 |
| --- | --- |
| `_connection` | 当前尚处于 `AwaitingRequest` 的连接上下文。 |
| `_packet` | 已通过 header、length 和 checksum 校验的首个 Packet。 |

### 15.2 response 的内容

```cpp
remote_control::Packet{
    remote_control::Command::TestConnection,
    {}}
```

第二个参数使用默认空 payload，因此 response 仍是 10 bytes。

当前服务端根据 command 生成空 response；客户端按协议也发送空 payload。项目这条分支不把 request payload 原样回显。

需要注意：当前服务端没有为 `TestConnection` 单独检查 request payload 是否为空。正常客户端按约定发送空 payload，服务端不读取它，并始终创建空 payload response。因此这条命令不是 echo，当前业务也不能依赖 request payload 被返回。

### 15.3 最后的 `return false` 为什么不是失败

response 已经交给 `sendFinalPacket()` 后，当前连接不应该继续提交 receive：

```text
handleInitialPacket 返回 false
  → processReceivedPackets 返回 false
  → handleReceiveCompletion 返回
  → 不再 postReceive
  → 只等待 final response 的 send completion
```

这里的 `false` 表示“停止 receive 主链”，正常完成动作已经转移到 send 主链。

---

## 16. response 从 Packet 进入 send operation

### 16.1 `sendFinalPacket()`

```cpp
void RemoteControlTransport::Impl::sendFinalPacket(
    std::shared_ptr<ConnectionContext> const& _connection,
    remote_control::Packet const& _packet)
{
    if (this->enqueuePacket(_connection, _packet))
    {
        this->requestCloseAfterSend(_connection);
    }
    else
    {
        this->closeConnection(_connection, ConnectionCloseReason::Backpressure);
    }
}
```

两个参数：

| 参数 | 作用 |
| --- | --- |
| `_connection` | response 所属连接，也是 send 状态和 socket 的保存位置。 |
| `_packet` | 要作为当前连接最终响应发送的 Packet。 |

函数没有返回值。它要么安排 response 并请求发送后关闭，要么进入失败关闭路径。

### 16.2 `enqueuePacket()`

```cpp
bool RemoteControlTransport::Impl::enqueuePacket(
    std::shared_ptr<ConnectionContext> const& _connection,
    remote_control::Packet const& _packet)
{
    QByteArray const bytes{_packet.serialize()};
    return !bytes.isEmpty() && this->enqueueBytes(_connection, bytes);
}
```

两个参数与 `sendFinalPacket()` 相同。它把 Packet 序列化为 wire bytes，再交给 byte 级发送入口。

返回值：

- `true`：完整 bytes 已进入发送路径。
- `false`：序列化或 byte 入队失败。

### 16.3 `enqueueBytes()` 的简单路径

本阶段初始发送状态：

```text
queuedSendBytes = 0
sendPending = false
closeAfterSend = false
```

response 是第一个 send，因此核心变化是：

```text
接收 10-byte response
  → queuedSendBytes = 10
  → sendPending = true
  → 创建 send IoOperation
  → postSend
```

`enqueueBytes()` 的两个参数：

| 参数 | 当前值 | 作用 |
| --- | --- | --- |
| `_connection` | 当前 `OneShot` 连接 | 读取并更新该连接的发送状态。 |
| `_bytes` | 10-byte serialized response | 要发送的完整 wire bytes。 |

`postSend()` 的一个参数是新建的 `std::unique_ptr<IoOperation>`。operation 接管 response bytes，并持有 `_connection` 强引用。之后即使局部 `QByteArray` 销毁，Windows 使用的 send buffer 仍然有效。

`postSend()` 返回 `bool`：

- `true`：send operation 已成功进入 overlapped I/O 路径；这仍不表示所有 bytes 已经完成。
- `false`：operation 没有继续进入发送路径，调用链随后进入关闭处理。

实际源码还包含 `sendQueue`、`sendMutex` 和容量判断。当前只有一个 response，因此 `sendQueue` 始终为空，不参与本阶段的状态推演；多个 response 怎样排队、锁怎样保护状态以及容量怎样限制，统一留到阶段七。

---

## 17. 为什么使用 `closeAfterSend`

错误做法：

```text
postSend
  → 立即 closesocket
```

`postSend()` 只表示 send operation 已提交成功，不表示全部 response bytes 已经完成发送。立即关闭会让在途 send 失败或让对端收不到完整响应。

最终 `WSASend` completion 表示服务端可以结束当前 send operation，并不等于客户端业务代码已经读取和解析 response。客户端是否成功，仍由后面的 `readyRead → tryParse → command 检查` 决定。

项目使用：

```cpp
void RemoteControlTransport::Impl::requestCloseAfterSend(
    std::shared_ptr<ConnectionContext> const& _connection)
{
    bool closeNow{false};
    {
        std::lock_guard<std::mutex> const lock{_connection->sendMutex};
        _connection->closeAfterSend = true;
        closeNow = !_connection->sendPending && _connection->sendQueue.empty();
    }

    if (closeNow)
    {
        this->closeConnection(
            _connection, ConnectionCloseReason::RequestComplete);
    }
}
```

唯一参数：

| 参数 | 作用 |
| --- | --- |
| `_connection` | 记录“最终 response 完成后关闭”状态的连接。 |

### 17.1 在单 response 路径中代入条件

源码中的 `closeNow` 同时检查 `sendPending` 和 `sendQueue`。当前路径的 queue 已知为空，因此本阶段只需理解为：

```text
send 仍在途
  → sendPending = true
  → 只记录 closeAfterSend = true
  → 暂不关闭

final send completion
  → sendPending = false
  → 发现 closeAfterSend = true
  → closeConnection(RequestComplete)
```

这条顺序足以解释 `TestConnection` 的正常关闭。为什么源码还要同时检查 queue、使用 mutex，并处理“completion 抢先到达”的情况，属于阶段七的发送并发问题。

---

## 18. send completion 与正常关闭

阶段四已经讲过部分发送。本阶段只把结论接入最终响应：

```text
sendOffset += transferredBytes
  ├─ sendOffset < sendBytes.size()
  │    → postSend 剩余 bytes
  └─ sendOffset == sendBytes.size()
       → 当前 response 完成
```

`handleSendCompletion()` 的三个参数：

| 参数 | 作用 |
| --- | --- |
| `_operation` | 拥有完整 response bytes、`sendOffset` 和 connection 引用。 |
| `_success` | 当前 send operation 是否成功完成。 |
| `_transferredBytes` | 当前这一次 send 实际完成的 byte 数。 |

### 18.1 10 bytes 一次完成

```text
完成前：
queuedSendBytes = 10
sendPending = true
closeAfterSend = true

WSASend completion：transferredBytes = 10
  → sendOffset = 10
  → response 全部完成
  → queuedSendBytes = 0
  → sendPending = false
  → closeAfterSend 为 true
  → closeConnection(RequestComplete)
```

### 18.2 阶段四部分发送结论的直接复用

```text
本次 completion 只完成部分 bytes
  → 更新 sendOffset
  → 使用同一个 operation 重新 post 剩余范围
  → 不把 sendPending 改为 false
  → 不触发 RequestComplete
```

`sendOffset` 的计算、`WSABUF` 刷新和重新提交所有权已经在阶段四完成。本阶段只使用结论：final response 没有全部完成，就不能进入正常关闭。

### 18.3 `closeConnection()` 的两个参数

```cpp
this->closeConnection(
    connection,
    ConnectionCloseReason::RequestComplete);
```

| 参数 | 作用 |
| --- | --- |
| `_connection` | 要结束的当前 `OneShot` 连接。 |
| `_reason` | `RequestComplete`，表示 final response 已发送完成。 |

本阶段只观察结果：连接从 `OneShot` 进入 `Closing`，socket 被关闭，最后进入 `Closed`。关闭竞争、取消在途 I/O 和 drain 顺序留到阶段九。

### 18.4 客户端确认 response

客户端收到可读通知后：

```cpp
this->m_buffer.append(this->m_socket.readAll());
auto const packet{remote_control::Packet::tryParse(this->m_buffer)};
```

- `readAll()` 没有参数，返回当前 Qt socket read buffer 中所有可用 bytes。
- 客户端 `append(QByteArray const& bytes)` 只有一个参数，把新到 bytes 追加到持久 `m_buffer`。
- `tryParse()` 与服务端使用同一协议实现，因此同样能处理半包。

确认：

```text
response.command == request.command == TestConnection
  → 报告 connectionTested(true, ...)
  → disconnectFromHost()
```

`handlePacket(remote_control::Packet const& _packet)` 的唯一参数 `_packet`，表示客户端已经完成 frame 校验的 response Packet。函数先把 `_packet.command` 与当前 request command 比较，再进入 `TestConnection` 分支。

`connectionTested(bool _success, QString const& _message)` 的两个参数：

| 参数 | 当前值 | 作用 |
| --- | --- | --- |
| `_success` | `true` | 告诉界面连接测试已经成功。 |
| `_message` | `"Connection succeeded."` 的本地化文本 | 提供可展示给用户的结果说明。 |

`disconnectFromHost()` 没有参数。客户端已经得到有效 response 后，请求对象开始关闭自己的 Qt socket。

服务端 send completion、服务端关闭和客户端 `readyRead` 分属不同线程甚至不同主机，不能依赖它们在日志中的固定先后。这里能够确定的是：服务端只在 final send completion 后关闭；客户端只在解析到完整有效 response 后报告成功。

两个完成点不能混为一件事：

| 完成点 | 判断位置 | 能说明什么 |
| --- | --- | --- |
| 服务端发送完成 | `handleSendCompletion()` 确认全部 response bytes 完成 | send operation 可以结束，连接可以按 `RequestComplete` 关闭 |
| 客户端连接测试成功 | `handlePacket()` 确认 response command | 客户端已经收到并解析了协议响应 |

---

## 19. 映射到项目源码

下面三组入口都相对于同一个源项目根目录：

> 源项目根目录：`D:\CodeRepository\claude\remote_control`

### 19.1 客户端发送与确认

1. `src\client\RemoteClient.cpp:616`
   - 创建 `TestConnection` 的 `OneShotRequest`。
2. `src\client\RemoteClient.cpp:125`
   - `start()` 调用 `connectToHost()`。
3. `src\client\RemoteClient.cpp:133`
   - `onConnected()` 序列化并写入 request。
4. `src\client\RemoteClient.cpp:142`
   - `onReadyRead()` 累计 response bytes 并循环解析。
5. `src\client\RemoteClient.cpp:316`
   - `handlePacket()` 检查 response command 并报告成功。

### 19.2 协议对象

1. `include\common\Protocol.h:16`
   - 查看 `Command::TestConnection = 1981`。
2. `include\common\Packet.h:30`
   - 查看 `Packet` 构造参数。
3. `src\common\Packet.cpp:63`
   - 查看 header、length、command、payload、checksum 的序列化。
4. `src\common\Packet.cpp:98`
   - 查看半包保留、长度校验、checksum 校验和消费一个 Packet。
5. `tests\ProtocolTests.cpp:73`
   - 参考 split header 的现有测试思路。

### 19.3 服务端处理与关闭

1. `server_transport\src\RemoteControlTransport.cpp:566`
   - receive completion 追加实际 bytes。
2. `server_transport\src\RemoteControlTransportProtocol.cpp:32`
   - 循环解析 connection receive buffer。
3. `server_transport\src\RemoteControlTransportProtocol.cpp:86`
   - 执行首包分类和 command 路由。
4. `server_transport\src\RemoteControlTransportProtocol.cpp:189`
   - 创建空 payload 的 `TestConnection` response。
5. `server_transport\src\RemoteControlTransport.cpp:699`
   - `sendFinalPacket()` 安排发送后关闭。
6. `server_transport\src\RemoteControlTransport.cpp:656`
   - Packet 序列化并进入 byte 发送入口。
7. `server_transport\src\RemoteControlTransport.cpp:663`
   - 当前无 send 时创建第一条 send operation。
8. `server_transport\src\RemoteControlTransport.cpp:377`
   - `postSend()` 提交 `WSASend()`。
9. `server_transport\src\RemoteControlTransport.cpp:590`
   - 完成全部 response 后检查 `closeAfterSend`。
10. `server_transport\src\RemoteControlTransport.cpp:727`
    - 以 `RequestComplete` 进入关闭入口。

### 19.4 阅读时追踪四条线

```text
字节线：
request Packet → 10 wire bytes → receive storage → receiveBuffer → parsed Packet
response Packet → 10 wire bytes → send operation → client buffer → parsed Packet

状态线：
AwaitingRequest → OneShot → Closing → Closed

所有权线：
receive operation → ConnectionContext → local Packet → send operation → close

线程线：
客户端 event loop → receive worker → send worker → 客户端 event loop
```

---

## 20. 失败入口与常见错误

### 20.1 当前链路的主要退出点

| 位置 | 条件 | 结果 |
| --- | --- | --- |
| 客户端 connect | host、port 或网络连接失败 | request 报告失败，不发送 Packet |
| 服务端 receive completion | `_success == false` | `IoFailure` 关闭 |
| 服务端 receive completion | `_transferredBytes == 0` | `PeerDisconnected` 关闭 |
| 累计输入 | `receiveBuffer > 1 MiB` | `ProtocolViolation` 关闭 |
| Packet 解析后路由 | 首包不能分类或 command 不支持当前路径 | `ProtocolViolation` 关闭 |
| response 序列化或容量检查 | `enqueuePacket()` / `enqueueBytes()` 返回 `false` | `sendFinalPacket()` 请求以 `Backpressure` 关闭 |
| `WSASend()` 同步提交 | 返回非 pending 的 socket error | `postSend()` 请求以 `IoFailure` 关闭 |
| send completion | `_success == false` | `IoFailure` 关闭 |
| send completion | `_transferredBytes == 0` | `PeerDisconnected` 关闭 |
| 正常 final send | 所有 response bytes 完成且 `closeAfterSend == true` | `RequestComplete` 关闭 |
| 客户端 response | command 与 request 不一致 | request 报告协议失败 |

`RequestComplete` 是正常业务终点；其他原因表示链路在某一步没有完成预期闭环。

### 20.2 常见错误

| 错误 | 直接后果 | 根因 |
| --- | --- | --- |
| 把 `connectToHost()` 返回当成已经连接 | 在 `connected` 前写入控制流混乱 | Qt 连接是异步过程 |
| 把 `TestConnection = 1981` 按 big-endian 写入 | wire command 变成错误值 | 项目协议使用 little-endian |
| 认为 Length 是整个 Packet 大小 | 计算出 4 或 14 等错误偏移 | Length 只包含 command、payload 和 checksum |
| 空 payload 时省略 checksum 字段 | Packet 只有 8 bytes，parser 永远无法完成 | checksum 字段固定占 2 bytes |
| 假设一次 write 对应一次 receive | 半包时把合法 request 当作错误 | TCP 不保留 Packet 边界 |
| 直接从本次 operation storage 解析并丢弃半包 | 下一次 receive 无法补全 Packet | operation buffer 生命周期太短 |
| 追加 storage 全部 8192 bytes | receiveBuffer 混入未完成范围 | 忽略 `_transferredBytes` |
| `tryParse()` 返回 `nullopt` 就清空 buffer | 半包永远不能重组 | parser 已负责保留必要 bytes |
| parser 成功后不移除已消费 frame | 同一 Packet 被重复处理 | 忽略 `_buffer` 是输入输出参数 |
| buffer 有两个 Packet 时只调用一次 parser | 第二个 Packet 长期滞留 | 一次 `tryParse()` 只返回一个 Packet |
| 把 `processReceivedPackets() == false` 一律理解成失败 | 正常 `OneShot` response 被误判 | 返回值控制 receive 是否继续 |
| `TestConnection` response 使用 request payload 原样回显 | 与当前协议实现不一致 | response 明确使用空 payload |
| response 安排后继续 `postReceive()` | `OneShot` 连接还能接收第二个 request | 忽略 final response 已接管生命周期 |
| `postSend()` 成功后立即关闭 socket | response 可能尚未真正发送完成 | 提交成功不等于最终 completion |
| 把服务端 `WSASend` completion 当作客户端已经处理 response | 过早报告连接测试成功 | 客户端必须独立完成 `readyRead`、解析和 command 检查 |
| 假设服务端关闭日志一定早于或晚于客户端 `readyRead` | 用跨主机日志顺序推导出错误因果关系 | 两端只能依赖各自的局部控制条件 |
| 认为 `closeAfterSend` 自己会发送数据 | response 永远没有 send operation | 它只记录关闭意图 |
| send 部分完成后按全部完成收尾 | 客户端只收到截断 response | 忽略 `sendOffset` |
| 把 `RequestComplete` 当成错误 | 正常日志和失败统计混乱 | 没区分业务完成与异常关闭 |
| 假设 receive 和 send completion 来自同一 worker | 线程局部状态被错误复用 | IOCP worker 对连接没有固定亲和性 |

---

## 21. 阶段练习与验收

按编号完成任务。每个任务同时检查协议字节、对象生命周期、控制流和当前返回值语义，不需要构建或修改项目代码。

### 21.1 任务一：手算 `TestConnection` request

**练习**

已知：

```text
Header = 0xFEFF
Command::TestConnection = 1981
Payload size = 0
Checksum = payload byte sum
```

回答：

1. Length 是多少？
2. Packet 总大小是多少？
3. Header、Length、Command 和 Checksum 分别怎样写成 little-endian bytes？
4. 按 offset 写出完整 wire bytes。
5. response bytes 是否相同？为什么？

**验收标准**

- [ ] Length 计算为 `4`。
- [ ] 总大小计算为 `10` bytes。
- [ ] Header 写成 `FF FE`。
- [ ] Length 写成 `04 00 00 00`。
- [ ] Command 写成 `BD 07`。
- [ ] Checksum 写成 `00 00`。
- [ ] 知道 response 也是同 command、空 payload。

**参考答案与解释**

```text
offset   00 01 02 03 04 05 06 07 08 09
bytes    FF FE 04 00 00 00 BD 07 00 00
```

Length 不包含前面的 Header 和 Length field。request 与 response 的 command、payload 和 checksum 相同，所以 wire bytes 相同。

### 21.2 任务二：推演半包与粘包

**练习**

分别推演：

1. 第一次 receive 得到前 6 bytes，第二次得到后 4 bytes。
2. 一次 receive 得到两个连续的 10-byte Packet。
3. buffer 是 `11 22 FF`，下一次 receive 以 `FE` 开头。
4. 第一个 frame length 非法，后面紧跟一个合法 Packet。

每一步写出：

```text
append 后 receiveBuffer 内容
tryParse 返回值
tryParse 后剩余 buffer
是否需要 postReceive
```

**验收标准**

- [ ] 6-byte 半包返回 `nullopt` 并保留 bytes。
- [ ] 第二次追加后成功得到一个 Packet。
- [ ] 两个 Packet 需要两次 `tryParse()` 才能全部取出。
- [ ] split header 场景保留末尾 `FF`。
- [ ] 非法 frame 不阻止 parser 寻找后续合法 header。
- [ ] 知道 `OneShot` 处理第一个 request 后不会继续第二个业务 request。

**参考答案与解释**

半包路径：

```text
6 bytes → nullopt → buffer 保留 6 → postReceive
再加 4 → 返回 TestConnection Packet → buffer 为空
```

通用 parser 对粘包一次消费一个；通用调用循环可以继续解析。当前 `TestConnection` handler 安排 final response 后终止 receive 链，所以第二个一次性 request 不会被继续处理。

### 21.3 任务三：区分两个 receive buffer

**练习**

假设：

```text
IoOperation::storage capacity = 8192
transferredBytes = 6
ConnectionContext::receiveBuffer 初始为空
```

回答：

1. `append()` 的两个实参分别是什么？
2. 应复制多少 bytes？
3. append 后谁拥有这 6 bytes？
4. 当前 receive operation 何时可以销毁？
5. 下一次 receive 为什么使用新的 operation storage，但仍能补全旧半包？

**验收标准**

- [ ] 首地址来自当前 operation storage。
- [ ] 长度使用实际完成值 `6`，不是 `8192`。
- [ ] append 后 connection receive buffer 拥有副本。
- [ ] operation 可以在 completion handler 返回时销毁。
- [ ] 跨 operation 数据由 `ConnectionContext` 保留。

**参考答案与解释**

```text
operation A storage[0, 6)
  → append 到 connection.receiveBuffer
  → operation A 销毁
  → connection.receiveBuffer 仍保存 6 bytes
  → operation B 的新 bytes 再 append 到同一 buffer
```

### 21.4 任务四：解释协议处理返回值

**练习**

分别推导下面三种场景中 parser、handler、最终返回值和下一步动作：

1. 当前只有 6-byte 半包。
2. 收到完整合法的 `TestConnection`。
3. 首包无法分类。

回答：为什么 `processReceivedPackets() == false` 不能直接翻译成“协议处理失败”？

**验收标准**

- [ ] 半包不调用 handler，返回 `true` 并继续 receive。
- [ ] 完整 Test request 安排 response 后返回 `false`。
- [ ] 分类失败关闭连接并返回 `false`。
- [ ] 能区分“停止 receive 链”和“业务失败”。

**参考答案与解释**

1. **半包**
   - parser：返回 `nullopt`。
   - handler：不调用。
   - 最终返回：`true`。
   - 下一步：再次 `postReceive()`。
2. **完整 `TestConnection`**
   - parser：返回 Packet。
   - handler：安排 final response，并返回 `false`。
   - 最终返回：`false`。
   - 下一步：等待 send completion。
3. **分类失败**
   - parser：返回 Packet。
   - handler：关闭连接并返回 `false`。
   - 最终返回：`false`。
   - 下一步：不再 receive。

`false` 只表示当前 receive completion 不应再续投；正常 `OneShot` 与失败关闭都符合这个条件。

### 21.5 任务五：还原首包分类和 response

**练习**

写出 `handleInitialPacket()` 在 `TestConnection` 路径中的五个动作：

```text
检查/转换 phase
检查 command
创建 response
安排最终发送
返回控制值
```

然后回答：

1. `tryClassify()` 的两个参数是什么？
2. phase 从什么变为什么？
3. response payload 是什么？
4. response 是否复制 request payload？
5. 为什么不进入任务池？

**验收标准**

- [ ] `_connection` 与 `OneShot` 两个实参解释正确。
- [ ] 状态变化为 `AwaitingRequest → OneShot`。
- [ ] response command 为 `TestConnection`。
- [ ] response payload 为空。
- [ ] `sendFinalPacket()` 后返回 `false`。
- [ ] 知道命令处理轻量，可在 completion worker 内完成。

**参考答案与解释**

```text
tryClassify(connection, OneShot)
  → command == TestConnection
  → response = Packet(TestConnection, empty payload)
  → sendFinalPacket(connection, response)
  → return false，停止 receive 链
```

### 21.6 任务六：还原单 response 的发送与关闭

**练习**

初始状态：

```text
queuedSendBytes = 0
sendPending = false
closeAfterSend = false
```

依次说明：

1. `enqueuePacket()` 怎样把 Packet 变成 wire bytes？
2. response bytes 进入 send operation 后由谁持有？
3. `enqueueBytes()` 为什么把 `queuedSendBytes` 改为 `10`、把 `sendPending` 改为 `true`？
4. `requestCloseAfterSend()` 为什么只记录关闭意图，而不立即关闭？
5. final send completion 后哪两个发送状态恢复初始值？
6. 如果只完成部分 bytes，为什么不能触发 `RequestComplete`？

**验收标准**

- [ ] byte 数从 0 增加到 10。
- [ ] 第一条 send 令 `sendPending = true`。
- [ ] send operation 持有完整 response bytes 和 connection 引用。
- [ ] final response 令 `closeAfterSend = true`。
- [ ] 全部完成后 byte 数归零、`sendPending = false`。
- [ ] 关闭已请求且 final response 已完成时使用 `RequestComplete`。
- [ ] 部分完成直接复用阶段四结论，不提前关闭。

**参考答案与解释**

1. **初始状态**
   - queued bytes：`0`。
   - pending：`false`。
   - close after send：`false`。
2. **response 进入发送**
   - queued bytes：`10`。
   - pending：`true`。
   - close after send：`false`。
3. **请求最终关闭**
   - queued bytes：`10`。
   - pending：`true`。
   - close after send：`true`。
4. **10 bytes 全部完成**
   - queued bytes：`0`。
   - pending：`false`。
   - close after send：仍为 `true`，随后关闭连接。

部分完成时，完整 response 仍由 operation 持有，`sendPending` 仍表示有发送在途，不能按最终完成清理。

### 21.7 任务七：区分服务端结束与客户端成功

**练习**

分别写出下面四个观察点的判断函数、条件和对外结果，并回答服务端与客户端事件是否存在固定的全局先后：

1. 服务端 final response 完成。
2. 客户端收到有效 response。
3. 服务端 send completion 失败。
4. 客户端 response command 不匹配。

**验收标准**

- [ ] 服务端只有在 final response 完成后才使用 `RequestComplete`。
- [ ] 客户端只有解析出 command 匹配的 Packet 后才报告 `connectionTested(true)`。
- [ ] 服务端发送完成不等于客户端业务已经成功。
- [ ] 能分别指出服务端 I/O 失败与客户端协议失败入口。
- [ ] 不用两端日志的固定先后推导因果关系。

**参考答案与解释**

1. **服务端 final response 完成**
   - 判断函数：`handleSendCompletion()`。
   - 条件：operation 的全部 bytes 已完成，并且已经请求发送后关闭。
   - 结果：`closeConnection(RequestComplete)`。
2. **客户端收到有效 response**
   - 判断函数：`handlePacket()`。
   - 条件：response command 等于 `TestConnection`。
   - 结果：`connectionTested(true, ...)`。
3. **服务端 send completion 失败**
   - 判断函数：`handleSendCompletion()`。
   - 条件：`_success == false`。
   - 结果：以 `IoFailure` 关闭连接。
4. **客户端 command 不匹配**
   - 判断函数：`handlePacket()`。
   - 条件：`_packet.command != m_command`。
   - 结果：request 报告协议失败。

两端只保证各自内部顺序。服务端 `RequestComplete` 与客户端 `connectionTested(true)` 是两个独立观察点。

### 21.8 最终综合验收

**练习**

闭卷复述：

```text
共同前半段：
RemoteClient::testConnection
  → connectToHost
  → connected
  → 构造并序列化 10-byte request
  → write
  → 服务端 receive completion
  → append actual bytes
  → tryParse
  → classify OneShot
  → 创建 10-byte response
  → enqueue / postSend

服务端收尾：
  → final send completion
  → close(RequestComplete)

客户端确认：
response bytes 到达
  → readyRead
  → tryParse response
  → connectionTested(true)
```

服务端收尾与客户端确认不是一条跨主机的严格时间线。复述时应分别说明两端的局部顺序，而不是断言两端 callback 的固定先后。

复述时必须同时指出：

1. request 和 response 的十个 bytes。
2. receive operation storage 与 connection receive buffer 的所有权。
3. 半包时为什么继续 receive。
4. 完整 Test request 后为什么停止 receive。
5. `Packet`、serialized bytes、send operation 的所有权变化。
6. `OneShot` 最小状态变化。
7. `closeAfterSend` 与 send completion 的配合。
8. receive 和 send 可能由不同 worker 处理。
9. 客户端在哪一步确认成功。
10. 任一步失败后进入哪个类型的退出入口。

**验收标准**

- [ ] 协议字节计算正确。
- [ ] 不把 TCP receive chunk 当作 Packet 边界。
- [ ] 不丢失半包，也不重复处理已消费 Packet。
- [ ] 能解释三个关键 bool：parser optional、协议处理返回值、`sendPending`。
- [ ] 能说明 `handleInitialPacket()` 返回 `false` 的正常含义。
- [ ] response 真正完成前不会关闭 socket。
- [ ] 能区分服务端 `RequestComplete` 与客户端 `connectionTested(true)` 两个完成点。
- [ ] `RequestComplete` 被识别为正常终点。
- [ ] 完整复述不依赖状态机 CAS、多个 send 排队、背压算法、任务池或安全停机知识。

**参考答案与解释**

完整答案至少包含以下九层：

1. 客户端创建 `OneShotRequest`，异步连接成功后发送 `FF FE 04 00 00 00 BD 07 00 00`。
2. 服务端 receive operation 只把实际完成范围复制到 connection 级 `receiveBuffer`，使半包能够跨 operation 保存。
3. `tryParse()` 校验 header、length 和 checksum；半包返回 `nullopt`，完整包则消费 10 bytes 并返回 `Packet`。
4. 半包令协议处理返回 `true` 并再次 `postReceive()`；完整 Test request 进入首包 handler。
5. 首包把 phase 从 `AwaitingRequest` 变为 `OneShot`，创建同 command、空 payload response。
6. `sendFinalPacket()` 序列化 response，简单路径创建拥有完整 bytes 的 send operation，并设置发送后关闭意图。
7. 如果出现部分 send，直接按阶段四结论继续剩余 bytes；全部完成后才清理当前发送状态。
8. “final send 完成”和“closeAfterSend 已请求”同时满足后，以 `RequestComplete` 正常关闭。
9. 客户端累计 response bytes、复用 `tryParse()`，确认 command 后报告连接测试成功。

全部任务通过后，阶段六才算完成。

---

## 22. 下一阶段衔接

阶段六使用的是最简单的线性路径：

```text
一个连接
  → 一个 TestConnection request
  → 一个 response
  → 一个 send operation
  → 正常关闭
```

它已经暴露了阶段七要解决的四个一般化问题：

```text
首个 Packet 怎样安全决定连接长期角色？
多个 response 同时产生时怎样保持发送顺序？
生产速度持续高于发送速度时怎样限制内存？
关闭请求与 send completion 并发发生时怎样保证只关闭一次？
```

阶段六结论在阶段七中的用途：

| 阶段六已经掌握 | 阶段七继续扩展 |
| --- | --- |
| `AwaitingRequest → OneShot` 的一次分类 | 完整合法状态转换和并发 CAS |
| 一个 response 在无竞争时直接 `postSend()` | 多个 response 进入有序 `sendQueue` |
| `sendPending` 表示一个 send 在途 | 保证每条连接最多一个 `WSASend` 在途 |
| `queuedSendBytes` 在完成后归零 | 对在途和排队 bytes 实施容量上限 |
| `closeAfterSend` 等 final response 完成 | 在队列排空后准确关闭 |
| receive/send worker 可能不同 | 用连接级锁保护共享发送状态 |

进入阶段七前，应能够准确回答：

> `TestConnection` response 已经创建后，为什么 `handleInitialPacket()` 停止 receive，而 socket 又必须继续存活到最终 send completion？

---

## 23. 官方资料与项目资料

阅读时重点关注 TCP 字节流、异步 Qt socket、buffer 消费语义和项目 Packet 布局。

- [QAbstractSocket Class](https://doc.qt.io/qt-6/qabstractsocket.html)
- [QIODevice Class](https://doc.qt.io/qt-6/qiodevice.html)
- [QByteArray Class](https://doc.qt.io/qt-6/qbytearray.html)
- [QDataStream Class](https://doc.qt.io/qt-6/qdatastream.html)
- [WSARecv function](https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-wsarecv)
- [WSASend function](https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-wsasend)

以下项目资料路径相对于 `D:\CodeRepository\claude\remote_control`：

- `docs\ProtocolReference.md`
- `tests\ProtocolTests.cpp`
