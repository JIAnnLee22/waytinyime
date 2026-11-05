# waytinyime

一个在原 Wayland 合成器（及 Xwayland）下运行的原生输入法。当前仓库实现
fcitx5 码表配置与 libime UTF-8 文本/二进制主词库加载、Wayland
input-method-v2 上屏与候选定位，以及 Xwayland XIM 上屏、预编辑/候选窗口和
SpotLocation 定位。

候选框不重复显示输入框中的当前编码，普通候选直接从 `1.` 开始；Emoji 类别名
和通配查询中各候选后的完整编码仍保留。XIM 未采用客户端预编辑回调时，候选框
保留当前编码作为回退，避免无法查看正在输入的内容。

## 构建

```sh
nix develop --no-update-lock-file .
meson setup build .
meson compile -C build
meson test -C build --no-rebuild --print-errorlogs
```

构建依赖：Wayland client、xkbcommon、fontconfig、freetype、xcb/xcb-imdkit、
CMake，以及 `libime`/`fcitx5`/Boost（用于只读解码 fcitx5 的二进制 `.dict`）。
`nix develop` 已包含这些依赖。运行 XIM 还需要合成器启动的 Xwayland；本输入法
不会自行启动 Xvfb/Xorg，也不会替代系统 fcitx5。

## 运行环境与支持矩阵

已在隔离的 sway 1.12（headless 或嵌套，非用户桌面）中实测：

| 前端 | 协议/应用 | 已验证能力 |
| --- | --- | --- |
| Wayland | `input-method-unstable-v2` + `virtual-keyboard-unstable-v1`，foot 1.28（text-input-v3） | 编码、选词、翻页、UTF-8 上屏、退格、取消、候选弹窗随插入点和窗口移动、缩放/多输出、失焦隐藏 |
| Xwayland | sway 启动的真实 Xwayland，xterm 411 + `XMODIFIERS=@im=waytinyime` | XIM 编码协商、中文选词/上屏、未消费键 press/release 转发、SpotLocation 候选定位、窗口移动/边界钳制、焦点切换清理 |

未在 GNOME/KDE 等其他合成器上验收；Wayland 候选弹窗依赖合成器实现的
`zwp_input_popup_surface_v2` 定位语义（`text_input_rectangle` 是弹窗相对坐标，
不是全局坐标）。Xwayland 必须由同一个合成器启动，`Xvfb`/独立 `Xorg` 不作为
验收环境。

## 隔离启动示例

以下命令只操作临时 `XDG_RUNTIME_DIR`，不要对已有桌面或已有 fcitx5 会话注入
按键。headless sway 需要至少一个输出：

```sh
R=$(mktemp -d)
export XDG_RUNTIME_DIR=$R
export WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=pixman
cat >$R/sway.conf <<'EOF'
output HEADLESS-1 resolution 1280x800
EOF
sway -d -c $R/sway.conf &
export WAYLAND_DISPLAY=wayland-1
```

让 Xwayland 建立后，从 Xwayland 客户端环境取得 `DISPLAY`（例如启动一次
`xeyes` 并读取其环境），再启动输入法：

```sh
XMODIFIERS=@im=waytinyime WAYTINYIME_DEBUG=1 \
  build/src/waytinyime --input-method flypy
```

原生 Wayland 应用直接使用同一 `WAYLAND_DISPLAY`：

```sh
foot
```

Xwayland 应用必须显式使用同一个 XIM 名称，且不要设置 `GTK_IM_MODULE`/
`QT_IM_MODULE` 让系统输入法代劳：

```sh
DISPLAY=:1 XMODIFIERS=@im=waytinyime xterm -u8
```

调试日志可用 `WAYTINYIME_DEBUG=1` 打开，包含 XIM 样式、SpotLocation、候选
坐标和前端焦点切换；不会读取或写入 `*.user.dict`、`*.history` 等学习数据。

## 启动与码表来源

默认按 fcitx5 的 XDG 数据目录查找 `flypy.conf`。来源优先级：

1. `--table PATH`：直接指定 `.txt` 或 `.dict`
2. `--config PATH`：指定 `.conf`
3. `WAYTINYIME_TABLE`：直接表路径
4. `WAYTINYIME_CONF`：配置路径
5. `--input-method NAME` / `WAYTINYIME_INPUT_METHOD`（默认 `flypy`）对应的
   `inputmethod/NAME.conf`

