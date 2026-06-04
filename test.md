# 基于 Hot-Tail Cubby 与 SQLite 的混合哈希存储系统设计与实现

作者：纪泽操

## 摘要

针对动态键值存储系统中查询效率、删除维护成本与持久化能力难以同时兼顾的问题，本文设计并实现了一种基于 Hot-Tail Cubby 与 SQLite 的混合哈希存储系统。系统以原始多层 cubby 哈希结构作为基础方案，在统一的 `IKeyStore` 接口下构建了三种实现：V1 为基于 facility 与多层 cubby 的内存哈希结构，V2 为按 facility 分表的纯 SQLite 对照方案，V3 为本文提出的热尾缓存与冷数据持久化混合方案。V3 保留每个 facility 内的 tail cubby 作为近期数据热缓存，利用 k-kick 插入和局部路由器支持常数级内存访问；当 tail cubby 写满后，将其转为 frozen tail，并在背压或收尾阶段批量写入 SQLite 冷存储。删除 frozen tail 中元素时采用 tombstone 机制，以避免只读冻结块的原地修改。

本文从数据结构设计、复杂度分析、系统实现和实验评估四个层面对三种方案进行比较。实验在相同键集合、相同哈希分区、相同近期局部性 workload 和相同统计口径下，测量插入、查询、删除平均延迟、单位键空间占用以及操作正确率。实验结果表明，在近期数据更热的访问场景下，V3 能够利用 hot tail 与 frozen tail 减少 SQLite 访问次数，在保持冷数据持久化能力的同时降低热查询与热删除的平均成本。本文工作体现了对原始复现系统的结构性改造，创新点在于将多层 cold cubby 维护替换为按 facility 分区的 SQLite 冷存储，并通过 hot tail/frozen tail/tombstone 机制实现面向近期局部性的混合存储优化。

关键词：动态哈希；k-kick；热缓存；SQLite；键值存储；近期局部性

## Abstract

This thesis designs and implements a hybrid hash-based key storage system based on hot-tail cubbies and SQLite. The system compares three implementations under a unified `IKeyStore` interface: an in-memory tiered-cubby hash table, a pure SQLite baseline, and the proposed hybrid design. The hybrid design keeps recently inserted keys in per-facility hot tail cubbies and stores cold keys in per-facility SQLite tables. Frozen tails and tombstones are used to decouple tail rotation, batch flushing, and deletion. Experiments are conducted with identical key sets, identical workload traces, identical facility partitioning rules, and consistent metrics. The results show that the hybrid design can reduce the average cost of hot queries and hot deletions while preserving persistent cold storage.

Keywords: dynamic hashing; k-kick; hot cache; SQLite; key-value storage; temporal locality

## 论文重写说明

本文件用于解决原论文中“题目不聚焦、摘要不规范、公式未编号、创新点不足、内容偏复现”等问题。重写时建议遵循以下原则：

1. 题目应突出“设计与实现”和“混合哈希存储”主题，避免只描述复现对象。
2. 摘要应包含研究背景、问题、方法、实现、实验指标、结果和创新点，不写空泛评价。
3. 所有核心公式均应编号，并在正文中引用。
4. 创新点应围绕 V3 展开：Hot-Tail Cubby 热缓存、Frozen Tail 队列、SQLite 冷存储、Tombstone 删除机制、统一对照实验框架。
5. 实验部分应把 V1、V2、V3 放在同一输入和同一指标下比较，避免只展示单系统复现结果。

## 目录建议

1. 绪论
2. 相关理论与技术
3. V1 原始多层 Cubby 哈希系统
4. V2 与 V3 存储方案设计
5. 复杂度分析
6. 系统实现
7. 实验设计与结果分析
8. 总结与展望

# 第一章 绪论

## 1.1 研究背景

哈希表是键值存储、数据库索引、缓存系统和内存管理中的基础数据结构。传统哈希表通常追求均摊常数时间的插入、查询与删除，但在动态扩容、高装载率、元数据空间、持久化能力和近期访问局部性利用方面仍存在权衡。对于本科毕业设计而言，仅复现某一种理论哈希结构难以体现完整系统设计能力，因此本文在原始多层 cubby 哈希结构基础上进一步设计了面向近期局部性的混合存储方案。

在真实键值存储场景中，新近写入的数据往往具有更高的再次访问概率。例如日志索引、会话状态、在线服务缓存和短期任务状态都表现出明显的时间局部性。如果所有数据都放在磁盘数据库中，热数据访问会受到 B-tree 查询和 I/O 路径影响；如果所有数据都放在复杂内存层级结构中，则冷数据维护和持久化成本较高。本文提出的 V3 方案试图在二者之间取得平衡：用 hot tail cubby 承接近期热数据，用 SQLite 承接冷数据。

## 1.2 研究问题

本文围绕如下问题展开：

1. 如何在保留原始 cubby 哈希结构高效内存访问能力的同时，引入可持久化的冷数据存储？
2. 如何将 V1、V2、V3 三种方案统一到相同接口和实验框架中，保证对比公平？
3. 在近期数据更热的 workload 下，V3 是否能够降低热查询和热删除的平均成本？
4. V3 引入 SQLite 后，空间统计口径和正确性验证应如何设计？

## 1.3 国内外研究现状

哈希表是算法教材和工程系统中最常见的字典结构之一，经典讨论通常从链地址法、开放寻址法、负载因子和冲突处理策略展开 [1,30]。链地址法利用链表或桶结构吸收冲突，结构直观、删除方便，但指针、动态分配和缓存不连续会带来额外空间与访存开销；开放寻址法将元素直接放入连续槽位，查询路径更容易利用缓存局部性，但在高装载率下会受到探测序列变长、删除标记积累和重哈希成本的影响。近年来的哈希研究不再只追求平均常数时间，而是同时关注每键元数据、缓存行为、动态更新成本和工程可实现性。Bender 等人的综述性工作 *Modern Hashing Made Simple* 对开放寻址、局部重排、动态维护和高装载率哈希结构作了较系统梳理，为理解现代哈希表的理论与实现差异提供了背景 [2]。

