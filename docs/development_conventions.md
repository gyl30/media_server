# media_server 开发格式与提交约定

> 整理日期：2026-10-13
> 适用范围：`media_server` 第一方代码与 Git 提交。不要将 `chat` 项目规则混入。
> 性质：从历史对话中已确认的约定整理而成；不是一次新架构设计。架构与生命周期取舍见版本化 [AGENTS.md](../AGENTS.md) 路由到的 `.agent/` 文档。

## 1. 使用原则

- 开发前阅读版本化 [AGENTS.md](../AGENTS.md)，按入口阅读 `.agent/` 中相关文档及本文；实际修改还应对照当前源码与 `.clang-format`。
- 本文只规定**提交信息与代码组织格式**，不覆盖当前任务对修改、提交、推送权限的要求。
- 以当前明确任务和后续决定覆盖早期示例。根据实际 diff 判断标题是否具体，不把“修复、精简、收口、记录、明确”等词作为禁词。
- 不为贯彻格式而全仓批量改动无关文件、破坏已有语义或碰触 `third/**`。

## 2. Git 提交信息

### 2.1 已确认的最终规范

1. **提交标题使用简体中文，简洁且具体，优先动宾结构**；协议名、类名、函数名、C++ 标识符保留原文。标题尽量不超过 50 个字符，最多 72 个字符，末尾不加句号。
2. **一个提交对应一个完整逻辑变化**，而非强制一个文件、一个函数或固定行数。实现与相应的测试可以处于同一逻辑提交；不同目标不为了省事混合提交，也不为了凑数量拆分。
3. **标题表达代码最终的行为、语义或结构结果**，让读者不看过程也能理解这个提交改变了什么。
4. 避免 `优化代码`、`完善功能`、`fix bug` 等不能说明具体变化的标题。是否使用“修复、精简、收口、记录”等词取决于实际内容；文档提交使用“记录”可以合理。
5. **不要求** Conventional Commits 前缀（`feat:`、`fix:` 等）、固定正文模板、固定 footer 或必须列出测试结果。复杂修改可以用正文说明原因、约束和验证情况；标题与正文间留一个空行，正文不重复代码已清楚表达的细节。
6. 只提交当前已获授权的逻辑范围。**是否自动 commit/push 取决于本次任务指令**；过去某次允许自动推送，不等于以后可以默认推送。尤其不得未经明确授权擅自改写历史或 force-push。

### 2.2 符合约定的标题示例

以下示例描述明确的代码或文档变化：

- `修复 RTSP 拉流迟到 PLAY 回调空指针`
- `隔离并行测试的媒体端口资源`
- `统一自有C++代码排版`
- `统一自有头文件包含顺序`
- `记录RTSP播放V2生命周期时序验证结论`

对比：

| 不推荐 | 更清楚的表达方向 |
| --- | --- |
| `修正 RTSP 问题` | 明确指出 RTSP 的哪个行为发生变化 |
| `收口生命周期` | 说明哪个会话/对象现在由谁关闭 |
| `优化代码` | 说明删除了哪种重复工作或实际降低了什么成本 |
| `fix bug` | 中文描述具体缺陷的最终修复语义 |

### 2.3 提交前检查

- 阅读目标文件近期 `git log`，确认没有重复实现既有决定。
- 检查 `git status`、`git diff --check`、暂存 diff，确认提交内容与标题相符、没有不相关文件。
- 不用 `git add -A` 误加入本地 Agent 资料、实验产物或其他用户工作；按任务范围精确暂存。
- 执行与修改范围相符的验证，检查通过后提交。未执行的验证和已知失败在交付报告中如实说明；不要求将原始测试输出写入 commit message。
- 历史改写不是普通提交操作；仅在用户明确授权后执行，并验证代码树未发生意外变化。必要的 `--force-with-lease` 也必须有单独授权。

## 3. C++ 头文件保护宏与命名

1. 使用传统包含保护宏：`#ifndef`、`#define`、末尾 `#endif`。**不使用 `#pragma once`**。
2. 宏名按仓库相对路径与文件名转换为**全大写下划线**，例如：
   - `media/net/tcp_transport.h` → `MEDIA_NET_TCP_TRANSPORT_H`
   - `media/rtsp/rtsp_pull_session.h` → `MEDIA_RTSP_RTSP_PULL_SESSION_H`
