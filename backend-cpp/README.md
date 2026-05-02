# OTSH Hash Backend (C++ / CMake)

实现 `设计文档.md` 的后端（内存结构 + 可持久化存储），并仅提供 `init/insert/delete/query` 四个对接接口。

存储固定为 **MySQL**（需要 MySQL/MariaDB client 库）。

## 依赖

- CMake ≥ 3.20
- C++20 编译器
- MySQL/MariaDB client dev（需要 `mysql.h` + `libmariadb`/`mysqlclient`）

## 构建

```bash
cmake -S backend-cpp -B backend-cpp/build -DCMAKE_BUILD_TYPE=Release
cmake --build backend-cpp/build -j
```

启动：

```powershell
.\backend-cpp\build\otsh_backend.exe
```

## API

- `GET /health`
- `POST /api/init`
  - `{ "n": 100000, "k": 2, "load_factor": 0.98, "mysql_host":"127.0.0.1", "mysql_port":3306, "mysql_user":"root", "mysql_password":"...", "mysql_database":"otsh", "mysql_table":"otsh_keys", "reset": true|false }`
- `POST /api/insert` `{ "key": 123 }`
- `POST /api/query` `{ "key": 123 }`
- `POST /api/delete` `{ "key": 123 }`
- `POST /api/stats` `{}`（只读：输出指标与当前表状态，便于验收/压测）
