# OTSH Hash Backend (C++ / CMake)

实现 `设计文档.md` 中“连续数组 idx 语义 + mini-bin + fallback + k-kick”的数据库版哈希系统（MySQL/MariaDB 为存储与计算层）。

数据库表结构（简化版）：

- `hash_table(idx, key, fp)`
  - `idx`：连续数组语义下标（PRIMARY KEY）
  - `key`：空槽为 NULL（UNIQUE，允许多个 NULL）
  - `fp`：16-bit fingerprint（空槽为 0）

## 依赖

- CMake ≥ 3.20
- C++20 编译器
- MySQL/MariaDB client dev（需要 `mysql.h` + `libmariadb`/`mysqlclient`）
- 本地 MySQL（文档要求）

## 构建

```bash
cmake -S backend-cpp -B backend-cpp/build
cmake --build backend-cpp/build -j
```

## 运行

先确保 MySQL 中有数据库 `otsh`（或用环境变量改名）。

你的环境约束：

- 只能用 `root`
- `root` 密码为你本机固定值（不要写进仓库），不允许创建新用户

建库（PowerShell）：

```powershell
& "E:\jzc\MySQL\MySQL Server 8.0\bin\mysql.exe" -uroot -p -e "CREATE DATABASE IF NOT EXISTS otsh;"
```

环境变量：

- `DB_HOST` `DB_PORT` `DB_USER` `DB_PASSWORD` `DB_NAME`
- `BIND_HOST`（默认 `0.0.0.0`）
- `PORT`（默认 `8080`）

启动：

```powershell
cd backend-cpp
$env:DB_PASSWORD="12345678"
.\build\otsh_backend.exe
```

## API

- `GET /health`
- `POST /api/init` `{ "n": 100000, "k": 2, "load_factor": 0.98 }`（会清空所有 key/fp 重新初始化）
- `POST /api/insert` `{ "key": 123 }`
- `POST /api/find` `{ "key": 123 }`
- `POST /api/erase` `{ "key": 123 }`
- `GET /api/stats`
