# Emoji 总入口与 26 类候选

仅使用 **`oj` 统一前缀**：`oj` 浏览总览，继续输入 `a`–`z` 选择分类。不是为
每个 Emoji 分配独立编码，也不占用 `of`、`qo` 等其他空码。

## 目录范围与来源

- 从 20 个扩充为 **1384 个不重复 Emoji**，保留原来 20 个常用表情在总览开头
  的顺序；全部候选同时被分配到 26 类之一。
- 数据源：[Unicode Emoji 16.0 emoji-test.txt](https://www.unicode.org/Public/emoji/16.0/emoji-test.txt)。
- 上游文件 SHA-256：`24f0c534e86cf142e2496953e8f0e46a3e702392911eddcd29c6cced85139697`。
- 采用 fully-qualified 的单个基字形，以及带 U+FE0F Emoji 变体选择符的形式，
  包括常见笑脸、手势、爱心、动物、食物、交通、工具和符号。
- 暂不收入需要复杂整形的肤色组合、ZWJ 家庭/职业、双区域字母国旗、组合键帽
  等 2397 个序列。不是声称包含 Unicode 的所有 Emoji。基本旗帜如 🏁、🚩、🏳️
  仍在目录中。字形是否完整还取决于已安装的字体版本。
- 原始 Unicode 数据按 Unicode License V3 使用，许可随
  `src/assets/UNICODE-LICENSE.txt` 安装。

## 分类速查

候选窗标题会显示类别名；数字始终选择当前页的 Emoji，不会提交类别标题。

| 编码 | 分类 | 数量 | 示例 |
| --- | --- | ---: | --- |
| `oj` | 常用总览 | 1384 | 😀 😃 😄 😁 😆 |
| `oja` | 笑脸开心 | 53 | 😀 😃 😄 😁 😆 |
| `ojb` | 情绪表情 | 63 | 🤐 🤨 😐 😑 😶 |
| `ojc` | 爱心亲密 | 45 | 🥰 😍 🤩 😘 😗 |
| `ojd` | 手势身体 | 61 | 👋 🤚 🖐️ ✋ 🖖 |
| `oje` | 人物身份 | 79 | 👶 🧒 👦 👧 🧑 |
| `ojf` | 动物昆虫 | 125 | 🐵 🐒 🦍 🦧 🐶 |
| `ojg` | 花草植物 | 29 | 💐 🌸 💮 🪷 🏵️ |
| `ojh` | 天气天象 | 47 | 🌑 🌒 🌓 🌔 🌕 |
| `oji` | 水果蔬菜 | 37 | 🍇 🍈 🍉 🍊 🍋 |
| `ojj` | 餐食甜点 | 65 | 🍞 🥐 🥖 🫓 🥨 |
| `ojk` | 饮品餐具 | 27 | 🍼 🥛 ☕ 🫖 🍵 |
| `ojl` | 运动奖牌 | 50 | 🤺 🏇 ⛷️ 🏂 🏌️ |
| `ojm` | 游戏娱乐 | 24 | 🎯 🪀 🪁 🔫 🎱 |
| `ojn` | 音乐艺术 | 53 | 🎭 🖼️ 🎨 🧵 🪡 |
| `ojo` | 交通车辆 | 72 | 🚂 🚃 🚄 🚅 🚆 |
| `ojp` | 旅行建筑 | 68 | 🌍 🌎 🌏 🌐 🗺️ |
| `ojq` | 科技设备 | 33 | 📱 📲 ☎️ 📞 📟 |
| `ojr` | 服饰配件 | 47 | 👓 🕶️ 🥽 🥼 🦺 |
| `ojs` | 日用工具 | 67 | 🔨 🪓 ⛏️ ⚒️ 🛠️ |
| `ojt` | 节庆礼物 | 21 | 🎃 🎄 🎆 🎇 🧨 |
| `oju` | 标志符号 | 64 | 🏧 🚮 🚰 ♿ 🚹 |
| `ojv` | 箭头按钮 | 86 | ⬆️ ↗️ ➡️ ↘️ ⬇️ |
| `ojw` | 时间钟表 | 31 | ⌛ ⏳ ⌚ ⏰ ⏱️ |
| `ojx` | 钱财商务 | 13 | 💰 🪙 💴 💵 💶 |
| `ojy` | 文书办公 | 59 | 📔 📕 📖 📗 📘 |
| `ojz` | 星座图形旗帜 | 65 | 🛐 ⚛️ 🕉️ ✡️ ☸️ |

## 选词与返回

- 输入 `oj` 打开总览；继续按 `f` 得到 `ojf` 动物类，按 Backspace 返回 `oj`
  总览；继续按另一个分类字母即可换类。
- 按当前 `PageSize=5` 每页显示 5 个；Up/Down 翻页，Left/Right 移动候选游标，
  空格选当前候选，`1`–`5` 选当前页对应 Emoji。
- 目录和分类不会因码长或 AutoSelect 设置自动提交首项，需明确选词。
- Enter 原样提交 `oj` / `ojf` 等编码；Ctrl+Space 也先提交原码再切换模式；
  Esc 取消。英文模式输入普通 ASCII，不打开 Emoji 候选。
- `ExactMatch=False` 也只显示所选页，不将总览与各类中的同一 Emoji 重复列出。

## 原码保护

按原始普通候选区核查，不包含用户学习数据：

| 来源 | 原始双字母空码 | `oj` 家族状态 |
| --- | --- | --- |
| `~/.local/share/fcitx5/table/flypy.dict` | `oj` | 根码及 `oja`–`ojz` 均未占用，启用目录 |
| `src/assets/flypy.txt` | `of`、`qo` | `oj` 已有 `）`，整组禁用，绝不改用其他入口 |

运行时检查原码冲突：根码被占用则禁用整个家族；某个分类码被占用则保留原候选，
不在该分类码追加 Emoji。该类 Emoji 仍可在总览找到。根码和类别标题都不覆盖原码。
真实词典 `of` → `^`、`qo` → `群殴` 保持不变，不改写词典或用户学习文件。

基础文件 SHA-256：

- 本地 `flypy.dict`：`2eed2a8f62484edad80e71e09c74c917c89b1c23b628c48e5ef5faee5a98fd81`
- 内置 `flypy.txt`：`12eff006341d5712a2f5a54d3215d5fdd1eab668307f19df2c695f9b402f079d`

## 数据维护与诊断

`src/assets/emoji.txt` 是生成后的离线目录，安装运行时不联网。所有数据都属于
`oj` 或 `oja`–`ojz`；`Label[code]=...` 是 waytinyime 的本地候选标题扩展，
只用于展示，不参与提交。类别码精确匹配，不改变普通码表的前缀匹配策略。

更新同一 Unicode 版本的数据（Python 标准库，无额外依赖）：

```sh
python3 tools/update-emoji.py
# 或使用已下载的官方 emoji-test.txt，完全离线重新生成：
python3 tools/update-emoji.py --source /path/to/emoji-test.txt
```

生成器输出每类数量和源文件哈希，拒绝不匹配的 Unicode 版本或未归类的子组。
总览原有的 20 个常用表情始终保留在开头。当前本地词典新增 2768 条候选记录：
1384 条总览加 1384 条分类记录，实际不重复 Emoji 数仍为 1384。

```sh
.cache/auto-loop/build/src/waytinyime --inspect --input-method flypy --query oj
.cache/auto-loop/build/src/waytinyime --inspect --input-method flypy --query ojf
.cache/auto-loop/build/src/waytinyime --inspect --input-method flypy --query ojq
```

两类候选窗使用中文主字体与 Emoji 彩色字体回退，处理 FE0F 而不显示多余方框；
固定 bitmap strike 缩放到候选行字号。XIM 使用现有 COMPOUND_TEXT 的 UTF-8 扩展段，
客户端对 Unicode 的支持仍取决于其实现。
