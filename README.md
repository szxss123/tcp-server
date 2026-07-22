# C++ HTTP Server

## 项目简介

这是一个基于 C++17、Linux Socket、CMake 和固定大小线程池实现的简易 HTTP Server。项目从 TCP Socket 的创建、绑定和监听开始，实现了多客户端并发连接、基础 HTTP 请求识别、响应构造与网络资源管理，适合作为 Linux 网络编程和 C++ 并发编程的入门实践项目。

## 已实现功能

- 使用 Linux Socket API 创建 TCP 服务端
- 默认监听 `0.0.0.0:8080`，支持通过命令行参数指定端口
- 接受多个客户端并发连接
- 使用固定大小线程池处理客户端任务
- 使用互斥锁、条件变量和任务队列协调工作线程
- 识别 HTTP GET 请求并返回 `200 OK`
- 对非 GET 请求返回 `405 Method Not Allowed`
- 正确生成 `Content-Type`、`Content-Length` 和 `Connection: close` 响应头
- 循环调用 `send()`，保证完整发送响应
- 使用 RAII 管理客户端连接描述符
- 处理系统调用错误、`EINTR` 和客户端提前断开等情况

## 项目结构

```text
tcp-server/
├── CMakeLists.txt
├── README.md
├── include/
│   ├── TcpServer.h
│   └── ThreadPool.h
└── src/
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

## 当前架构

```text
客户端
  ↓ HTTP 请求
监听 Socket
  ↓ accept
任务队列
  ↓
固定线程池
  ↓
请求解析 → 构造响应 → send → close
```

主线程负责监听端口并接受连接，将客户端处理任务提交到共享队列。固定数量的工作线程通过条件变量等待任务，从队列取出连接后完成请求读取、基础方法识别、响应构造和发送。客户端描述符由 RAII 对象负责关闭，线程池析构时会唤醒并回收全部工作线程。

## 后续计划

- 完整读取并解析 HTTP 请求行和请求头
- 支持路由、静态文件和更多 HTTP 状态码
- 增加请求大小限制、超时和异常请求处理
- 完善优雅停机，主动关闭仍在处理的客户端连接
- 增加自动化单元测试、集成测试和结构化日志
- 根据压力测试结果优化任务队列和线程池策略

## 简历描述

- 基于 C++17、Linux Socket 与 CMake 实现 HTTP 服务器，完成 Socket 创建、端口绑定、连接监听、HTTP 请求解析及响应发送。
- 设计固定大小线程池，通过互斥锁、条件变量和任务队列管理并发连接，并使用 RAII 降低网络资源泄漏和重复释放风险。
