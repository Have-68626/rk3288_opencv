#pragma once

#include "Compat.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>

namespace rk_win {

// 轮转档位的**代数收缩清理**：删除形如 `<current>.<N>`（N 全为十进制数字）且 `N > maxRollFiles`
// 的既有档位，返回实际删除个数。
//
// ── 为什么需要它 ──
// 三个日志器（StructuredLogger / EventLogger / RenderMetricsLogger）的轮转循环都只做
// `path.(i) → path.(i+1)` 的**迁移**，起点是 `maxRollFiles - 1`。于是 `maxRollFiles` 一旦被
// 运维从大调小（例如 20 → 3），索引 4…20 的既有档位**再也不会被任何一次轮转触及** ⇒ 永久滞留
// 在磁盘上。后果是「maxFileBytes × (maxRollFiles + 1)」这条**预算记账在收缩之后不再成立**
// （历史档位继续占盘直到人工删除），而该记账正是 `WinConfig.h` 的 `kLogPerSetBudgetBytes` 与
// `clampLogMaxFileBytes` / `clampLogMaxRollFiles` 的推导依据。
//
// ── 为什么在 `open()` 里调用，而不是在轮转里 ──
// 收缩只可能发生在「两次 open 之间」——`maxRollFiles_` 在一次 open 的生命周期内是常量。
// 在 open 时扫一次目录即可覆盖全部收缩场景，且**无需**在每次轮转（可能很频繁）时做目录扫描。
// 因此本函数的调用点固定为各日志器 `open()` 内、创建目录之后、打开文件之前。
//
// ── 边界与拒绝策略（逐条都是「不删」的理由）──
// 1. `maxRollFiles <= 0` ⇒ 轮转被**显式关闭**（`0` = 不限，三处语义一致）⇒ 直接返回 0、不删任何
//    既有档位：此时既有档位不是本类产物，删除属破坏性操作，且用户关轮转的意图恰恰是「不要动我的文件」。
// 2. 后缀必须是**纯十进制数字**：`recognition.csv.bak`、`recognition.csv.corrupt.1737…`、
//    `recognition.csv.tmp` 这类同名干扰项一律跳过（对它们调 remove 会误删别的功能的数据）。
// 3. 不删 `N <= maxRollFiles` 的档位（那是轮转循环自己管理的范围），也不删 `current` 自身
//    （`current` 的文件名不含 `.N` 后缀，天然不匹配前缀判据）。
// 4. 逐项用 `error_code` 重载，**不抛异常**：轮转是尽力而为，清理失败不得影响日志打开
//    （与 `renameBestEffort` 吞错同一口径）。删除失败的项不计入返回值，但也不中断其余项。
// 5. 目录不可遍历（不存在 / 无权限）⇒ 返回 0，不报错。
// 6. **符号链接一律跳过**（`is_symlink()` 优先判定，用不跟随链接的 `symlink_status`）。
//    ⚠️ 这条不能只靠 `is_regular_file()` 实现——它走 `status()`，会**跟随**链接：一个指向普通
//    文件的链接会被判为 regular，随即被 `remove`（删掉的是链接本身，不是目标）。跳过链接是刻意的
//    保守取舍：本函数的职责是清理**自己生成**的档位，没有理由动别人建的链接。
// 7. 唯一可能抛出的位置是文件名从宽字符到窄字符的转换（见实现里的 `try/catch`）：MSVC 的
//    `path::string()` 在文件名含当前 ANSI 代码页无法表示的字符时抛 `std::system_error`——
//    而本函数是该目录**任意第三方文件名**的唯一接触点，故该处单独兜住（跳过该项）。除它以外，
//    本函数全程不抛。
inline int pruneShrunkGenerations(const std::filesystem::path& current, int maxRollFiles) {
    if (maxRollFiles <= 0) return 0;  // 见边界 1

    const std::string base = current.filename().string();
    if (base.empty()) return 0;
    const std::string prefix = base + ".";
    const std::filesystem::path dir = current.parent_path();

    std::error_code itEc;
    std::filesystem::directory_iterator it(dir.empty() ? std::filesystem::path(".") : dir, itEc);
    if (itEc) return 0;  // 见边界 5

    int removed = 0;
    const std::filesystem::directory_iterator end;
    for (; it != end; it.increment(itEc)) {
        if (itEc) break;
        std::error_code typeEc;
        const std::filesystem::path entry = it->path();
        // 先判链接（symlink_status，不跟随），再判常规文件：详见边界 6。
        if (it->is_symlink(typeEc)) continue;
        if (!it->is_regular_file(typeEc) || typeEc) continue;
        // 见边界 7：宽→窄转换在该目录含当前 ACP 无法表示的文件名时会抛，此处单独兜住并跳过该项。
        std::string name;
        try {
            name = entry.filename().string();
        } catch (const std::system_error&) {
            continue;
        }
        if (name.size() <= prefix.size()) continue;                                    // 见边界 3
        if (name.compare(0, prefix.size(), prefix) != 0) continue;                     // 前缀不符
        const std::string suffix = name.substr(prefix.size());
        if (suffix.empty()) continue;
        if (suffix.find_first_not_of("0123456789") != std::string::npos) continue;     // 见边界 2

        // 后缀可达 10+ 位（人为造出的超大档位号），std::stoi 会抛 out_of_range ⇒ 手动截断到
        // 溢出前即视为「远超 maxRollFiles、应当删除」，而不是抛异常中断整轮清理。
        int gen = 0;
        bool overflow = false;
        for (char c : suffix) {
            const int digit = c - '0';
            if (gen > (1000000 - digit) / 10) {
                overflow = true;
                break;
            }
            gen = gen * 10 + digit;
        }
        if (!overflow) {
            if (gen <= maxRollFiles) continue;  // 见边界 3
        }
        // overflow ⇒ gen 必然远大于 maxRollFiles（后者来自 schema，上界 1000）⇒ 落入删除分支。

        std::error_code rmEc;
        std::filesystem::remove(entry, rmEc);
        if (!rmEc) ++removed;  // 见边界 4
    }
    return removed;
}

}  // namespace rk_win