在随机化与开放寻址方向，研究重点之一是如何用较弱但可实现的随机性保证探测长度和装载表现。Carter 与 Wegman 提出的 universal hashing 奠定了哈希函数族分析基础 [22]，Luby 与 Rackoff 说明了如何由伪随机函数构造伪随机置换 [21]，这类思想使“先将原始键映射到伪随机地址空间、再在局部结构中放置”的设计具有理论依据。Thorup 与 Zhang 的 tabulation-based hashing 进一步表明，有限独立性或表格化哈希在许多场景下可以提供足够的随机性 [23]；Pagh 关于有限独立哈希的研究也说明，哈希表实现未必需要完全随机函数即可获得可证明性质 [25]。在线性探测方面，Pagh、Pagh 与 Ruzic 证明了常数独立性也能支撑线性探测的性能保证 [20]。此外，“power of two choices” 的负载均衡思想说明，只要为每个元素提供多个候选位置，就可以显著降低最大负载 [19]，该思想也影响了后续布谷鸟哈希、多候选桶哈希和局部重排哈希结构。

布谷鸟哈希是开放寻址中具有代表性的多候选位置方案。Pagh 与 Rodler 提出的 Cuckoo Hashing 通过两个或多个候选位置以及踢出过程，使查询只需检查常数个位置 [9]；后续 SicHash 等工作则面向小型不规则布谷鸟表和完美哈希构造，进一步优化静态集合上的空间与构造效率 [10]。最小完美哈希方向主要服务于静态集合，目标是在无冲突查找的同时尽可能接近信息论下界。Lehmann 等人的综述总结了现代最小完美哈希的构造路线、空间开销与应用场景 [11]；ShockHash 尝试在避免暴力搜索的前提下接近最优空间 [12]；PHOBIC 通过优化桶大小和交错编码改进完美哈希构造 [13]；Combined Search and Encoding for Seeds 则将种子搜索与编码结合，用于进一步降低最小完美哈希中的辅助信息 [14]。这些工作说明，静态集合上的空间压缩已经相当成熟，但它们通常不直接解决频繁插入、删除和冷热数据迁移问题。

与完美哈希相邻的是紧凑检索结构、近似成员查询和动态过滤器。Dietzfelbinger 与 Pagh 研究了 succinct retrieval 与 approximate membership 结构，将查找、检索和紧凑编码联系起来 [8]；Jacobson 关于静态树和图的空间高效表示则为位级数据结构提供了早期基础 [26]。Kuszmaul 与 Walzer 给出了 dynamic filters 和 value-dynamic retrieval 的空间下界，说明在支持动态更新时，辅助信息并不能任意压缩 [5]。这些结果强调：哈希结构的空间开销不能只看槽位空闲率，还必须计算查询路由、键恢复、删除维护和动态更新所需的元数据。本文所关注的 cubby local router、MiniArray、metadata 与 tombstone，本质上也属于这类“为动态语义支付少量位级辅助信息”的设计问题。

动态高装载率场景还与球槽分配和删除模型密切相关。Aamand、Knudsen 与 Thorup 研究了动态 balls-and-bins 负载均衡问题 [17]，Bansal 与 Kuszmaul 进一步讨论了 heavily loaded 情况下带删除的 balanced allocations [18]。这些工作揭示，在表接近满载且删除持续发生时，元素搬移和局部平衡不变量会成为系统成本的重要来源。Conway、Farach-Colton 与 Kuszmaul 从动态最优性的角度统一分析 linear probing 与 Robin Hood hashing [27]，说明不同重排策略背后都在处理“查询代价、更新搬移和空间利用率”之间的权衡。本文 V1 中 tail 回填、tier 合并/拆分和 k-kick 踢出链，与这些动态负载均衡问题具有相同的核心矛盾：为了维持高空间利用率，删除和插入路径必须承担一定结构维护成本。

Bender 等人在 *On the Optimal Time/Space Tradeoff for Hash Tables* 中给出了本文原始复现系统的主要理论来源 [4]。该工作将开放寻址哈希的查询复杂度与元素探测位置联系起来，提出在查询保持常数时间的前提下，允许插入和删除承担 $O(k)$ 次元素交换，从而把每键冗余空间降到 $O(\log^{(k)} n)$ 量级。其核心构造包括 facility/cubby 分层、k-kick 插入、local query router、MiniArray 以及 quotient-style 键恢复。MiniArray 的思想与 B-tree 结构、Rank/Select、packed leaf 等位级维护技术相关 [24,26]，Local Query Router 可借鉴 Patricia 前缀压缩降低路径冗余 [29]。Iceberg Hashing 和 IcebergHT 则从高性能哈希表角度强调稳定性、低关联度、多指标同时优化和工程吞吐 [6,7]，与 Bender 等人的理论结构共同构成本文 V1 方案的直接相关背景。

在工程化紧凑哈希表方面，研究者越来越重视空间、吞吐、构造时间、缓存行为和实现复杂度之间的折中。PaCHash 将 packed and compressed 的思想用于哈希表设计，通过紧凑布局和压缩编码降低每键开销 [15]；IcebergHT 在 SIGMOD 场景中强调低关联度与稳定性，以适应高性能数据库和系统负载 [7]。这些系统表明，理论上的渐进空间优势若要在真实机器上体现，还必须面对缓存行、分支、批量构建、更新路径和统计口径等工程因素。本文的 V3 方案延续这一工程化思路：不再只复现纯内存多层 cubby，而是在统一接口下引入 SQLite 冷存储和 hot-tail 内存缓存，使结构能够讨论持久化、冷热分层和近期局部性。