3. 类、函数、字段、参数遵守项目既有 **snake_case** 约定；不要从其他项目引入 PascalCase API 风格。
4. namespace 使用仓库形式：

   ```cpp
   namespace media_server
   {
   // declarations
   }    // namespace media_server
   ```

5. 避免无必要的显式拷贝/移动构造、赋值删除声明和模板化样板代码；只有实际所有权约束确实需要时才声明。
6. 用于承载简单配置/数据的 `struct` 以字段为主，不无端给数据类型加入行为或额外层次；已有复杂类型按其实际语义处理，不做全仓机械改写。

## 4. include 的最终排序规则

这是用户在 **2026-09-16** 明确，并在 **2026-09-29** 再次确认的规则：

1. **标准库/系统头文件**。
2. **第三方库头文件**（Boost 等）。
3. **项目自身头文件**。
4. **`extern "C"` 中的 C 头文件**（需要时）。

各组之间保留一个空行。**每个组内部按完整 include 路径的字符长度由短到长**，而非按字母序。长度只计算 `<>` 或 `""` 内的内容，不计算 `#include` 等前缀；同长度尽量保持原有相对顺序，无需额外按字母排序。

```cpp
#include <span>
#include <memory>
#include <string>
#include <cstdint>

#include <boost/asio/spawn.hpp>
#include <boost/asio/ip/tcp.hpp>

#include "media/net/idle_timer.h"
#include "media/net/tcp_transport.h"

extern "C"
{
#include "rtsp-client.h"
}
```

补充：

- **已撤销**“`.cpp` 中必须把自己的 `.h` 放第一行”的建议；不要恢复。
- **`.clang-format` 的 `SortIncludes: false` 是刻意设置的**；include 手工维护，不让格式化工具重新按字母序排序。
- 头文件必须自行声明直接使用类型所需的依赖；不依赖偶然的传递 include。可用前向声明的场合遵循现有代码风格，但不得因追求减少 include 造成不完整类型错误。
- 不要求每个文件都有全部四组；没有使用 C 头文件就不创建空 `extern "C"` 组。
- 条件编译、平台依赖和 feature-test 宏的语义优先于排序。例如 `_GNU_SOURCE` 必须位于系统头之前；不能为排序跨越有效的条件分支或改变 C linkage。
- 本轮整理不新增或删除 include。真正缺失的直接依赖应单独记录、验证，不混入纯排序提交。
- 日常修改不顺带整理无关文件；全仓规范化只有在任务明确授权时执行。

## 5. 类成员的组织顺序

这是 **2026-09-16～09-18** 已确认的最终组织方式：

1. 构造函数、析构函数独立为一组。
2. 对外 `public` API 独立一组；确有必要时将接口实现、访问器等细分为独立 `public:` 组。
3. `private` 中按实际职责将静态/外部回调、运行/协程、协议处理、timer/event、cleanup（例如 `safe_shutdown()`）分组。
4. **私有成员函数和成员变量使用不同的 `private:` 段**；数据成员集中在独立的末尾 `private:` 组。
5. 允许重复 `public:` / `private:` 来表达真实的功能分组；**仅有一种成员时不为凑格式重复访问标签**，也不为没有实际职责的组增加空段。
6. 成员变量的**声明顺序**应考虑初始化依赖和生命周期；构造函数初始化列表顺序必须与声明顺序一致。变更声明顺序可能改变析构顺序，不可只为外观重排。
7. 遵循项目既有 `startup()` / `shutdown()` / `safe_shutdown()` 语义；组织头文件不意味着重新设计会话生命周期。

### 5.1 示意布局（说明分组，不要求创建此类）

```cpp
#ifndef MEDIA_EXAMPLE_DEMO_SESSION_H
#define MEDIA_EXAMPLE_DEMO_SESSION_H

#include <memory>
#include <string>
#include <cstdint>

#include <boost/asio/spawn.hpp>

#include "media/net/tcp_transport.h"

extern "C"
{
#include "rtsp-client.h"
}

namespace media_server
{

class worker_context;

class demo_session final
{
   public:
    explicit demo_session(worker_context& worker);
    ~demo_session();

   public:
    void startup();
    void shutdown();

   private:
    static int on_message(void* param, int code);

   private:
    void run(boost::asio::yield_context yield);

   private:
    void safe_shutdown();

   private:
    worker_context& worker_;
    std::shared_ptr<tcp_transport> transport_;
    std::string stream_id_;
    std::uint16_t port_{};
    rtsp_client_t* client_{};
};

}    // namespace media_server

#endif
```

