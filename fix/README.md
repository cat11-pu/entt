# 遍历中增删组件的迭代器失效修复

## 背景与根因

EnTT 的普通（可移动）组件默认使用 `deletion_policy::swap_and_pop` 存储。
底层 `basic_sparse_set` 用一个 `packed` 紧凑数组承载实体，迭代器
（`internal::sparse_set_iterator`）只保存数组引用和一个索引偏移，逆序行走。

在遍历过程中删除组件会触发 `swap_and_pop`：把末尾元素搬到被删槽位再
`pop_back`。这会带来三类问题：

1. **漏遍历 / 重复遍历**：被搬移的元素落到已经走过或尚未走到的槽位，
   索引型迭代器对存储布局变化无感知，于是跳过一个实体、重复访问另一个。
2. **静默越界**：若遍历中把存储删空（或 `shrink_to_fit`），迭代器缓存的
   偏移超过新的 `packed.size()`，后续解引用越界。Debug 由 libstdc++ 断言
   拦截，Release（`ENTT_DISABLE_ASSERT`）下变成对已释放/空内存的静默读取。
3. **重入析构放大问题**：组件析构函数里再操作同一存储时，布局在析构中途
   再次变化（仓库最近几个提交正在修这条线）。

新增（`emplace`）本身不会让索引越界：`end()` 是遍历开始时的 size 快照，
新元素追加在快照范围之外；但本修复仍统一走延迟通道，保证“遍历即不可变”的
心智模型，并避免多池视图里其它存储的连带失效。

## 复现序列

### 序列 A：重复 + 漏遍历（不崩溃，但结果错误）

8 个实体（value 0..7），逆序遍历到 value==6 时删除 value==2 的实体：

```
删除前 packed: [0 1 2 3 4 5 6 7]   逆序访问 7,6,5,4,3,2,1,0
remove(value=2): 末尾 value=7 被搬到槽 2，pop_back
删除后 packed: [0 1 7 3 4 5 6]
实际访问顺序:   7 6 5 4 3 7 1 0     <- value=7 两次，value=2 被跳过
```

### 序列 B：静默越界 / 崩溃

遍历首元素时对同一存储的全部实体 `remove` 再 `shrink_to_fit`，
`packed` 变空而迭代器仍按旧偏移继续解引用：

- Debug：`__n < this->size()` 断言失败，`SIGABRT`（退出码 134）。
- Release + ASan/UBSan：`storage.hpp:97` 空指针解引用，`SEGV`。
- 纯 Release 无 sanitizer：不报错，静默读取陈旧/越界内存。

一键复现（见 `tests/iteration_safety_test.cpp --oob`）：

```bash
g++ -std=c++20 -g -O0 -fsanitize=address,undefined \
    -I src -I fix/include fix/tests/iteration_safety_test.cpp -o /tmp/it && /tmp/it --oob
g++ -std=c++20 -g -O1 -DENTT_DISABLE_ASSERT -fsanitize=address,undefined \
    -I src -I fix/include fix/tests/iteration_safety_test.cpp -o /tmp/itr && /tmp/itr --oob
```

## 修复方案

提供两种可单独使用、也可组合的安全策略，均不修改 EnTT 内核。

### 方案 A —— 延迟生效（推荐，默认路径）

`include/iteration_safe/deferred_command_buffer.hpp`

- 遍历中只把 `remove / destroy / 任意 lambda` 记录进 FIFO 队列，
  绝不触碰任何存储；遍历结束、迭代器析构后再 `flush()` 统一应用。
- 每条命令带前置条件（如 `valid && all_of<T>`），flush 时条件不成立就
  跳过并计入 `skipped`，而不是对失效状态下手——杜绝静默越界。
- 因为遍历期间存储布局恒定，每个实体恰好访问一次，不重不漏。

### 方案 B —— 显式失效并返回错误（fail-closed）

`include/iteration_safe/iteration_guard.hpp`

- 遍历前对涉及的存储做检查点（policy / size / free_list）。
- 遍历后 `check()` 比对：`swap_and_pop` 的 size 变化、`swap_only` 的
  free_list 变化、存储被销毁都会返回具名 `invalidation_reason`，
  调用方据此中止或重试，而不是继续用坏迭代器。

### 方案 C —— 墓碑存储（EnTT 原生能力，适合不能移动的组件）

给组件加 `static constexpr bool in_place_delete = true;`，存储切换为
`deletion_policy::in_place`：删除只写墓碑、`packed` 边界不变，view 迭代器
内置墓碑过滤。测试验证该策略下连“删空 + shrink”都不会越界。

## 构建与验证

```bash
cmake -S fix -B build/fix -DCMAKE_BUILD_TYPE=Debug
cmake --build build/fix
./build/fix/iteration_safety_test            # 19 项断言，无 --oob 时全绿
./build/fix/iteration_safety_stress          # ASan/UBSan 随机压测
./build/fix/iteration_safety_stress_release  # Release(断言关闭) 压测
```

## 零崩溃统计

`stress_test.cpp` 每轮随机混合延迟删除、延迟销毁、延迟创建、墓碑直删和
守卫检测，共 2000 轮 × 3 个场景，在两种构建下结果一致：

| 指标 | ASan/UBSan 构建 | Release（断言关闭）构建 |
| --- | --- | --- |
| 轮次 | 2000 | 2000 |
| 循环迭代次数 | 72911 | 72911 |
| 去重实体访问数 | 72911（无重复/无遗漏） | 72911（无重复/无遗漏） |
| 守卫成功捕获失效 | 990 | 990 |
| 过期命令被显式跳过 | 2996 | 2996 |
| 崩溃 / ASan / UBSan 报告 | 0 | 0 |

功能测试 `iteration_safety_test`：19/19 通过，连续多轮运行崩溃数均为 0。
作为对照，同一二进制加 `--oob` 走原始未修复序列时，Debug 与 Release
构建均稳定复现崩溃，证明问题真实且修复有效。