国内相关工作更多从具体系统场景出发，关注键值数据库、GPU 显存管理和位图辅助结构等工程问题。王楠、吴云针对持久化键值数据库提出自适应热点感知哈希索引，强调热点数据识别与持久化场景下的索引优化 [3]；熊轶翔等研究支持分页显存的高性能哈希表索引系统，面向 GPU 环境中的大规模并发访问和显存分页管理 [16]；王天宇等提出基于位图的键值存储哈希优化，利用位图辅助减少查找和状态维护成本 [28]。这些国内工作虽然未必采用 Bender 等人的 cubby 理论模型，但它们与本文关注的问题一致：在真实系统环境下，哈希结构必须同时考虑热点访问、存储介质、元数据开销和可扩展性。

除纯哈希结构外，持久化键值存储还需要考虑底层数据库索引。SQLite 使用 B-tree 管理表和主键索引，具备成熟的事务、WAL 和文件持久化能力 [31]；但与内存哈希相比，单次查询、插入和删除通常需要更长路径和更多工程开销。因此，直接把所有键都放入 SQLite 可以获得可靠落盘，却可能损失近期热数据访问效率。另一方面，真实 workload 往往具有时间局部性和访问偏斜，Zipf 分布长期被用于建模“少数高 rank 数据被频繁访问”的现象 [32]。本文 V3 方案正是基于这一观察，将近期插入的数据保留在 hot tail/frozen tail 中，将较冷数据批量写入 SQLite，从而在持久化能力和热路径性能之间取得折中。

综上，国内外研究大体形成了几条路线：第一，传统开放寻址、布谷鸟哈希和随机化哈希关注查询路径与冲突控制 [9,19-23]；第二，最小完美哈希、紧凑检索和位级数据结构关注静态或半静态场景下的极低空间开销 [8,11-15,26]；第三，动态 balls-and-bins、Robin Hood/linear probing 与最优时间空间权衡研究关注高装载率下的动态更新和元素搬移 [4,5,17,18,27]；第四，IcebergHT、PaCHash 及国内键值数据库/GPU/位图哈希工作更强调系统实现和工程指标 [3,6,7,15,16,28]。现有工作为本文提供了充分理论和工程背景，但多数方案要么偏静态压缩，要么偏纯内存动态结构，要么偏持久化索引本身。本文的定位是在原始 cubby 哈希复现基础上进行结构性改造：保留 hot tail cubby 的近期内存访问优势，用 per-facility SQLite 承接冷数据，并通过 frozen tail 与 tombstone 机制降低删除和批量落盘之间的耦合。

## 1.4 主要工作与创新点

本文主要工作如下：

1. 实现了基于 facility、tail cubby、多层 cubby、local router 与 k-kick 插入的 V1 原始内存哈希系统。
2. 实现了按 facility 分表的 V2 纯 SQLite 对照方案，作为持久化 baseline。
3. 设计并实现了 V3 hot-tail + SQLite 混合方案，将近期数据保留在内存 hot tail/frozen tail 中，将冷数据批量写入 SQLite。
4. 设计 frozen tail 队列与 tombstone 删除机制，避免 tail 轮换后对冻结 cubby 做复杂原地修改。
5. 建立统一实验入口，使用相同 key 集合、相同 Zipf 查询序列、相同删除目标和相同指标统计方式，对 V1、V2、V3 进行对比。

本文创新点可概括为：将原始多层 cold cubby 的维护路径替换为 per-facility SQLite 冷存储，同时保留 hot tail cubby 对近期数据的常数级访问能力；通过 frozen tail 和 tombstone 机制，将热缓存轮换、批量落盘和删除语义解耦，从而形成面向近期局部性的混合哈希存储结构。

## 1.5 论文组织结构

第二章介绍动态哈希、k-kick、局部路由器、SQLite 与时间局部性等相关技术。第三章描述 V1 原始多层 cubby 哈希系统。第四章介绍 V2 纯 SQLite 方案与 V3 混合优化方案。第五章给出三种方案的复杂度分析。第六章说明系统实现与关键模块。第七章介绍实验设计、指标统计和实验结果。第八章总结全文并讨论后续改进方向。

# 第二章 相关理论与技术

## 2.1 动态哈希与高装载率存储

动态哈希结构需要在插入、查询、删除和扩容过程中保持较低的平均访问成本。设系统当前逻辑容量为 $N$，键集合规模为 $n$，装载率为 $\alpha$，则有：

$$
\alpha = \frac{n}{N}
\tag{2-1}
$$

高装载率能够降低空间浪费，但也会增加冲突处理和维护成本。因此系统需要在空间利用率与操作延迟之间折中。

## 2.2 Facility 与 Cubby 分层思想

原始系统将逻辑地址空间划分为多个 facility，每个 facility 维护若干 cubby。给定键 $x$，系统先通过可逆置换 $\pi$ 将其映射到伪随机空间，再根据低位确定 facility 和 facility 内位置：

$$
g(x) = \pi(x) \bmod N
\tag{2-2}
$$

$$
r(x) = \left\lfloor \frac{g(x)}{K} \right\rfloor,\qquad
b(x) = g(x) \bmod K
\tag{2-3}
$$

其中，$K$ 表示每个 facility 的逻辑大小，$r(x)$ 为 facility 下标，$b(x)$ 为 facility 内 bucket 下标。该设计将全局哈希问题拆分为多个规模较小的局部存储问题，有利于降低单个路由结构的复杂度。

## 2.3 k-kick 插入思想

k-kick 机制将一个 cubby 逻辑划分为若干层级 bin。设 cubby 容量为 $I$，第 $d$ 层 bin 大小为 $s_d$，则可写为：

$$
s_d = \Theta\left((\log^{(d)} I)^6\right),\quad d=0,1,\dots,k
\tag{2-4}
$$

键 $x$ 在 cubby 内具有从粗到细的偏好 bin 序列：

$$
G(x)=\left(g_0(x),g_1(x),\dots,g_k(x)\right)
\tag{2-5}
$$

插入时优先尝试较深层位置，若目标 bin 已满，则踢出插入深度较浅的元素并递归向上安置。由于 $k$ 为常数，单次插入的踢出链长度在设计上受到控制。

## 2.4 局部路由器与元数据恢复