实际文件以其职责决定分组，**不要照抄示意类、不创建无意义的函数或字段**。以上是长期规范，不授权格式化任务改变命名、guard、类布局、成员顺序或访问权限。

## 6. clang-format、注释与修改范围

仓库 `.clang-format` 是唯一格式权威，不用命令行覆盖其规则，也不修改 `.clang-tidy` 来迁就排版。本轮使用 Ubuntu clang-format 20.1.8，能读取全部现有配置。

当前配置：

```yaml
---
Language:        Cpp
BasedOnStyle:  Google
InsertBraces: true
FixNamespaceComments: true
AllowShortBlocksOnASingleLine: false
AllowShortIfStatementsOnASingleLine: false
SortIncludes:    false
IndentWidth:     4
PointerAlignment: Left
DerivePointerAlignment: false
BreakBeforeBraces: Allman
SortUsingDeclarations: false
AllowAllArgumentsOnNextLine: true
SpacesBeforeTrailingComments: 4
BinPackParameters: false
BinPackArguments: false
BraceWrapping:
  AfterCaseLabel:  true

ColumnLimit: 150
```

Google 基础风格、4 空格缩进、Allman 花括号、150 列和左侧指针符号均由该文件决定。`SortIncludes: false`、`SortUsingDeclarations: false` 保持原顺序；include 另外按第 4 节整理。

```bash
clang-format --version
clang-format --style=file -i <已确认的第一方文件>
clang-format --style=file --dry-run --Werror <已确认的第一方文件>
git diff --check
```

- 日常只格式化实际修改的第一方文件；本轮全仓范围是明确授权的专项，不含 `third/**`、vendor、generated、build、Go 或其他 worktree。
- `InsertBraces: true` 可能新增非空白 token；必须核对作用域、`else` 绑定和执行顺序。`FixNamespaceComments: true` 可能补 namespace 注释。不能把这些变化当作纯空白差异直接接受。
- 二次格式化应稳定。格式化与 include 排序分开审查和提交，检查 include 数量、内容、C linkage 以及其他 token 是否意外变化。
- 只对不自明的逻辑写必要注释，默认使用简体中文；不要顺手改注释内容或给无关代码添加注释。
- 不改 `third/**`、外部依赖或其 Git 状态；使用独立构建目录，避免覆盖运行中的程序。
- 不为统一访问标签改变权限，不为形式化分组引入新抽象、字段或包装层。

## 7. 历史依据与优先级

| 时间 / 依据 | 已确认事项 |
| --- | --- |
| 2026-08-05～08-08 历史讨论 | 全大写 include guard、不用 `#pragma once`；snake_case；简化 `struct` 与拷贝/移动样板代码 |
| 2026-09-03～09-07 历史讨论 | 中文短提交标题、单一逻辑变化、描述最终行为；避免无具体内容的模板化标题 |
| 2026-09-16 历史讨论与提交 `774ef15` | `private` 函数与数据独立分组；include 四类分组、组内按路径长度排列、`SortIncludes: false` |
| 2026-09-18 历史讨论 | 构造析构、公开 API、callback、协程、协议、timer、cleanup、成员变量按职责分组；初始化次序与声明次序一致 |
| 2026-09-29 历史确认 | 不再规定 `.cpp` 自有头必须置顶；不依赖传递 include；同长度保持原序 |
| 2026-10-13 本轮明确要求 | include 按路径长度排序；不把特定动词列为禁词；不改写已发布历史，不影响 Pi 的固定 SHA |
| 当前仓库 `.clang-format`、`media/rtsp/rtsp_pull_session.h` | 现行格式与布局的实物参考；具体文件可能有早期遗留差异 |

**若新任务与历史格式决定冲突：**先指出冲突及原因，以用户当前明确要求为准；未经确认不要借题全仓重新格式化。

## 类布局约定

普通 `class` 使用固定布局顺序：

1. 构造函数 / 析构函数
2. `public` 方法
3. `private` 方法
4. `private` 成员变量

要求：

- 普通 `class` 不定义 `public` 成员变量。
- 成员变量统一放在类末尾的独立 `private:` 区域。
- 不为了形式上的封装引入无意义的 getter/setter；但对外可见状态应通过方法表达，而不是直接暴露成员。
- 如需简单数据聚合，优先使用 `struct`，允许 `public` 字段；`struct` 仅用于配置、参数、返回值等无行为或弱行为对象。
- 无明确需求时，不主动使用 `protected`。