查找目录依次为 `WAYTINYIME_FCITX_DIR`（隔离测试用，覆盖 fcitx5 数据根目录）、
`$XDG_DATA_HOME/fcitx5`（默认 `~/.local/share/fcitx5`）和 `$XDG_DATA_DIRS` 中的
`*/fcitx5`。

配置中的 `File=table/xxx.dict` 是相对于 fcitx5 数据目录的路径，不是相对于
`.conf` 文件所在目录。找不到配置时回退到内置 `share/waytinyime/flypy.txt`
（开发构建中为 `src/assets/flypy.txt`），因此没有 fcitx5 配置的环境仍可启动。

`.conf` 的 `[Table] File=` 可以指向：

- `.txt`：按 libime/fcitx5 官方 UTF-8 文本码表解析。
- `.dict`（或带 libime 主词库 magic 的文件）：用 libime 只读解码为主词库文本，
  再交给同一解析器。不会读取 `*.user.dict`、`*.history`，遇到这些路径会直接
  报错；用户学习数据不会被修改或合并。

## 诊断入口

不连接 Wayland 即可检查真实配置和码表：

```sh
# 默认发现 flypy.conf
build/src/waytinyime --inspect

# 分别检查三套真实配置
build/src/waytinyime --inspect \
  --config ~/.local/share/fcitx5/inputmethod/flypy.conf --query a
build/src/waytinyime --inspect \
  --config ~/.local/share/fcitx5/inputmethod/tiger.conf --query u
build/src/waytinyime --inspect \
  --config ~/.local/share/fcitx5/inputmethod/tigress.conf --query u

# 直接检查文本码表
build/src/waytinyime --inspect --table src/assets/flypy.txt --query xm
```

输出包含解析后的 `[InputMethod]` 本地化名称、`File` 解析结果、文本/二进制类型、
`[Table]` 选项、分页与选词键、码表统计和一次真实查询。错误会带配置文件或码表
路径、行号及原因。

## 文本码表兼容范围

已支持：

- 英文与中文关键字：`KeyCode=`/`键码=`、`Length=`/`码长=`、
  `InvalidChar=`/`规避字符=`、`Pinyin=`/`拼音=`、`Prompt=`/`提示=`、
  `ConstructPhrase=`/`构词=`、`PinyinLength=`/`拼音长度=`（兼容保留）。
- 节：`[Data]`/`[数据]`、`[Rule]`/`[组词规则]`、`[Phrase]`/`[词组]`。
- UTF-8 BOM、CRLF、配置/规则节中的 `#` 注释、空行、前后空白。
- `[Data]` 的引号值及 `\"`、`\\`、`\n`、`\r`、`\t`、`\f`、`\v` 转义；
  `[Phrase]` 的整行引号值。
- 普通词条、重复编码的稳定文件顺序、`InvalidChar` 对单字反查表的排除。
- `Pinyin=`、`Prompt=`、`ConstructPhrase=` 前缀条目不会混入普通候选；提示
  单独保存，构词条目用于 `[Phrase]` 规则生成。
- `[Rule]` 的 `eN`/`aN`、`p`/`n`、正向/反向码元索引，以及 `[Phrase]` 按规则
  和单字最短编码生成词条（与 libime 的“最短码、同长取先出现者”规则一致）。
- 非法 UTF-8、缺失/非法 `KeyCode`/`Length`、前缀与键码冲突、非法规则、
  无规则却出现 `[Phrase]`、超长/越界编码等均返回带行号的诊断。

仍有限制：

- `Pinyin=` 条目已解析但不提供拼音反查/辅助输入。
- `Prompt=` 提示已保存但当前候选窗不显示拆字提示。
- 自动组词的稳定性依赖单字反查；没有单字编码的词组会被拒绝并计入诊断。
- 二进制 `.dict` 经 libime 的文本导出再解析；导出顺序为 libime 的主词库顺序，
  不会包含用户词库或历史。

## 反引号通配查询