由于 k-kick 会改变元素在 cubby 中的真实槽位，仅凭哈希首选位置无法完成常数时间查询。系统为 cubby 维护 local query router，用于记录键到实际槽位或探测序号的映射。同时，cubby 中不直接保存完整键，而是保存 quotient payload 与元数据。若元素位于槽位 $pos$，其局部偏移可表示为：

$$
\Delta(x) = g_I(x) - pos
\tag{2-6}
$$

其中 $g_I(x)$ 表示键 $x$ 在容量为 $I$ 的 cubby 内的局部哈希位置。通过 $\Delta(x)$、facility 下标和哈希位片段，系统可以恢复或校验 $\pi(x)$，从而避免错误命中。

## 2.5 SQLite 与 B-tree 冷存储

SQLite 使用 B-tree 管理表中主键。若某个 facility 对应 SQLite 表中有 $m_r$ 个键，则典型查询、插入和删除成本可近似表示为：

$$
T_{\mathrm{sqlite}}(m_r)=O(\log m_r)
\tag{2-7}
$$

SQLite 的优势是持久化能力强、工程稳定性高；不足是对近期热数据的单次访问成本通常高于内存结构。因此 V3 仅将冷数据放入 SQLite，近期数据优先保留在 hot tail/frozen tail 中。

## 2.6 近期局部性

近期局部性指最近写入或最近访问的数据更可能在短时间内再次被访问。本文实验使用 Zipf 分布建模这一现象。设 rank 越小表示越新的键，则查询权重为：

$$
P(rank=i)=\frac{(i+1)^{-\theta}}{\sum_{j=0}^{n-1}(j+1)^{-\theta}},
\quad \theta=1.1
\tag{2-8}
$$

该 workload 能够检验 V3 是否有效利用 hot tail 缓存近期数据。

# 第三章 V1 原始多层 Cubby 哈希系统

## 3.1 总体结构

V1 是原始内存哈希结构的实现。系统由 `HashTable` 管理全局表，每个 active table 包含多个 facility。每个 facility 包含 facility 级 router、一个当前 tail cubby 和若干 tiered cubby。每个 cubby 内部包含存储槽位、local router、metadata mini-array、free slot map 与 k-kick 几何信息。

V1 的核心数据路径为：原始键经过 Feistel 置换得到 $\pi(x)$，再由式（2-2）和式（2-3）定位 facility 与 bucket。facility 级 router 给出候选 cubby 和槽位，cubby local router 与 metadata 负责最终校验。

## 3.2 插入流程

V1 插入时首先定位 facility 和 bucket。若 facility router 中已经存在该键，则返回重复插入；否则确保 tail cubby 存在，并调用 k-kick 插入逻辑写入 tail cubby。当 tail cubby 已满时，系统将旧 tail 放入 tier 0，并新建一个 tail cubby。随后根据 tier 数量触发合并或拆分。

```text
function V1Insert(key):
    gx = pi(key)
    r  = facility_index(gx)
    b  = bucket_index(gx)
    if D[r][b].locate(key):
        return inserted = false
    ensure_tail(r)
    ok = kkick_insert(tail[r], key)
    if not ok:
        rotate_tail_to_tier0(r)
        ok = kkick_insert(tail[r], key)
    update_facility_router(key)
    maybe_schedule_rebuild(r)
    return inserted = ok
```

## 3.3 查询流程

V1 查询时先通过 facility router 定位候选位置，再通过 cubby local router 和 metadata 校验目标键。查询路径主要发生在内存中，预期为常数级操作。

## 3.4 删除流程

V1 删除的特殊之处在于：当被删除元素不在 tail cubby 中时，系统可能从 tail cubby 中取一个元素回填到被删除位置，以维持多层 cubby 的结构平衡。该机制保证了原始结构的维护逻辑，但也增加了删除路径的常数开销和 router 更新成本。

## 3.5 V1 的局限

V1 的优势是查询路径短、纯内存访问快；不足是冷数据仍占据内存结构，多层 cubby 的合并、拆分和删除回填会增加实现复杂度。V1 也不直接提供持久化能力，因此难以满足需要可靠落盘的场景。

# 第四章 V2 与 V3 存储方案设计

## 4.1 统一接口设计

为保证三种方案可公平比较，系统定义统一接口 `IKeyStore`。实验程序只依赖该接口，不直接依赖具体实现。

```cpp
class IKeyStore {
public:
    virtual OpResult init(const StoreParams& p) = 0;
    virtual InsertResult insert(uint64_t key) = 0;
    virtual QueryResult query(uint64_t key) const = 0;
    virtual DeleteResult erase(uint64_t key) = 0;
    virtual void drain_background_work() = 0;
    virtual StoreStats stats() const = 0;
};
```

统一接口带来三个好处：第一，三种方案使用同一 workload；第二，延迟计时位置一致；第三，空间统计和正确性验证可以复用。

## 4.2 V2 纯 SQLite 对照方案

V2 是纯 SQLite baseline。它不使用 cubby，也不使用内存 hot tail。为了与 V3 公平比较，V2 同样按照 facility 下标 $r$ 分表：

$$
table(x)=facility_{r(x)}
\tag{4-1}
$$

每个表使用 `key INTEGER PRIMARY KEY` 存储键，插入、查询和删除分别映射为 SQL 的 `INSERT OR IGNORE`、`SELECT` 和 `DELETE`。V2 的作用不是作为创新方案，而是为 V3 的 SQLite 冷存储提供对照基线。

## 4.3 V3 Hot-Tail + SQLite 混合方案

V3 是本文的核心设计。每个 facility 维护一个可写 hot tail cubby、一个 frozen tail 队列和一个 tombstone 集合。冷数据存储在对应的 SQLite `facility_r` 表中。

```cpp
struct PerFacility {
    unique_ptr<Cubby> hot;
    deque<unique_ptr<Cubby>> frozen;
    unordered_set<uint64_t> tombstones;
    mutex mu;
};
```

V3 的 tail 容量按式（4-2）设定：

