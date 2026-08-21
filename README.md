# C++ HTTP Server

## 项目简介

这是一个基于 C++17、Linux Socket、CMake 和固定大小线程池实现的简易 HTTP Server。项目从 TCP Socket 的创建、绑定和监听开始，实现了多客户端并发连接、基础 HTTP 请求识别、响应构造与网络资源管理，适合作为 Linux 网络编程和 C++ 并发编程的入门实践项目。

## 已实现功能

- C++17 与 CMake 构建
- Socket RAII 封装
- 固定大小线程池
- epoll 事件分发
- 非阻塞 ET 连接接收
- HTTP 分片请求头累积
- 基础请求解析与 200、400、404、405 状态码响应
- 8KB 请求头大小限制
- HTTP/1.1 Keep-Alive 与空闲连接超时
- 静态 HTML/CSS 文件服务
- 小文件缓冲发送与大文件非阻塞 sendfile 发送
- MIME 类型识别
- 路径穿越防护
- 单文件 16MB 大小限制
- 有界队列异步访问日志
- 基于 signalfd 的 SIGINT/SIGTERM 优雅停机
- 基于 std::atomic 的线程安全运行指标与 `GET /metrics`

## 项目结构

```text
tcp-server/
├── CMakeLists.txt
├── README.md
├── public/
│   ├── index.html
│   └── style.css
├── include/
│   ├── AsyncLogger.h
│   ├── HttpRequest.h
│   ├── SocketFd.h
│   ├── TcpServer.h
│   └── ThreadPool.h
└── src/
    ├── AsyncLogger.cpp
    ├── HttpRequest.cpp
    ├── SocketFd.cpp
    ├── main.cpp
    ├── TcpServer.cpp
    └── ThreadPool.cpp
```

## 编译运行方法

环境要求：Linux 或 WSL2、支持 C++17 的编译器、CMake 3.16 或更高版本。

```bash
cd ~/projects/tcp-server
cmake -S . -B build
cmake --build build
./build/tcp_server
```

服务器默认使用 `8080` 端口。也可以指定其他端口：

```bash
./build/tcp_server 9000
```

### 启动与停止

前台启动后，可直接按 `Ctrl+C`。服务器通过 signalfd 接收 `SIGINT`，不会在异步信号处理器中执行清理操作：

```bash
./build/tcp_server
```

后台运行时，可以向进程发送 `SIGTERM`：

```bash
./build/tcp_server > /tmp/tcp-server.log 2>&1 &
PID="$!"
kill -TERM "$PID"
wait "$PID"
```

也可以查找正在运行的服务进程后停止：

```bash
PID="$(pgrep -n tcp_server)"
kill -TERM "$PID"
```

收到 `SIGINT` 或 `SIGTERM` 后，服务器停止接受新连接，退出 epoll 事件循环，等待线程池中已经入队的任务完成，关闭剩余客户端连接，刷新并停止异步日志线程，最后释放 timerfd、signalfd、epoll 和监听 Socket。

浏览器访问：

```text
http://127.0.0.1:8080/
```

## curl 测试方法

发送 GET 请求，预期返回 `HTTP/1.1 200 OK` 和 HTML 页面：

```bash
curl -v http://127.0.0.1:8080/
```

发送非 GET 请求，预期返回 `HTTP/1.1 405 Method Not Allowed`：

```bash
curl -v -X POST http://127.0.0.1:8080/
```

连续发送 10 次请求：

```bash
for i in $(seq 1 10); do
    curl -s http://127.0.0.1:8080/ > /dev/null
done
```

## 运行指标

通过以下请求读取服务器当前进程的运行指标：

```bash
curl -s http://127.0.0.1:8080/metrics
```

接口返回 `text/plain; charset=UTF-8`，每行由指标名称和无符号整数值组成。指标是多个原子计数器在读取瞬间形成的近似快照，不保证所有字段严格对应同一个时刻，也不会暴露客户端 IP、请求内容或本地文件路径。

| 指标 | 含义 |
| --- | --- |
| `tcp_server_total_requests` | 成功解析出的完整 HTTP 请求总数，包含 `/metrics` 请求本身 |
| `tcp_server_active_connections` | 当前仍在连接状态表中的客户端连接数 |
| `tcp_server_total_connections` | 成功加入连接状态表的客户端连接累计数 |
| `tcp_server_bytes_sent_total` | 所有成功 `send()` 和 `sendfile()` 调用实际发送的累计字节数 |
| `tcp_server_client_errors_total` | 已生成的 400、404 和 405 响应累计数 |
| `tcp_server_server_errors_total` | 内部错误或 500 响应累计数 |

一次本机 WSL2 环境测试使用并发度 100 发起 1000 次首页请求，失败数为 0，总耗时为 1507 ms。该结果仅用于本次功能回归和指标核对，会受到硬件、系统负载、构建类型及网络环境影响，不代表服务器的普遍性能上限。

## I/O模型

服务器使用 epoll 监听网络事件：

- 监听 Socket：非阻塞 + ET
- 客户端 Socket：LT
- HTTP处理：固定大小线程池
- Socket资源：RAII自动管理
- 静态文件：小文件读入缓冲区，256KB 及以上文件使用 sendfile

## 当前架构

```text
客户端
  ↓
非阻塞监听 Socket（ET）
  ↓ accept4 循环至 EAGAIN
epoll 事件循环
  ↓ 客户端可读
固定大小线程池
  ↓
累计 HTTP 请求 → 解析请求行 → 静态文件路由
  ↓
路径规范化与边界检查 → MIME 识别 → 文件大小检查
  ↓
200 / 400 / 403 / 404 / 405 响应
  ├→ 小文件 send / 大文件 sendfile → SocketFd 自动释放或 Keep-Alive 复用
  └→ 访问日志记录 → 有界队列
                         ↓
                    独立日志线程 → logs/access.log
```

监听 Socket 使用非阻塞 ET 模式并在事件到达后循环调用 accept4，直至返回 EAGAIN。客户端 Socket 保持 LT 模式，可读后先从 epoll 移除，再交给固定大小线程池。工作线程累计读取请求头并调用独立解析器，响应完成后由 SocketFd 自动关闭连接描述符。
## 后续计划

- 继续完善 HTTP 请求头语义校验，并支持请求体解析
- 支持更多 MIME 类型、缓存控制和条件请求
- 增加连接与读取超时，完善异常请求处理
- 增加自动化单元测试、集成测试和结构化日志
- 根据压力测试结果优化任务队列和线程池策略

## 简历描述

- 基于 C++17 与 Linux Socket 实现 HTTP 服务器，使用 epoll 完成 I/O 事件分发，并采用非阻塞 ET 模式批量接收突发连接。
- 设计固定大小线程池和可移动的 Socket RAII 封装，实现 HTTP 请求分片累积、请求行解析及 200、400、404、405 响应，完成 500 次并发请求测试。
- 实现安全的静态资源服务，基于 std::filesystem 完成路径规范化和文件类型识别，并通过路径穿越校验及文件大小限制降低异常访问风险。
- 基于非阻塞 sendfile 实现大文件零拷贝传输，通过连接状态保存文件偏移和剩余字节，正确处理部分发送、EAGAIN、慢客户端及异常断开。
- 设计有界队列异步访问日志模块，通过条件变量和独立消费线程批量落盘，降低磁盘 I/O 对网络工作线程的阻塞，并对队列溢出进行统计与降级处理。
- 使用 std::atomic 实现低开销运行指标统计，提供请求量、活跃连接、发送字节及错误计数接口，并通过统一连接回收保证并发场景下指标一致性。
