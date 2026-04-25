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
cmake --build backend-cpp/build -j
```
启动：

```powershell
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