$$
C_{\mathrm{tail}}=\min\left(K,\max\left(64,\frac{K}{16}\right)\right)
\tag{4-2}
$$

该设置使 hot tail 具有足够容量承接近期数据，同时避免单个 tail 过大导致 flush 粒度过粗。

## 4.4 V3 插入流程

V3 插入时按 hot、frozen、SQLite 的顺序检查重复键。若键不存在，则优先插入 hot tail；当 hot tail 写满或 k-kick 插入失败时，系统将当前 hot tail 转入 frozen 队列，并新建空 hot tail。若 frozen 队列长度超过阈值，则触发 oldest frozen tail flush。

```text
function V3Insert(key):
    gx = pi(key)
    r  = facility_index(gx)
    lock facility[r]
    if key in hot[r]:
        return inserted = false
    if key in frozen[r] and key not in tombstones[r]:
        return inserted = false
    if sqlite_contains(r, key) and key not in tombstones[r]:
        return inserted = false
    tombstones[r].erase(key)
    if not kkick_insert(hot[r], key):
        frozen[r].push_back(move(hot[r]))
        hot[r] = new_tail_cubby()
        flush_oldest_if_needed(r)
        kkick_insert(hot[r], key)
    return inserted = true
```

## 4.5 V3 查询流程

V3 查询顺序为 hot tail、frozen tail、SQLite cold store：

$$
Query_{V3}(x)=
\begin{cases}
\mathrm{hit}, & x\in Hot,\\
\mathrm{hit}, & x\in Frozen \land x\notin Tombstone,\\
\mathrm{hit}, & x\in SQLite \land x\notin Tombstone,\\
\mathrm{miss}, & \text{otherwise}
\end{cases}
\tag{4-3}
$$

由于 frozen 队列长度有上界，hot 与 frozen 查询仍可视为常数级内存路径；只有冷数据查询需要访问 SQLite。

## 4.6 V3 删除流程

V3 删除同样按 hot、frozen、SQLite 的顺序执行。如果键位于 hot tail，则直接清除槽位；如果键位于 frozen tail，则写入 tombstone；如果键已经进入 SQLite，则执行 SQLite 删除。其核心思想是避免 frozen cubby 的原地修改，同时保持后续 flush 的正确性。

## 4.7 Flush 与 Tombstone 机制

当 frozen tail 需要落盘时，系统遍历其中被占用槽位，利用 metadata 恢复原始 key。若 key 在 tombstone 集合中，则说明该键在 flush 前已经被逻辑删除，系统跳过该键并移除 tombstone；否则批量写入 SQLite。

```text
function FlushOldest(r):
    cubby = frozen[r].pop_front()
    keys = []
    for slot in cubby.occupied_slots:
        key = recover_key_from_slot(cubby, slot, r)
        if key in tombstones[r]:
            tombstones[r].erase(key)
            continue
        keys.push_back(key)
    sqlite_bulk_insert(r, keys)
```

# 第五章 复杂度分析

## 5.1 时间复杂度

设某个 facility 的 SQLite 冷数据规模为 $m_r$，V3 中 hot 命中概率为 $p_h$，frozen 命中概率为 $p_f$，冷存储命中概率为 $p_c=1-p_h-p_f$。V3 查询期望成本可写为：

$$
\mathbb{E}[T_{query}^{V3}]
=p_h O(1)+p_f O(1)+p_c O(\log m_r)
\tag{5-1}
$$

当 workload 具有近期局部性时，$p_h+p_f$ 较高，V3 查询平均成本接近内存访问路径。

V3 插入成本由 hot tail k-kick 插入和偶发 flush 组成。设每次 flush 批量写入 $B$ 个键，则平摊写入成本可表示为：

$$
\mathbb{E}[T_{insert}^{V3}]
=O(1)+\frac{1}{B}O(B\log m_r)
\tag{5-2}
$$

实际实现中，批量写入和 WAL checkpoint 的工程开销会影响常数项，但结构上避免了每个新键都直接访问 SQLite。

V3 删除成本为：

$$
T_{delete}^{V3}=
\begin{cases}
O(1), & x\in Hot,\\
O(1), & x\in Frozen,\\
O(\log m_r), & x\in SQLite
\end{cases}
\tag{5-3}
$$

与 V1 相比，V3 删除 hot/frozen 数据时不需要从 tail 回填到其他 cold cubby；与 V2 相比，V3 删除热数据时不需要直接访问 SQLite。

## 5.2 空间复杂度

三种方案的空间来源不同。V1 主要统计 facility router 与 cubby metadata 的逻辑元数据；V2 主要统计 SQLite 主数据库文件；V3 统计 hot/frozen cubby 元数据与 SQLite 主数据库文件。统一的单位键空间占用定义为：

$$
bits\_per\_key =
\frac{mem\_meta\_bits + 8\cdot disk\_file\_bytes}{n}
\tag{5-4}
$$

其中 $n$ 为实验结束后的有效 key 数量。SQLite 的 WAL、SHM 文件和 C++ 容器运行时对象开销不计入该指标，而作为实现边界单独说明。

从渐进复杂度看，三种方案都保持线性空间：

$$
S_{V1}=O(n),\qquad S_{V2}=O(n),\qquad S_{V3}=O(n)
\tag{5-5}
$$

本文的空间优化主要体现在空间组成与常数项：V3 将冷层 cubby 维护替换为 SQLite 冷存储，同时只保留有限数量的 hot/frozen tail 元数据；与 V2 相比，热数据可以暂留内存，不必每次操作都立即进入 SQLite 路径。

## 5.3 复杂度对比

| 方案                 | 插入                           | 查询                                   | 删除                               |
| -------------------- | ------------------------------ | -------------------------------------- | ---------------------------------- |
| V1 多层 cubby        | $O(1)$ 平摊，含 rebuild 常数 | $O(1)$ 内存路由                      | $O(1)$ 平摊，可能 tail 回填      |
| V2 SQLite            | $O(\log m_r)$                | $O(\log m_r)$                        | $O(\log m_r)$                    |
| V3 hot-tail + SQLite | hot 为$O(1)$，flush 平摊     | 热命中$O(1)$，冷命中 $O(\log m_r)$ | 热删$O(1)$，冷删 $O(\log m_r)$ |

