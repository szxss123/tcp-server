# Minimal C++ TCP Server

一个面向 WSL2/Linux 的最小 C++17 TCP Server。它监听指定端口，接受客户端连接，记录客户端地址，并返回一行欢迎消息。

## 1. 配置 WSL2（Windows PowerShell，管理员）

```powershell
wsl --install -d Ubuntu
```

安装后重启 Windows，打开 Ubuntu，并在项目根目录执行：

```bash
chmod +x scripts/setup_wsl.sh
./scripts/setup_wsl.sh
```

如果项目位于 Windows 的 `C:` 盘，可从 WSL2 通过 `/mnt/c/...` 访问。开发时建议把仓库放在 WSL 的 Linux 文件系统（如 `~/projects`），编译和文件访问通常更快。

## 2. 编译

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
```

## 3. 运行和测试

终端 1：

```bash
./build/tcp_server 8080
```

终端 2：

```bash
nc 127.0.0.1 8080
```

客户端会收到：

```text
Hello from TCP server!
```

端口参数可省略，默认监听 `8080`。服务端监听 `0.0.0.0`，因此 Windows 通常也可通过 `localhost:8080` 访问。

## 4. 使用 GDB

```bash
gdb --args ./build/tcp_server 8080
```

进入 GDB 后：

```gdb
break main
run
```

如果使用 VS Code，请在 WSL2 中安装 `code`，安装 **WSL** 与 **C/C++** 扩展，然后选择 `Debug TCP Server`。项目已包含构建任务和 GDB 启动配置。

## 5. 创建 GitHub 仓库

先在 GitHub 创建一个空仓库（不要勾选 README），然后执行：

```bash
git init
git add .
git commit -m "feat: add minimal TCP server"
git branch -M main
git remote add origin https://github.com/YOUR_NAME/tcp-server.git
git push -u origin main
```

也可以安装并登录 GitHub CLI 后一条命令创建：

```bash
gh auth login
gh repo create tcp-server --public --source=. --remote=origin --push
```

## 目录结构

```text
.
├── .vscode/           # VS Code 构建与 GDB 调试配置
├── scripts/           # WSL2 工具链安装脚本
├── src/main.cpp       # TCP Server
├── CMakeLists.txt     # CMake 构建配置
└── README.md
```
