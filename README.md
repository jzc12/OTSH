# OTSH Hash Backend (C++ / CMake)

## 依赖

- CMake ≥ 3.20
- C++20 编译器
- MySQL/MariaDB client dev

## 构建

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

## 自动化测试

可执行文件 `otsh_tests`：对哈希表做初始化、批量插入/查询/删除等检查；
跑完后打印一行 JSON，并尽量写入 MySQL 表 `otsh_test_session_log`

- Windows：

```powershell
.\scripts\run_tests.ps1
```

环境变量与 HTTP 后端一致，使用 `OTSH_MYSQL_*`；文本日志默认仓库根目录 `test_output/test_session.log`，可用 `OTSH_TEST_LOG` 覆盖。

启动 HTTP 服务：

```powershell
.\build\otsh_backend.exe
```