# 第六章 系统实现

## 6.1 代码模块

系统实现采用 C++，核心模块如下。

| 模块        | 文件                             | 作用                                  |
| ----------- | -------------------------------- | ------------------------------------- |
| 原始哈希表  | `include/ht.h`, `src/ht.cpp` | V1 核心结构                           |
| 统一接口    | `include/otsh/keystore.h`      | 抽象三种实现                          |
| V1 包装     | `src/v1_store.cpp`             | 将 `HashTable` 包装为 `IKeyStore` |
| V2 SQLite   | `src/v2_sqlite_store.cpp`      | 纯 SQLite 对照实现                    |
| V3 混合方案 | `src/v3_hot_tail_store.cpp`    | hot tail/frozen/SQLite                |
| 参数派生    | `src/system_params.cpp`        | 推导$N$、$K$、tier 参数           |
| 实验入口    | `experiments/ch5_variants.cpp` | 三方案统一实验                        |

## 6.2 参数派生

实验中根据规模提示 $n$ 推导逻辑容量 $N$ 与 facility 大小 $K$：

$$
N = 2^{\lceil \log_2 n\rceil}
\tag{6-1}
$$

$$
K = choose\_K(N)
\tag{6-2}
$$

其中 $K$ 在实现中被限制在固定区间内，并取为 2 的幂，以便位运算和 facility 划分。

## 6.3 正确性保证

实验阶段通过插入数、查询命中数和删除数共同检查正确性。设期望操作总数为：

$$
E = E_{insert}+E_{query}+E_{delete}
\tag{6-3}
$$

实际成功数为：

$$
S = S_{insert}+S_{query}+S_{delete}
\tag{6-4}
$$

正确率定义为：

$$
correctness\_pct = 100\cdot \frac{S}{E}
\tag{6-5}
$$

只有当 `correctness_pct` 为 $100\%$ 时，对应实验组的性能结果才可用于比较。

## 6.4 实现边界说明

为了保证实验可复现和分析清晰，当前实现有如下边界：

1. V3 的 flush 主要由 rotate 背压和 `drain_background_work()` 触发，未依赖不可控的长期后台线程。
2. `bits_per_key` 是逻辑空间与 SQLite 主文件空间口径，不等同于进程 RSS。
3. SQLite 小规模空间占用会受页大小、schema 和 per-table 固定开销影响。
4. 当前 CH5 实验聚焦 CRUD 与存储层差异，暂不展开并发吞吐、P99 延迟和 crash recovery。

# 第七章 实验设计与结果分析

## 7.1 实验目的

实验目标是比较三种方案在相同 workload 下的平均插入延迟、平均查询延迟、平均删除延迟、单位键空间占用和正确性。重点验证 V3 是否能在近期数据更热的场景下，利用 hot tail 和 frozen tail 降低 SQLite 访问次数。

## 7.2 实验设置

实验规模为：

$$
n\in \{5\times 10^3,10^4,5\times 10^4,10^5,5\times 10^5\}
\tag{7-1}
$$

每组实验包括三个阶段：

1. 生成 $n$ 个唯一 64 位 key 并全部插入。
2. 使用 Zipf 分布执行 $n$ 次查询，rank 0 对应最新 key。
3. 删除最新 $20\%$ 的 key。

删除结束后，最终有效 key 数量为：

$$
n_{\mathrm{final}} = 0.8n
\tag{7-2}
$$

`bits_per_key` 使用 $n_{\mathrm{final}}$ 作为分母。

## 7.3 评价指标

插入、查询和删除平均延迟分别定义为：

$$
insert\_avg\_us = \frac{1}{n}\sum_{i=1}^{n} T_{insert}(i)
\tag{7-3}
$$

$$
query\_avg\_us = \frac{1}{n}\sum_{i=1}^{n} T_{query}(i)
\tag{7-4}
$$

$$
delete\_avg\_us = \frac{1}{0.2n}\sum_{i=1}^{0.2n} T_{delete}(i)
\tag{7-5}
$$

空间指标和正确性指标分别使用式（5-4）与式（6-5）。

## 7.4 实验结果示例

下表给出当前单 seed 日志中的一组实验结果。正式论文中建议运行多 seed，并对相同方案和规模取平均值或中位数。

| 规模 | 方案 | 插入/us | 查询/us | 删除/us | bits/key |
| ---- | ---- | ------: | ------: | ------: | -------: |
| 5e3  | V1   |  38.300 |   0.270 | 150.911 |   65.510 |
| 5e3  | V2   |  30.220 |   1.157 |  15.277 |  204.800 |
| 5e3  | V3   |   8.789 |   0.408 |   7.679 |  188.416 |
| 1e4  | V1   |  41.320 |   0.349 | 123.029 |   64.220 |
| 1e4  | V2   |  17.268 |   1.250 |  94.745 |  192.512 |
| 1e4  | V3   |  10.060 |   0.453 |  62.000 |  192.512 |
| 5e4  | V1   |  56.562 |   0.437 | 192.263 |   66.434 |
| 5e4  | V2   |  20.936 |   1.305 |  23.450 |  159.744 |
| 5e4  | V3   |  16.227 |   0.488 |  22.824 |  153.190 |
| 1e5  | V1   |  60.683 |   0.541 | 161.573 |   67.925 |
| 1e5  | V2   |  15.949 |   1.338 |  18.857 |  158.515 |
| 1e5  | V3   |  17.128 |   0.507 |  18.281 |  155.648 |
| 5e5  | V1   |  79.060 |   0.706 | 117.725 |   68.316 |
| 5e5  | V2   |  20.885 |   1.437 |  27.022 |  154.747 |
| 5e5  | V3   |  18.832 |   0.570 |  22.946 |  151.634 |