- 中文模式下，反引号 `` ` `` 是通配符，每个恰好匹配一个 `a`–`z` 字母，
  不匹配空字符、多个字母或标点；可放在编码的任意位置，也可连续输入。
- `` jk` ``：仅查询 `jk` 开头的三码；``` jk`` ```：仅查询 `jk` 开头的四码；
  `` jk`a ``：仅查询 `jk` 开头、`a` 结尾的四码。
- 含通配符时严格按输入码长查询，即使 `ExactMatch=False` 也不扩展到更长编码。
  保留词库顺序和翻页选词，不因 `AutoSelect` 或无匹配自动选词而提前上屏。
- 通配查询的候选后显示完整实际编码，例如 `警察 [jkia]`；Wayland 和 Xwayland
  候选窗一致。普通查询不附加编码，选词上屏也只提交候选文本。
- Backspace 删除一位并刷新结果；Enter 原样提交包含反引号的编码；Esc 取消。
  英文模式的反引号仍直接交给应用。

## 回车与无匹配编码

- 中文模式下，有尚未提交的编码时，按 Enter 或小键盘 Enter 原样提交英文编码，
  不选择中文候选，也不额外插入换行；没有编码时 Enter 照常交给应用。
- Ctrl+Space 切换中英文前，先原样提交尚未完成的编码（例如 `jk`），即使存在
  中文/Emoji 候选也不自动选词、不丢弃，不额外插入空格。没有未提交编码时只
  切换模式；Esc 仍明确取消当前组合。
- 空格和数字选词仍提交中文候选。
- 按 `AutoSelectLength`/`AutoSelectRegex` 自动上屏：当前 flypy 配置通常四码有
  候选即提交首选，即使同码有多个候选；其他码表仍遵守各自的自动选词配置。
- 四码没有匹配时继续保留完整编码，超过码表最大码长也不会被拆开自动提交。
  可继续输入、退格编辑，最后用 Enter 上屏完整英文，或用 Esc 取消。
- 无匹配编码不因 `CommitInvalidSegment` 自动上屏，以免英文单词在码长边界
  被强行切断。

## 中文标点与双码表情

- 中文模式把标点转换为中文全角形式：`,.?!:;` → `，。？！：；`，括号为
  `（）【】`，书名号为 `《》`，`^`/`_` 为 `……`/`——`，`/` 与反斜杠 `\`
  映射到顿号 `、`；其他 ASCII 符号使用对应全角形式。单双引号分别交替输出
  `‘’` 和 `“”`。
- 英文模式直接输入半角 ASCII。Ctrl/Alt/Super 快捷键不转换；字母、数字和
  空格不会全角化。按 Ctrl+Space 切换模式时重置引号配对状态。
- 选词/翻页绑定优先（例如已有多个候选时 `;` 仍选第二候选）。标点若确实是
  当前编码的有效延续，则保留编码用途；空闲状态的标点优先输出全角符号，
  包括 `;` 和 `'`，不再用它们启动符号前缀编码。
- 标点前有未提交内容时，先提交当前选中候选（没有候选则提交原始英文），
  再输出全角标点。Enter 原样提交英文的行为不变。
