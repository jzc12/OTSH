# OTSH — 面向实践的紧凑哈希表（本地测试版）

依据《系统重构方案 v1》实现的 Facility / Cubby / k-kick / Local Query Router 原型。

## 依赖

- CMake ≥ 3.20
- C++20 编译器

## 构建

```powershell
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

产物：`otsh_tests`（单元测试）、`otsh_experiment`（第四章实验）。

## 单元测试

```powershell
.\build\otsh_tests.exe
```

## 第四章实验（口径见 experiments/out.log）

```powershell
.\experiments\run.ps1 -Quick
.\experiments\run.ps1
.\build\otsh_experiment.exe --group=all --quick --seed=1
```

参数说明见 `experiments/presets.json`。