所有示例实验组的 `correctness_pct` 均为 $100\%$。因此，上述数据可用于性能趋势分析。

## 7.5 结果分析写法建议

查询延迟方面，V2 始终需要访问 SQLite B-tree，因此查询平均延迟随规模增长保持在微秒级以上。V3 在查询时优先访问 hot tail 和 frozen tail，当 workload 更偏向近期 key 时，大量查询可以在内存中完成，因此查询延迟明显低于 V2，并在较大规模下接近或优于 V1。

插入延迟方面，V1 需要维护多层 cubby 和 rebuild 逻辑，插入成本随规模增加而上升。V3 先将新键写入 hot tail，只有 tail 轮换时才批量 flush 到 SQLite，因此在当前 workload 下插入平均延迟低于 V1，并与 V2 保持同一量级。

删除延迟方面，V1 删除冷层元素时可能触发 tail 回填和 router 更新，导致删除平均成本较高。V3 删除 hot key 时直接清除槽位，删除 frozen key 时写 tombstone，删除 cold key 时才访问 SQLite，因此对“删除最新 20% key”的 workload 更有优势。

空间占用方面，V1 的 `bits_per_key` 只统计内存逻辑元数据，不包含持久化文件，因此不能简单与 V2/V3 的 SQLite 主文件空间作绝对优劣判断。V3 与 V2 的口径更接近；在较大规模下，V3 的 `bits_per_key` 低于或接近 V2，说明 hot tail 与批量落盘没有引入不可接受的额外空间成本。

## 7.6 实验公平性

为了避免实验偏向某一种方案，本文采用以下公平性设计：

1. 三种方案使用相同 key 生成逻辑。
2. 三种方案使用相同查询序列和删除目标。
3. V2 与 V3 都按 facility 下标分表，避免 V3 独占分区优势。
4. 每组实验结束前调用 `drain_background_work()`，确保延迟工作收尾后再统计空间。
5. 所有方案使用相同正确性公式，正确率不足 $100\%$ 的结果不纳入性能比较。

# 第八章 总结与展望

## 8.1 全文总结

本文围绕动态键值存储中的高效访问、冷数据持久化和近期局部性利用问题，设计并实现了三种可对比的存储方案。V1 复现并工程化了原始多层 cubby 内存哈希结构，V2 提供按 facility 分表的 SQLite baseline，V3 则提出 hot-tail + SQLite 混合方案。V3 的核心在于：近期数据保留在 hot tail/frozen tail 中，冷数据批量写入 SQLite；删除 frozen 数据时通过 tombstone 保证语义正确；实验通过统一接口和统一 workload 对三种方案进行公平比较。

与单纯复现相比，本文的重写重点不再是“实现一个已有哈希结构”，而是“围绕原始结构设计新的混合存储优化方案，并通过实验说明其适用场景”。这使论文具有更明确的工程设计目标和可讨论的创新点。

## 8.2 不足与展望

后续工作可从以下方面展开：

1. 增加多 seed 实验，报告均值、标准差或中位数。
2. 实现真正的后台 flush 线程，并分析并发读写下的锁竞争。
3. 完善 crash recovery 机制，使 hot tail/frozen tail 状态能够在异常退出后恢复。
4. 将 `bits_per_key` 扩展为逻辑空间、文件空间和进程 RSS 三种口径，进一步区分理论结构与工程实现开销。

# 参考文献

[1] Donald E. Knuth. *The Art of Computer Programming, Volume 3: Sorting and Searching*. Addison-Wesley, 1973.

[2] Michael A. Bender, Martin Farach-Colton, William Kuszmaul, et al. Modern Hashing Made Simple. *Proceedings of the 2024 SIAM Symposium on Simplicity in Algorithms (SOSA 2024)*. SIAM, 2024: 363-373. DOI: 10.1137/1.9781611977936.33.

[3] 王楠, 吴云. 面向持久化键值数据库的自适应热点感知哈希索引. *计算机应用研究*, 2024, 41(1): 226-230. DOI: 10.19734/j.issn.1001-3695.2023.04.0188.

[4] Michael A. Bender, Martin Farach-Colton, William Kuszmaul, et al. On the Optimal Time/Space Tradeoff for Hash Tables. *Proceedings of the 54th Annual ACM SIGACT Symposium on Theory of Computing (STOC 2022)*. ACM, 2022: 1284-1297. DOI: 10.1145/3519935.3519969.

[5] William Kuszmaul and Stefan Walzer. Space Lower Bounds for Dynamic Filters and Value-Dynamic Retrieval. *Proceedings of the 56th Annual ACM Symposium on Theory of Computing (STOC 2024)*. ACM, 2024: 1153-1164. DOI: 10.1145/3618260.3649649.

[6] Michael A. Bender, Alex Conway, Martin Farach-Colton, et al. Iceberg Hashing: Optimizing Many Hash-Table Criteria at Once. *Journal of the ACM*, 2023, 70(6): 40:1-40:51. DOI: 10.1145/3625817.

[7] Prashant Pandey, Michael A. Bender, Alex Conway, et al. IcebergHT: High Performance Hash Tables Through Stability and Low Associativity. *Proceedings of the ACM on Management of Data*, 2023, 1(1): 47:1-47:26. DOI: 10.1145/3588727.

[8] Martin Dietzfelbinger and Rasmus Pagh. Succinct Data Structures for Retrieval and Approximate Membership. *Proceedings of ICALP 2008*, Lecture Notes in Computer Science, vol. 5125. Springer, 2008: 385-396. DOI: 10.1007/11523468_30.

[9] Rasmus Pagh and Flemming Friche Rodler. Cuckoo Hashing. *Journal of Algorithms*, 2004, 51(2): 122-144. DOI: 10.1016/j.jalgor.2004.03.002.

[10] Hans-Peter Lehmann, Peter Sanders, and Stefan Walzer. SicHash: Small Irregular Cuckoo Tables for Perfect Hashing. *Proceedings of ALENEX 2023*. SIAM, 2023: 176-189. DOI: 10.1137/1.9781611977561.ch15.

