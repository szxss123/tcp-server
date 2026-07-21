# C++ HTTP Server

基于 C++17、Linux Socket、CMake 和线程池实现的简易 HTTP Server。

## 当前功能

- TCP 端口监听
- 多客户端并发连接
- 固定大小线程池
- HTTP GET 请求处理
- 200 和 405 响应
- RAII 资源管理

## 编译运行

```bash
cmake -S . -B build
cmake --build build
./build/tcp_server
```

服务器默认监听 `0.0.0.0:8080`。也可以通过命令行参数指定端口：

```bash
./build/tcp_server 9000
```

## HTTP 测试

发送 GET 请求：

```bash
curl -v http://127.0.0.1:8080/
```

浏览器访问：

```text
http://127.0.0.1:8080/
```

发送非 GET 请求：

```bash
curl -v -X POST http://127.0.0.1:8080/
```

非 GET 请求将返回：

```http
HTTP/1.1 405 Method Not Allowed
```

## 简历素材

基于 C++17 与 Linux Socket 实现多线程 HTTP 服务器，使用固定大小线程池管理并发连接，支持基础 HTTP 请求解析与响应，并通过 RAII 管理网络资源。