- 反引号 `` ` `` 保留为上述通配查询，不转换为全角标点。
- **Emoji 使用 `oj` 统一入口**：`oj` 总览，继续输入 `a`–`z` 进入 `oja`–`ojz`
  的 26 类候选，共 **1384 个不重复 Emoji**。不是为每个表情分配独立编码，也
  不填充其他空码。候选窗显示类别名，完整分类表见 [Emoji 分类](docs/emoji-codes.md)。
  候选数据位于 [`src/assets/emoji.txt`](src/assets/emoji.txt)。
- 当前本地 `flypy.dict` 的 `oj` 为空，所以启用该入口；`of`、`qo` 等其他编码
  不会新增表情。若别的词库已经使用 `oj`，则保留原字符并禁用此入口，不会
  自动改用其他空码，也不改写 `~/.local/share/fcitx5` 文件。

| 基础码表 | 原始空码（仅供核查） | Emoji 入口 |
| --- | --- | --- |
| 当前本地 `flypy.dict` | `oj` | 仅 `oj` |
| 仓库内置 `flypy.txt` | `of`、`qo` | 不启用，`oj` 已有 `）` |

当前真实词典中，`oj` + 空格 → `😀`，`oj` + `2` → `😃`；`ojf` 动物、
`ojj` 餐食、`ojq` 科技、`ojt` 节庆。输入分类码后按 Backspace 回总览，再输入
另一字母换类。Up/Down 翻页，左右键和数字选词沿用原规则，不自动提交目录首项。
`of` 仍输入 `^`，`qo` 仍输入“群殴”。

`--inspect` 会显示 `emoji-overlay: added=...`；扩展表和内置码表一并安装到
`share/waytinyime/`。两类候选窗均支持 emoji 字体回退、彩色 BGRA 位图及固定
字号缩放；本机使用 Noto Color Emoji。没有覆盖相应字形的 emoji 字体时，
候选外观仍可能缺字，但提交的 UTF-8 文本不受影响。数据来自 Unicode Emoji 16.0，
采用单个基字形及可选的 FE0F 表情样式，暂不包含 ZWJ 连字、肤色组合、双区域字母
国旗或组合键帽；不是宣称覆盖全部 Unicode Emoji。原始许可随数据一起安装。

## `[Table]` 选项支持范围

实际接入输入流程：

- `PageSize`：候选窗每页数量（0 表示隐藏候选窗并禁用数字选词；此时候选以
  内联方式工作，默认选词键仍提交第一个候选而不是把该键转发给客户端）。
- `AutoSelect`、`AutoSelectLength`、`AutoSelectRegex`：有匹配候选，且达到最大码长
  或满足配置的长度/正则时自动提交首选；无匹配时继续组合。正则使用 POSIX ERE
  （真实 flypy 配置中的 `^;.$|^[a-z']{4}$` 已验证）。
- `NoMatchAutoSelectLength`、`NoMatchAutoSelectRegex`：追加新码后无匹配时先提交
  上一段候选。
- `ExactMatch`：`True` 时只匹配完整编码；`False` 时按 libime 的
  `TableMatchMode::Prefix` 返回所有以当前编码为前缀的候选；含反引号的查询始终
  等码长匹配。
- `[Table/PrevPage]`、`[Table/NextPage]`：翻页键，支持 `Up`、`Down`、`minus`、
  `equal` 等 xkb 键名。
- `[Table/Selection]`、`[Table/DefaultCandidate]`、`[Table/SecondCandidate]`、
  `[Table/ThirdCandidate]`：当前页选词、默认选词、第二/第三候选键；默认选词
  提交当前候选游标处的候选，而不是固定第一个候选。
- `[Table/PrevCandidate]`、`[Table/NextCandidate]`：候选游标左右移动。与 fcitx5
  的 `CommonCandidateList` 一致，游标跨越页边界时自动翻页并在首尾循环；翻页键
  会把游标重置到新页第一个候选，候选窗高亮显示当前游标。
- `CommitInvalidSegment`：保留解析值；无匹配时由上述英文输入规则优先处理，
  不再因达到最大码长提前提交原始编码。

已解析并保留、但尚未影响当前交互：

- `CommitAfterSelect`、`CommitWhenDeactivate`、`FirstCandidateAsPreedit`、`NoSortInputLength`、`OrderPolicy`、
  `UseSystemLanguageModel`、`UseContextRelatedOrder`、`Learning`、
  `AutoPhraseLength`、`SaveAutoPhraseAfter`、`MatchingKey`、`PinyinKey`、
  `QuickPhraseKey`、`QuickPhraseText`、`Hint`、`DisplayCustomHint`、
  `HintSeparator`、`SpaceBeforeHint`、`CandidateLayoutHint`、`KeypadAsInput`。
- 这些选项会在 `--inspect` 中显示；其中排序、词频/上下文、快捷短语和提示 UI
  属于后续阶段或独立功能，当前不会静默声称已生效。
- `UseFullWidth`、`IgnorePunc` 保留解析值；符号形态固定跟随中英文模式，不由这
  两个表选项切换。字母、数字及空格仍保持半角。

## 候选窗字体与清晰度

Wayland 与 XIM 候选窗均使用 Fontconfig 的 `sans-serif:lang=zh-cn` 系统匹配，
保留完整的字体集合索引与可变字体命名实例，不固定某个字体文件或字重。本机
匹配为 Noto Sans CJK SC Regular；可用以下命令查看当前系统选择：

```sh
fc-match -f '%{family} %{style} index=%{index}\n' 'sans-serif:lang=zh-cn:pixelsize=18'
```

候选窗基准字号仍为 18px，不读取 GTK/Qt 的桌面字号。原生 Wayland 在合成器
支持 `wl_surface.preferred_buffer_scale`（`wl_compositor` v6）时，按建议倍率
重新栅格化文字与 Emoji，并提交对应的高分辨率缓冲，避免把 1x 小位图直接放大。
旧合成器维持 1x；尚未接入精确分数缩放协议。XIM 仍使用 X 像素，窗口被
Xwayland/合成器缩放时可能产生额外模糊。系统字体设置改变后需重启输入法。

## XIM 前端行为

- 声明样式：`PreeditPosition|StatusNothing`（xterm 默认的 OverTheSpot）、
  `PreeditCallbacks|StatusNothing`、`PreeditNothing|StatusNothing` 和
  `PreeditNothing|StatusNone`。不支持 OffTheSpot 的 `PreeditArea|StatusArea`、
  XIM 状态区、字体集协商和客户端自绘状态。
- 对 `PreeditPosition` 消费 IC 的 `XNSpotLocation`/`XNPreeditAttributes`，
  按 XIM 规范把坐标视为客户端窗口相对坐标，再通过
  `xcb_translate_coordinates` 转为根窗口坐标。候选 X 窗口是 override-redirect
  的 `waytinyime-candidate`，使用 FreeType 绘制候选列表和游标高亮，仅在客户端
  未采用预编辑回调时额外绘制当前码；
  提交、Escape、失焦或 `UNSET_IC_FOCUS` 时立即 unmap。
- 同时监听焦点窗口的 `FocusOut` 和顶层客户窗口的 `StructureNotify`。移动
  xterm 顶层窗口时，即使 XIM 不再发送新的 SpotLocation，候选窗口也会随
  客户窗口的根坐标重新定位。
- `PreeditCallbacks` 会发送 `PREEDIT_START`、`PREEDIT_DRAW`、
  `PREEDIT_CARET`、`PREEDIT_DONE` 和 COMPOUND_TEXT 预编辑串；`PreeditNothing`
  客户端没有 SpotLocation 时使用指针位置作为降级几何，不能声称得到精确光标。
- XIM 只协商 `COMPOUND_TEXT`，提交和预编辑使用 UTF-8 转 COMPOUND_TEXT。
  已验证 xcb-imdkit 的 UTF-8 扩展段可往返补充平面字符，并在真实 xterm 中
  收到 `😀😃😅`；不能据此保证所有旧 XIM 客户端都支持 emoji。
  xterm 在 XIM 启动/首次聚焦的极端竞态下可能丢弃第一个按键；这是客户端/XIM
  初始化限制，使用文档不把它当作已修复。
- XIM 候选窗使用 X 像素和整个 X screen 的边界钳制，不单独查询每个输出；
  Xwayland/合成器的输出缩放会统一缩放该窗口，但没有按输出做独立 DPI 或
  多输出边界翻转。

## 焦点与模式策略

Wayland 与 Xwayland 共用同一个 `input_mode`，但焦点来源独立：

- Wayland `input_method.done` 的激活（即使已处于 active）会取消 XIM 未提交
  编码并隐藏 XIM 候选窗。
- XIM `SET_IC_FOCUS`、`FORWARD_EVENT` 或 X `FocusOut` 会取消另一前端残留的
  组合；`UNSET_IC_FOCUS`/`DESTROY_IC` 也会清理 XIM 组合和候选窗。
- 单独的 Ctrl/Alt/Super/Shift 等修饰键不提交未完成编码；按下 Ctrl+Space
  时先原样提交当前编码、清空预编辑并隐藏候选窗，再切换一次模式。Ctrl 按下的
  press/release 继续转发，切换本身不产生额外空格或换行。
- Wayland 和 XIM 的候选窗都在取消/提交/失焦时 unmap，不在焦点切换后悬挂。

## 当前阶段状态

阶段 1 的配置、`.dict` 只读解码、UTF-8 文本码表、`[Table]` 选项和错误诊断已
接入生产入口；阶段 2 的 Wayland 上屏/候选定位和阶段 3 的 Xwayland XIM
上屏/候选定位已在隔离 sway + Xwayland 中用真实 foot/xterm 实测。阶段 4 的
Ctrl+Space 中英文切换、英文直通、跨前端焦点清理和启动/退出回归已覆盖。
仍存在的限制已在上文逐项列出，尤其是 XIM 首次按键竞态、无 SpotLocation 时的
降级定位、XIM 不绘制客户端状态区，以及未实现的 Pinyin/Prompt UI、学习/词频/
快捷短语等表选项。中文标点及双码表情已接入两类前端并验证上屏。