[11] Hans-Peter Lehmann, Tobias Muller, Rasmus Pagh, et al. Modern Minimal Perfect Hashing: A Survey. *ACM Computing Surveys*, 2026, 58(10): 251:1-251:36. DOI: 10.1145/3797036.

[12] Hans-Peter Lehmann, Peter Sanders, and Stefan Walzer. ShockHash: Towards Optimal-Space Minimal Perfect Hashing Beyond Brute-Force. *Proceedings of ALENEX 2024*. SIAM, 2024: 194-206. DOI: 10.1137/1.9781611977929.15.

[13] Stefan Hermann, Hans-Peter Lehmann, Giulio Ermanno Pibiri, et al. PHOBIC: Perfect Hashing with Optimized Bucket Sizes and Interleaved Coding. *Proceedings of ESA 2024*, LIPIcs, vol. 308. Schloss Dagstuhl - Leibniz-Zentrum fur Informatik, 2024: 69:1-69:17. DOI: 10.4230/LIPIcs.ESA.2024.69.

[14] Hans-Peter Lehmann, Peter Sanders, Stefan Walzer, et al. Combined Search and Encoding for Seeds, with an Application to Minimal Perfect Hashing. *Proceedings of ESA 2025*, LIPIcs, vol. 351. Schloss Dagstuhl - Leibniz-Zentrum fur Informatik, 2025: 109:1-109:18. DOI: 10.4230/LIPIcs.ESA.2025.109.

[15] Florian Kurpicz, Hans-Peter Lehmann, and Peter Sanders. PaCHash: Packed and Compressed Hash Tables. *Proceedings of ALENEX 2023*. SIAM, 2023: 162-175. DOI: 10.1137/1.9781611977561.ch14.

[16] 熊轶翔, 蒋筱斌, 张珩, 等. 支持分页显存的高性能哈希表索引系统. *计算机系统应用*, 2022, 31(9): 82-90. DOI: 10.15888/j.cnki.csa.008664.

[17] Anders Aamand, Jakob Bæk Tejs Knudsen, and Mikkel Thorup. Load Balancing with Dynamic Set of Balls and Bins. *Proceedings of the 53rd Annual ACM SIGACT Symposium on Theory of Computing (STOC 2021)*. ACM, 2021: 1262-1275. DOI: 10.1145/3406325.3451107.

[18] Nikhil Bansal and William Kuszmaul. Balanced Allocations: The Heavily Loaded Case with Deletions. *Proceedings of the 63rd IEEE Annual Symposium on Foundations of Computer Science (FOCS 2022)*. IEEE, 2022: 801-812. DOI: 10.1109/FOCS54457.2022.00081.

[19] Michael Mitzenmacher, Andrea Richa, and Ramesh Sitaraman. The Power of Two Random Choices: A Survey of Techniques and Results. In *Handbook of Randomized Computing*. Springer, 2001: 255-312.

[20] Rasmus Pagh, Anna Pagh, and Milan Ruzic. Linear Probing with Constant Independence. *SIAM Journal on Computing*, 2009, 39(3): 1107-1120. DOI: 10.1137/070702278.

[21] Michael Luby and Charles Rackoff. How to Construct Pseudorandom Permutations from Pseudorandom Functions. *SIAM Journal on Computing*, 1988, 17(2): 373-386. DOI: 10.1137/0217022.

[22] J. Lawrence Carter and Mark N. Wegman. Universal Classes of Hash Functions. *Journal of Computer and System Sciences*, 1979, 18(2): 143-154. DOI: 10.1016/0022-0000(79)90044-8.

[23] Mikkel Thorup and Yin Zhang. Tabulation-Based 5-Independent Hashing with Applications to Linear Probing and Second Moment Estimation. *SIAM Journal on Computing*, 2012, 41(2): 293-331. DOI: 10.1137/100800774.

[24] Rudolf Bayer and Edward M. McCreight. Organization and Maintenance of Large Ordered Indexes. *Acta Informatica*, 1972, 1(1): 173-189. DOI: 10.1007/BF00288683.

[25] Rasmus Pagh. Hashing with Limited Independence. *Proceedings of the 12th Annual ACM-SIAM Symposium on Discrete Algorithms (SODA 2001)*. SIAM, 2001: 830-839.

[26] Guy Jacobson. Space-efficient Static Trees and Graphs. *Proceedings of the 30th Annual Symposium on Foundations of Computer Science (FOCS 1989)*. IEEE, 1989: 549-554. DOI: 10.1109/SFCS.1989.63533.

[27] Alex Conway, Martin Farach-Colton, and William Kuszmaul. A Unified Approach to Dynamic Optimality for Linear Probing and Robin Hood Hashing. *Proceedings of ESA 2018*, LIPIcs, vol. 112. Schloss Dagstuhl - Leibniz-Zentrum fur Informatik, 2018: 29:1-29:14. DOI: 10.4230/LIPIcs.ESA.2018.29.

[28] 王天宇, 徐云, 王彪. 基于位图的键值存储哈希优化. *计算机应用研究*, 2023, 40(7): 2106-2110. [http://www.c-s-a.org.cn/1003-3254/8664.html](http://www.c-s-a.org.cn/1003-3254/8664.html).

[29] Donald R. Morrison. PATRICIA: Practical Algorithm To Retrieve Information Coded in Alphanumeric. *Journal of the ACM*, 1968, 15(4): 514-534. DOI: 10.1145/321066.321078.

[30] Thomas H. Cormen, Charles E. Leiserson, Ronald L. Rivest, and Clifford Stein. *Introduction to Algorithms*. MIT Press, 2009.

[31] SQLite Documentation. *SQLite Database Engine*. [https://www.sqlite.org/docs.html](https://www.sqlite.org/docs.html).

[32] George K. Zipf. *Human Behavior and the Principle of Least Effort*. Addison-Wesley, 1949.
