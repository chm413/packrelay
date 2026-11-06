# packrelay

面向 **LeviLamina（BDS）** 的非原版资源包自动维护与分发插件（C++20）。

扫描本地资源目录 → 按 `uuid + 版本 + 全量文件哈希` 精确判定变更 → 自动 git push 到指定仓库 →
仓库预置的 GitHub Actions 识别变更、打包 zip、创建 Release → 插件把 CDN 地址切到最新 Release。

- `packrelay` —— 可独立运行的引擎（守护进程 / CLI），零第三方依赖
- `packrelay.mod.dll` —— LeviLamina 适配层，只做接线，业务逻辑全在 core
- `.github/workflows/release.yml` + `scripts/ci-release.sh` —— 插件自动铺进目标仓库的发布流水线

> **命名约定**：对外名称统一 `packrelay`（mod id、DLL 名、CLI 名、指令前缀、日志前缀）。
> 内部标识保留 `mcpsync`（C++ namespace、`include/mcpsync/`、`mcpsync_core` 静态库、源码目录 `mcpack-sync/`），
> 以及状态目录 `.mcpack-sync/`——改它会让已有部署的 `state.json` 失效、全部资源包当作新增重传一遍。

---

## 1. 整体架构

```
┌──────────────────────────── BDS / bedrock_server_mod.exe ────────────────────────────┐
│  PreLoader → LeviLamina (mod loader)                                                 │
│      └── mods/packrelay/packrelay.mod.dll           ← 适配层（MSVC 构建）             │
│              │  读取 mods/packrelay/config/config.json                              │
│              │  订阅 ServerStartedEvent / ServerStoppingEvent                         │
│              │  注册 /relay (status|scan|cdn|push|dump|reload) 与 /relaygit          │
│              ▼                                                                        │
│      mcpsync_core（静态库，纯 C++20）                                                  │
└──────────────────────────────────────────────────────────────────────────────────────┘
                                    │
        ┌───────────────────────────┼────────────────────────────┐
        ▼                           ▼                            ▼
  ①扫描/比对                   ②推送                        ③④发布与分发
  resource_packs/          git push → GitHub            Releases API / 镜像
  manifest.json            (index.json + packs/)         → CDN 三层 sink
```

数据流：

| 阶段 | 动作 | 关键产物 |
|---|---|---|
| Scan | 递归找出所有含 `manifest.json` 的目录，逐文件 SHA-256 | `PackSnapshot` |
| Diff | 与 `.mcpack-sync/state.json` 比对 uuid / version / fileSetHash | `Change{Added,Updated,Removed,Unchanged}` |
| Publish | 镜像到仓库 `packs/<slug>/`，写 `index.json`，commit + push | commit sha |
| Release | 轮询 `GET /repos/{o}/{r}/releases/tags/{tag}` | 资产 URL |
| Distribute | 解析镜像 → 更新 CDN sink | `cdn.json` / `cdn_config.json` / 302 目标 |

---

## 2. 关键模块划分

| 模块 | 文件 | 职责 |
|---|---|---|
| M0 基础 | `src/core/{json,sha256,fsutil,logger,model}.cpp` | 零依赖 JSON（保序）、SHA-256、UTF-8 路径与原子写、分级日志 |
| M1 配置 | `src/core/config.cpp` | JSON→`Config`、默认值、`baseDir` 解析、`validateConfig` |
| M2 扫描 | `src/core/scanner.cpp` | manifest 解析、全量文件指纹、`fileSetHash`、同 uuid 冲突检测 |
| M3 状态 | `src/core/state.cpp` | `.mcpack-sync/state.json`，原子写、损坏备份重建 |
| M4 比对 | `src/core/diff.cpp` | 三要素判定、`fingerprint = sha256(uuid\|version\|fileSetHash)` |
| M5 进程 | `src/net/process.cpp` | `CreateProcessW` + 管道并发读取 + 超时强杀，CRT 引号规则 |
| M6 网络 | `src/net/{http,github,mirror}.cpp` | WinHTTP（代理/超时/重试）、Releases API、镜像模板渲染 |
| M7 Git | `src/ops/git.cpp` | 幂等提交、token 注入、非快进自动 rebase、仓库模板落地 |
| M8 分发 | `src/ops/cdn.cpp` | `cdn.json` / `server.properties` / `cdn_config.json` / 302 重定向服务 |
| M9 编排 | `src/ops/pipeline.cpp` | 单次/周期执行、失败续跑、并发互斥、报告 |
| M10 打包 | `src/ops/zip.cpp` | 自带 store 模式 ZIP 写入（少量环境无 `zip` 命令） |
| M11 适配 | `src/mod/levilamina_mod.cpp` | LL 生命周期/事件/命令/日志接线 |

---

## 3. 精确比对：如何做到「不误判、不重复上传、不遗漏更新」

**身份**：`header.uuid`（小写归一化）为主键；`modules[type=="resources"].uuid` 作为辅助。
uuid 相同即同一资源包，与目录名、路径、改名完全无关。

**指纹**（三重）：

```
fileSetHash  = SHA-256( ⋃ sorted("relpath\0sha256\n") )     // 全量文件，与 mtime 无关
fingerprint  = SHA-256( uuid + "|" + "1.0.3" + "|" + fileSetHash )
```

**判定矩阵**：

| 历史 state | uuid | version | fileSetHash | 结论 |
|---|---|---|---|---|
| 无 | — | — | — | `Added` |
| 有 | 同 | 同 | 同 | `Unchanged`（不提交、不推送） |
| 有 | 同 | 变 | 任意 | `Updated` |
| 有 | 同 | 同 | **变** | `Updated`（防"只改内容不改版本"的漏更） |
| 有 | 消失 | — | — | `Removed`（`git rm` 目录） |

三条工程约束：

1. **不依赖 mtime / 文件大小**——打包工具、rsync、git clone 都会改写时间戳，只认内容哈希。
2. **推送成功即落状态**（`state.pushed.commit` + `releasePending=true`）；若后续 release 阶段失败，
   下次运行只补发布阶段，**绝不重复提交**。状态文件 `.tmp` + `MoveFileEx(REPLACE_EXISTING)` 原子替换，
   损坏时自动备份为 `state.json.corrupt.<ts>` 并重建，避免"永远认为已是最新"。
3. **同 uuid 多目录冲突**：`identity.conflictPolicy = highest_version | error | newest_mtime`，
   默认保留高版本，冲突写进报告与日志（BDS 自带 `vanilla`/`vanilla_1.21.x` 就是同 uuid 多目录）。

实测：BDS 1.26.60 自带 `resource_packs/` 共 67 个 manifest → 归并为 12 个唯一 uuid，
报告 55 条冲突并被策略正确收敛。

---

## 4. 配置项设计

配置文件：`<serverRoot>/config/packrelay/config.json`（mod 场景亦可用 `mods/packrelay/config/config.json`）。
所有相对路径以 `baseDir` 为基准（`baseDir` 缺省为配置文件所在目录，可由 JSON 字段覆盖；
mod 场景固定为服务器根目录，保证升级重装后状态不漂移）。

| 段 | 键 | 说明 |
|---|---|---|
| 根 | `resourceDir` | 资源根目录，递归查找 `manifest.json` |
| | `baseDir` | 相对路径基准 |
| `server` | `scanIntervalSeconds` | 周期扫描间隔 |
| | `scanOnStart` / `scanOnWorldLoad` | 启动时触发 / World（Level 可获取）后补一次扫描 |
| | `shutdownTimeoutMs` | 停服等待流水线收尾的上限；超时则放手（对象故意泄漏）避免卡死进程 |
| | `debounceMs` | 事件触发去抖，避免连续写盘打爆扫描 |
| `identity` | `conflictPolicy` | 同 uuid 多目录策略 |
| | `ignorePatterns` | 通配（`**`、`*`、`?`，大小写不敏感） |
| | `matchModuleUuid` | 是否同时校验 resources 模块 uuid |
| `git` | `remote` / `branch` / `subDir` | 目标仓库与包存放子目录 |
| | `authMode` | `token_env`（默认）/ `token_inline` / `credential_helper` / `ssh` |
| | `tokenEnv` / `token` | 模式一读环境变量，模式二读明文 `token`（控制台写入） |
| | `username` / `password` | HTTPS 基本认证；不给 `username` 时默认 `x-access-token` |
| | `sshKey` / `sshKeyEnv` | SSH 私钥路径，走 `core.sshCommand` 注入 |
| | `pushRetries` / `retryBackoffMs` / `timeoutMs` | 推送退避与超时 |
| | `commitTemplate` | `{name} {version} {uuid8} {slug} {files}` |
| `release` | `tagTemplate` | 默认 `{slug}-v{version}-{hash8}`，**由插件预渲染进 index.json** |
| | `assetNameTemplate` / `assetSuffix` | 资产名与匹配后缀 |
| | `waitForRelease` / `pollIntervalMs` / `pollTimeoutMs` | 轮询策略；`waitForRelease=false` 时用 URL 规则推导 |
| `github` | `apiBase` / `owner` / `repo` | 可在 GitHub Enterprise / 本地 mock 间切换 |
| | `tokenEnv` / `proxy` / `timeoutMs` / `maxRetries` | 认证、代理、重试 |
| `cdn` | `mode` | `release_asset`（默认，等 Release）/ `static`（外部静态托管，用 `staticUrl` 拼地址、不等 Release）；`mirror_template` 为历史等价取值 |
| | `staticUrl` | `mode=static` 时的基址，最终地址 = `staticUrl + "/" + assetName` |
| | `mirrors[]` | `{name, enabled, priority, prefix}` 或 `{..., template}`，两种写法见下节 |
| | `activeMirror` | `auto`（按 priority 取最高）或镜像名，如 `"ghfast"` |
| | `writeManifest` / `manifestFile` | 写 `.mcpack-sync/cdn.json` 供外部分发层消费 |
| | `bdsCdnConfig` | 写 BDS 官方 `cdn_config.json`，每包完整下载地址（`key` 默认 `"CDN URL"`） |
| | `serverProperties` | 可选，把地址写进 `server.properties` 自定义键 |
| | `redirectServer` | 可选内嵌 302 服务（默认关闭）：`bind/port/path`，对外地址恒定、目标热切换 |
| `log` | `level` / `file` / `maxSizeMB` / `rotate` | 分级 + 按大小轮转 |

完整示例见 [`config/config.example.json`](config/config.example.json)。敏感项全部走环境变量
（`MCPACK_GIT_TOKEN` / `MCPACK_GH_TOKEN` / `MCPACK_GH_OWNER` / `MCPACK_GH_REPO`），配置文件可入库。

### 下载地址拼接

GitHub 托管模式下，资源包的下载地址就是拼出来的字符串，插件负责拼完整：

```
基线（直连）
  https://github.com/{owner}/{repo}/releases/download/{tag}/{asset}

加 gh 加速（前缀直接拼在基线前面，不做 URL 编码）
  https://ghfast.top/https://github.com/{owner}/{repo}/releases/download/{tag}/{asset}
```

实跑示例（`owner=chm413`、`repo=mcpack`）：

```jsonc
// cdn_config.json
{
  "CDN URL": "https://ghfast.top/",
  "packs": [{
    "uuid": "11111111-...",
    "url":       "https://ghfast.top/https://github.com/chm413/mcpack/releases/download/demo-pack-v1.0.0-4c375dbc/demo-pack-v1.0.0-4c375dbc.zip",
    "directUrl": "https://github.com/chm413/mcpack/releases/download/demo-pack-v1.0.0-4c375dbc/demo-pack-v1.0.0-4c375dbc.zip",
    "mirrors": [ { "mirror": "direct", "url": "..." }, { "mirror": "ghfast", "url": "..." } ]
  }]
}
```

`url` 永远是可直接下载的完整地址；`mirrors` 给出全部备选，客户端或运维可自行降级。

### 加速镜像

`cdn.mirrors` 支持两种写法，按 `priority` 升序生效：

| 写法 | 字段 | 说明 |
|---|---|---|
| 前缀式 | `prefix` | 把完整地址直接拼在加速域名后，**推荐**（ghfast / gh-proxy 都属于这类） |
| 模板式 | `template` | 更灵活，可用 `{url}` `{url_enc}` `{owner}` `{repo}` `{tag}` `{asset}` `{path}` |

```json
"mirrors": [
  { "name": "direct",   "enabled": true,  "priority": 0,  "template": "{url}" },
  { "name": "ghfast",   "enabled": true,  "priority": 10, "prefix": "https://ghfast.top/" },
  { "name": "gh-proxy", "enabled": false, "priority": 20, "prefix": "https://gh-proxy.com/" },
  { "name": "jsdelivr", "enabled": false, "priority": 30, "template": "https://cdn.jsdelivr.net/gh/{owner}/{repo}@{tag}/{path}" }
]
```

`activeMirror` 三种取值：

- `"auto"`（默认）— 取优先级最高的启用项，即上面的 `direct`，下发直连地址
- `"ghfast"` — 强制走 ghfast 加速，下发的每条地址都带 `https://ghfast.top/` 前缀
- 其他镜像名 — 同理；名字不存在时自动退回 `auto`，不会导致发布失败

前缀尾部斜杠会自动收敛成单个 `/`，`https://ghfast.top`、`https://ghfast.top//` 都等价于 `https://ghfast.top/`。

### 让 BDS 真正生效：写内存，而不是只改文件

`CDNConfig` 只在 `ServerInstance` 构造时读一次 `cdn_config.json`，**LeviLamina 加载之后再改磁盘文件不会触发重读**，
所以纯文件路线只能等重启才生效。插件因此走 LeviLamina 的服务访问器直接写内存：

| 步骤 | 调用 |
|---|---|
| 拿服务端实例 | `ll::service::bedrock::getServerInstance()` |
| 取 CDN 配置 | `ServerInstance::mCDNConfig`（public 成员，`Bedrock::NonOwnerPointer<CDNConfig>`） |
| 取映射表 | `CDNConfig::mPackCDNUrls`（`std::vector<std::pair<packId, url>>`） |
| 写映射 | `mcpsync::upsertPackCdnUrls()` —— 就地 upsert，保留运维手工加的条目 |
| 切线程 | `ServerInstance::queueForServerThread()`，保证在服务器线程碰 BDS 对象 |

```cpp
auto inst        = ll::service::bedrock::getServerInstance();
::CDNConfig* cfg = inst->mCDNConfig.get().get();   // 持有者失效时返回 nullptr，必须判空
auto& urls       = cfg->mPackCDNUrls.get();         // vector<pair<string,string>>&
mcpsync::upsertPackCdnUrls(urls, entries);          // entries 来自 cdn.json 的 packCdnUrls
```

主键同时写 `<uuid>_<version>` 和裸 `<uuid>` 两种，兼容不同版本的查找方式；写之前会清掉同 uuid 的**历史版本主键**，
否则版本升级后旧键残留，BDS 仍可能取到过期外链。

生效时机：

| 时机 | 触发 |
|---|---|
| 服务器启动 | `ServerStartedEvent` 用上次落盘的 `cdn.json` 补一遍（重启后内存是空的） |
| 每次发布 | `Pipeline::onCdnPublished` 回调，自动推送，**无需重启** |
| 手动 | `/relay push` |

排查：`/relay dump` 打印 BDS 内存里现有的 `packId -> url`，用来核对主键格式；`/relay cdn` 列出清单侧的目标。

---

## 5. 异常处理策略

| 层 | 策略 |
|---|---|
| 调用约定 | core 内部一律 `Result<T>/Status` + `ErrCode`，**不跨 DLL 边界抛异常**；mod 适配层对 LL 回调做 try/catch 兜底 |
| 配置 | 缺失必填项时 mod **不阻塞服务器启动**，自动降级为"仅扫描"并 WARN |
| 状态 | 原子写（`.tmp` → `MoveFileEx`）；解析失败备份 `.corrupt.<ts>` 后重建 |
| 扫描 | 单个包 manifest 非法 → 记 warning 并跳过，不中断整轮；同 uuid 冲突按策略收敛 |
| Git | 认证失败（`Authentication failed` / `could not read Username`）**立即停止重试**并给出修复提示；非快进 → `pull --rebase --autostash` 重试一次；网络类错误指数退避 |
| 网络 | 连接/发送/接收分设超时；仅对 5xx 与网络错误重试（4xx 直接失败）；`404` 在 release 轮询中视为"尚未就绪"继续等 |
| Release | 超时后保留 `releasePending=true`，下次运行只补发布阶段；`waitForRelease=false` 时用 `releases/download/{tag}/{asset}` 规则推导，避免 API 限流阻断发布 |
| 幂等 | `git status --porcelain` 为空 → 跳过提交；同内容重复运行不产生新 commit（E2E 已验证远端 HEAD 不变） |
| 并发 | 流水线内 `std::mutex` 串行化；mod 侧事件只投递 `requestScan()`，不阻塞 tick；停止时 `stopWatcher()` + `join` |
| Git 子进程 | `GIT_TERMINAL_PROMPT=0` / `GCM_INTERACTIVE=never`，防止服务端卡在交互式凭据输入 |

---

## 6. 实现思路要点

**推送**：token 不写进 remote URL，改用 `-c http.extraheader="AUTHORIZATION: basic base64(x-access-token:TOKEN)"`，
避免泄漏到 `.git/config` 与 reflog。参数按 CRT 引号规则拼装后直接 `CreateProcessW`，不经 shell，杜绝注入。

**仓库预置**：`GitPublisher::prepare()` 首次运行时把 `release.yml` / `ci-release.sh` / `README.md`
写入目标仓库并随包一起提交——"仓库预置 workflow"不依赖人工操作。

模板写入是**按内容哈希幂等**的（哈希锁记在目标仓库 `.packrelay/templates.lock`，不需要人工维护模板版本号）：

| 情况 | 行为 |
|---|---|
| 文件不存在 | 写入模板 |
| 内容与模板一致 | 什么都不做（不产生空 diff） |
| 文件哈希 == lock 记录的哈希 | 文件仍是插件写的 → 判定为**模板升级**，备份旧版后覆盖 |
| 文件哈希 != lock 记录的哈希 | 判定为**本地改动**，保留并 warn（删掉文件即可恢复内置模板） |
| 无 lock 但文件已存在 | 首次接管，**备份后覆盖**（保证 Actions 链路开箱可用） |

备份落在 `.packrelay/backup/`（该目录自带 `.gitignore`，不会被 `git add -A` 提交）。

**脏工作区自愈**：`prepare()` 的 `checkout -B <branch> origin/<branch>` 若因上一轮中断留下的
未提交改动而失败，会先 `reset --hard origin/<branch>` 再重试——镜像仓库内容每轮都由扫描结果重建，
硬对齐远端是安全且幂等的；否则这种残留会让后续每一次推送都卡死在同一处。

**tag 单一事实源**：插件在写 `index.json` 前就渲染好 `tag` 与 `assetName` 并写入索引；
Actions 端**不做任何命名推断**，直接读 `index.json`。插件取回 release 用的也是同一个 tag，两端永不漂移。
tag 含 `{hash8}`（指纹前 8 位），因此"内容变了但没改版本号"也能产生唯一 tag，不会撞车。

**CDN 分发（地址是拼出来的，不是查出来的）**

GitHub 托管模式下没有"注册下载地址"的接口，**下载地址完全由 `owner/repo/tag/asset` 拼接得到**：

```
https://github.com/{owner}/{repo}/releases/download/{tag}/{asset}
```

需要加速就在前面加前缀（ghfast 属于前缀式加速，不对 URL 做编码）：

```
https://ghfast.top/https://github.com/{owner}/{repo}/releases/download/{tag}/{asset}
```

因此"始终指向最新 release"= 每次发布把 tag/asset 换成新的，重新拼一遍地址。落地为四层 sink，可任意组合：

1. **BDS 内存映射** —— 通过 LL 接口写 `CDNConfig::mPackCDNUrls`，热生效，见上节（主路径）；
2. `cdn.json` —— 机器可读清单，含 `assetUrl`（直连）/ `publicUrl`（当前生效，含加速）/ `candidates`（全部备选）
   以及扁平映射 `packCdnUrls[]`（`packId` / `uuid` / `version` / `url` / `directUrl`）；
3. `cdn_config.json` —— BDS 磁盘配置，每包写**完整可下载地址** `url` + `directUrl` + `mirrors[]`，供重启后兜底；
4. 内嵌 302 服务（可选，默认关闭）—— 对外暴露恒定地址如 `http://host:19134/pack`，每次发布只换 `Location`。

服务重启时 `CdnManager::init()` 会从 `cdn.json` 恢复目标，**未完成首轮扫描也不会回退到空地址**。

---

## 7. 控制台指令

两条根命令，权限刻意分开：

| 根命令 | 权限 | 用途 |
|---|---|---|
| `/relay` | `GameDirectors` | 运维查询与手动触发，游戏内管理员可用 |
| `/relaygit` | `Host` | git 凭据管理，**仅服务器控制台**，游戏内不可用 |

`/relaygit` 单独一条根命令就是为了守住这条边界：改令牌不能由游戏内管理员操作。

服务器控制台（或管理员）直接管理 git 凭据，**所有修改立即写回 `config.json` 并重建流水线**，无需重启。

| 指令 | 说明 |
|---|---|
| `/relaygit` | 查看当前 git 配置，敏感项脱敏（`ghp_****2345`） |
| `/relaygit remote <url>` | 设置远端，接受 `https://` 或 `git@` 形式 |
| `/relaygit branch <name>` | 设置分支 |
| `/relaygit user <name> <email>` | 设置提交作者 |
| `/relaygit token <PAT>` | 设置令牌，**自动切到 `token_inline`** |
| `/relaygit password <user> <pass>` | 设置 HTTPS 用户名密码 |
| `/relaygit sshkey <path>` | 设置 SSH 私钥，**自动切到 `ssh`** |
| `/relaygit mode <mode>` | `token_env` / `token_inline` / `credential_helper` / `ssh` |
| `/relaygit clear` | 清除 token/password/sshKey，`authMode` 收回 `token_env` |
| `/relaygit test` | 连通性测试（`git ls-remote --heads`，最长 20s） |

其余运维指令（都挂在 `/relay` 下）：

| 指令 | 说明 |
|---|---|
| `/relay` | 等价于 `/relay status`，少敲一层 |
| `/relay status` | 最近一次流水线结果 + 各包当前外链 |
| `/relay scan` | 立即触发一次扫描（异步） |
| `/relay cdn` | 列清单侧的包与外链 |
| `/relay push` | 把外链推入 BDS 内存（热生效） |
| `/relay dump` | 打印 BDS 内存中现有的 `packId -> url` |
| `/relay reload` | 重新读取 `config.json` |

`/relaygit test` 的失败诊断直接给可操作建议，例如实测输出：

```
> /relaygit test
  probing https://github.com/chm413/mcpack.git (auth=token_inline) ...
  failed: exit=128 elapsed=3787ms
  git: remote: Invalid username or token. ...
  → 凭据无效或缺失：用 /relaygit token <PAT> 设置令牌，或 /relaygit password <user> <pass>
```

覆盖的诊断分支：凭据无效、SSH 公钥被拒、host key 未受信、仓库不存在/无权限、网络不可达、工作目录非 git 仓库。

> **凭据落盘是明文的。** `/relaygit token|password|sshkey` 会把值原样写进 `config.json`（用户要求"自动保存"）。
> 因此：
> - 服务器上的 `config.json` 建议 `chmod 600` / 加 NTFS ACL，不要提交进版本库；
> - 更安全的做法是保持 `authMode=token_env`，把令牌放环境变量，凭据不落盘；
> - 回显一律脱敏（`ghp_****2345`），但**日志里也不会打印明文**，只在写盘时出现一次。

### CLI 等价命令

同样的能力在 `packrelay` CLI 上也可用（便于在服务器外用同一份配置排错），参数与行为完全一致：

```bash
packrelay git                                  # 查看
packrelay git remote https://github.com/chm413/mcpack.git
packrelay git user chm413 chm413@users.noreply.github.com
packrelay git token ghp_xxx
packrelay git sshkey C:/Users/me/.ssh/id_ed25519
packrelay git test
packrelay git clear
packrelay cdn                                  # 列出发布记录与外链
packrelay cdn render --dir <服务器根>          # 按 cdn.json 重写 cdn_config.json / server.properties
```

两者共用 `src/core/gitauth.cpp`：`resolveGitToken` / `buildAuthArgs` / `probeGitRemote` / `applyGitField` / `patchConfigFileGit`。

> **注意**：BDS 命令 API（`CommandOutput::success` / `CommandRegistry::parse`）的符号来自
> `bedrock_runtime.dll`——它不是磁盘文件，而是 `prelink` 写入的假模块名，由 **SymbolProvider**
> （`levilamina` 包的传递依赖）在 delay-load 时解析到 BDS 真实地址。这是**链接期**能力，
> xmake 构建默认就具备，插件不做运行期探测；万一注册失败会 catch 住并 WARN，不影响其余功能。
> CLI 命令在任何环境下都可用，且用的是同一份配置文件。

---

## 8. 目录结构

```
mcpack-sync/
├── CMakeLists.txt
├── include/mcpsync/*.hpp
├── src/core/    json sha256 fsutil logger model config scanner state diff gitauth
├── src/net/     process http github mirror
├── src/ops/     git cdn zip repo_template pipeline
├── src/cli/     main.cpp            # scan | once | watch | redirect | package | dump-state | init-config | git | cdn
├── src/mod/     levilamina_mod.cpp  # LL 适配层（需 MSVC）
│                manifest.json
├── config/config.example.json
├── scripts/     e2e.mjs  mock-gh-server.mjs
└── tests/test_main.cpp
```

---

## 9. 构建

两条路径，职责不重叠：

| 产物 | 构建系统 | 依赖 |
|---|---|---|
| `packrelay`（CLI）+ `mcpsync-tests`（单测） | **CMake** | 零第三方依赖，Windows / Linux 都能编 |
| `packrelay.mod.dll`（LL 插件） | **xmake** | 需要 LL 整套包生态，见下 |

```bash
# 引擎 + CLI + 单测
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release -j
ctest --test-dir build --output-on-failure -C Release
```

本机实测工具链：**g++ 15.2.0 (MinGW-w64 UCRT, x86_64)**，CMake 4.2.1。

```bash
# mod（仅 Windows + MSVC）
xmake f -c -y -m release --target_type=server --toolchain=msvc
xmake build -y packrelay
# 产物 build/windows/x64/release/packrelay.dll → 发布时重命名为 packrelay.mod.dll
```

> **为什么 mod 只能走 xmake**：它需要 LL 的整套包生态 —— bedrockdata 预编译头、
> entt/gsl/glm/leveldb 等十几个依赖、`prelink.exe` + `bedrock_runtime_data`
> （生成 `bedrock_runtime_api.lib`）、以及 `symbolprovider`（把 delay-load 的
> `bedrock_runtime.dll` 解析到 BDS 真实地址）。这些由 levimc-repo 的 `levilamina`
> 包一次性解析，还在 CMake 里手工拼不现实。
>
> 另外 `LeviLamina.dll` 导出的是 MSVC 修饰名（如
> `?getModDir@Mod@mod@ll@@QEAAAEBVpath@filesystem@std@@XZ`），MinGW / clang 的 ABI
> 无法解析，所以 mod 必须用 MSVC 工具链。
>
> CI 上另有一个坑：runner 的 PATH 里有 LLVM/clang，xmake 在**包安装阶段**会自己重新
> 探测工具链（`--toolchain=msvc` 管不到这一层），结果用 clang-cl 配 MSVC 的 STL 头编译，
> 炸在 `invalid application of 'sizeof' to an incomplete type`。解决办法是在 configure
> 前把 clang 从 PATH 里剔除，见 `.github/workflows/build.yml`。

**附：`manifest.json` 的三个硬约束**（写错就加载不上，且报错信息不直白）：

| 字段 | 必须 | 写错的报错 |
|---|---|---|
| `type` | `"native"`（`NativeModManagerName`） | `无法识别的模组类型：mod` |
| `entry` | 与实际 DLL 文件名完全一致（这里是 `packrelay.mod.dll`） | 静默不加载 |
| `dependencies` | 若要写必须是数组 `[{"name":"LeviLamina","version":">=26.0.0"}]`；简化起见建议整段省略 | `field must be an array` / `dependency LeviLamina is missing` |

### 9.1 为什么不提供第二条构建路径

早期这里记录过一套「手工 MSVC 直编」流程（自己 `cl` 编全部 TU + 手调 `prelink` + 手动链接）。
它在本机能跑通，但**是一条注定反复出错的路径**，已废弃：

- 手写链接命令必然会漏掉 `SymbolProvider.lib`——而它才是把 delay-load 的
  `bedrock_runtime.dll` 解析到 `bedrock_server_mod.exe` 真实地址的机制。漏了它，
  命令 API 一调用就 `0xC06D007E` 把进程带走。
- 反过来，自己定义 `__pfnDliNotifyHook2` 又会与 SymbolProvider 冲突（`LNK2005`），
  把它顶掉后同样解析失败。两条路都会走到「命令永远不可用」。
- 每次 LL 升级都要重新对齐 prelink 参数顺序、expected-lite 版本、fmt 非内联实现、
  memory operators 这些细节。

这些在 xmake + `levilamina` 包下全部自动完成（`symbolprovider v1.3.0` 是它的传递依赖）。
所以**不要再加第二条构建路径**——真想离线构建，就装一次 xmake 并让它把包缓存下来。

### 9.2 CI：编译 / 打包 / 发布

两个 workflow，职责分开，`release.yml` 直接复用 `build.yml`，保证「发布出去的产物」就是「CI 验证过的产物」。

| 文件 | 触发 | 干什么 |
|---|---|---|
| `.github/workflows/build.yml` | push 到 main、PR、手动 | 三块并行：`core`（Windows+Linux 编译 CLI 与单测并 ctest）、`mod`（Windows+MSVC 编 DLL）、`dist`（汇总打 zip） |
| `.github/workflows/release.yml` | 打 `v*` tag、手动指定 tag | `workflow_call` 调 build.yml → 下载 dist 产物 → `gh release create` |

发版就是打 tag：

```bash
git tag -a v1.0.0 -m "packrelay v1.0.0"
git push origin v1.0.0
```

`v1.0.0` 实测产物：

| 资产 | 内容 |
|---|---|
| `packrelay-v1.0.0-mod-win-x64.zip` | `packrelay/{manifest.json, packrelay.mod.dll, packrelay.pdb}`，解压到 `mods/` 即用 |
| `packrelay-v1.0.0-cli-win-x64.zip` | `packrelay.exe` |
| `packrelay-v1.0.0-cli-linux-x64.zip` | `packrelay`（Linux 版可扫描/比对/打包/渲染；HTTP 只在 Windows 实现，故发布阶段需配 `release.waitForRelease=false` 走 URL 规则推导，`redirect` 不可用） |

**产物结构**（三个 zip）：

```
packrelay-<tag>-mod-win-x64.zip     解压 = packrelay/，整个目录丢进 mods/ 即用
  └── packrelay/{manifest.json, packrelay.mod.dll, packrelay.pdb}
packrelay-<tag>-cli-win-x64.zip     packrelay.exe
packrelay-<tag>-cli-linux-x64.zip   packrelay
```

**两条构建路径为什么不同**：

- `core` 用 **CMake**：零第三方依赖，Windows / Linux 都能编，跑 ctest。
- `mod` 用 **xmake**：mod 必须用 MSVC 且要链接 `LeviLamina.lib`，头文件树里有 entt/gsl/glm/leveldb/rapidjson 等十几个依赖，还有 `prelink.exe` + `bedrock_runtime_data`。
  这些由 levimc-repo 的 `levilamina 26.60.2` 包 + `levibuildscript` + `symbolprovider` 一次性解析，手工拼不现实。

> CI 里 xmake 首次会**现场编译 LeviLamina**（约 15–25 分钟），所以 workflow 里缓存了 `C:\xmake\packages`，
> 二次构建降到 2 分钟左右。缓存 key 带 `hashFiles('xmake.lua')`，改构建脚本会自动失效。
>
> xmake 产出的 DLL 名是 `packrelay.dll`，而 `manifest.json` 的 `entry` 要求 `packrelay.mod.dll`，
> 所以 stage 步骤里做了重命名——这一步错了 LL 会**静默不加载**，属于易错点。

发布一个版本：

```bash
git tag v1.0.0
git push origin v1.0.0      # 自动构建并创建 Release
```

CLI 常用命令：

```bash
packrelay init-config --path config/packrelay/config.json
packrelay scan    --config <cfg>                 # 只扫描+比对
packrelay once    --config <cfg> [--dry-run] [--force] [--skip-release]
packrelay watch   --config <cfg>                 # 常驻
packrelay redirect --config <cfg>                # 只跑 302 服务
packrelay package --input <dir> --output <zip> [--json]
packrelay dump-state --config <cfg>
```

### 9.3 仓库元数据（description / topics）

`git push` 只搬代码，搬不走仓库的 description 和 topics——它们存在仓库设置里。对应的两个 REST 端点
（`PATCH /repos/{owner}/{repo}`、`PUT /repos/{owner}/{repo}/topics`）要求 **Administration:write**，
而推代码只需要 **contents:write**。这是两套权限：SSH key 再完整也改不了后者，PAT 只给代码权限时
会得到一句 `403 Resource not accessible by personal access token`。

所以这两项没有"随手改一下"的余地，只能声明化：

```bash
scripts/sync-repo-metadata.sh --dry-run   # 打印将要发送的请求体，不落任何写
scripts/sync-repo-metadata.sh --check     # 只比对；public 仓库不需要 token，有差异 exit 1
GITHUB_TOKEN=... scripts/sync-repo-metadata.sh   # 应用，幂等，只写真有差异的字段
```

内容写在 `scripts/repo-metadata.conf`（`REPO_DESCRIPTION` / `REPO_HOMEPAGE` / `REPO_TOPICS`），
和代码一起 review、一起回滚。token 需要 **Administration: Read and write**（fine-grained，且
Repository access 要包含本仓库），或 classic PAT 勾 `repo`。权限不够时脚本会直接指出缺哪一项，
`--dry-run` 也能在没有 token 的情况下先看结果。

**topics 的取值有约束**：GitHub 网页端只能从**已存在**的标签里选，不存在的一律补全成最相近的那个
（`minecraft-bedrock` 就会被补成 `minecraft-bedrock-modding`，而 `/topics/minecraft-bedrock` 本身是 404）。
用 API 虽然能凭空建新标签，但没人会点开一个零索引的标签，所以 conf 里只放生态中真实流通的名字。

`--check` 可以挂进 CI 当门禁，但要为此在 secret 里放一个高权限 token——为两行元数据不值当，
默认没有接。

回归：`bash scripts/e2e-repo-metadata.sh`。它起一个本地 mock（`scripts/mock-gh-meta-server.mjs`，
按真实语义区分 contents / administration 权限），跑 8 条断言：dry-run 零写入、漂移检测、缺 token、
403 指引、应用成功、读回一致、幂等、只改 topics 时不动 description。不联网、不需要真 token，
也不会碰到真实仓库。其中"读回一致"这条是防回归的关键：请求体一旦按 GBK 发出去、或读回时把值里的
`\"` 当成字符串结尾，表现都是**服务端 200 但内容错/永远判定有差异**，属于静默污染。

---

## 10. LLBDS 测试环境搭建（本机实测步骤）

官方 `www.minecraft.net` 在本机不可达（DNS 解析到 `23.217.205.12`，TCP 超时），
故改为用 **lip 的 `github.com/LiteLDev/bds` 依赖**从 GitHub 取 BDS 本体：

```bash
# lip 0.34.7 已存在；BDS 作为 LeviLamina 的依赖被一并安装（约 436 MiB）
cd <workspace>/llbds
lip install github.com/LiteLDev/LeviLamina
```

安装结果（实际产物）：

```
llbds/
├── bedrock_server_mod.exe     232 MiB  ← LeviLamina 打过补丁的服务端
├── bedrock_server.exe.bak              ← 原版备份
├── PreLoader.dll / PeEditor.exe / bdsdown.exe
├── LeviLamina.dll (mods/LeviLamina)
├── mods/ config/ resource_packs/ behavior_packs/ data/ definitions/
└── tooth_lock.json            ← 依赖锁：bds 1.26.60 / LeviLamina 26.60.2
```

启动验证（`./bedrock_server_mod.exe`）：

```
INFO [PreLoader] Preloading: LeviLamina <LeviLamina.dll>
INFO [PreLoader] LeviLamina Loaded.
IFO [LeviLamina] 正在加载模组…… / 已加载 0 个模组
IFO [Server] Version: 1.26.60.6(ProtocolVersion 2225) with LeviLamina-26.60.2
IFO [Server] Server started in (3.2s)!
```

插件落位（`mods/packrelay/`），配置与状态：

```
llbds/mods/packrelay/{manifest.json, packrelay.mod.dll, config/config.json}
llbds/.mcpack-sync/{state.json, cdn.json, repo/}
llbds/cdn_config.json          ← 由 bdsCdnConfig sink 生成（扁平 map）
llbds/logs/packrelay.log
```

> `config.json` 的实际路径是 `mods/packrelay/config/config.json`（`NativeMod::getConfigDir()`），
> 相对路径（`resourceDir`、`.mcpack-sync/*`、`cdn_config.json`）全部以**服务器根目录**为基准。

---

## 11. 验证结果

### 11.1 单元测试 —— 183/183

覆盖 SHA-256 已知向量与分块一致性、JSON 往返/非法输入/键序保持、通配符（含 `**` 跨目录）、
版本与 slug、镜像模板与选择策略（含 ghfast 前缀拼接与尾斜杠收敛）、配置默认值与校验、
扫描/比对全矩阵（新增→未变→改内容→改版本→删除）、状态原子写往返、git 凭据解析与脱敏、
BDS 侧 `packId -> url` 映射与幂等 upsert、`cdn_config.json` 扁平格式回归。

```
$ ./build/mcpsync-tests
183 passed, 0 failed
```

### 11.2 端到端 —— 36/36（真实 git + 真实进程 + 模拟 Actions）

`node scripts/e2e.mjs`：用真实 Kaleidoscope 资源包（1980 文件 / 19.1 MiB）跑完整链路，
本地裸仓库充当 GitHub，`scripts/ci-release.sh` 由"CI watcher"在检测到新 commit 时并发执行。

| 阶段 | 验证点 | 结果 |
|---|---|---|
| 首轮 | 扫描识别新增包、push 成功 | PASS |
| | 仓库已预置 `release.yml` + `ci-release.sh` | PASS |
| | `index.json` 含 uuid / 版本 / 预渲染 tag+asset | PASS |
| | Actions 产出 release 元数据与 zip（CRC 校验通过，1980 条目） | PASS |
| | `cdn.json` 指向最新资产 | PASS |
| | `state.json` 指纹与索引一致、记录 commit | PASS |
| 二轮 | 判定 `unchanged=1`，远端 HEAD 不变（**无重复提交**） | PASS |
| 三轮 | 改 `pack_icon.png`（版本号不变）→ 判定 `updated=1` | PASS |
| | 产生新 commit、新 tag、CDN 地址切换到新 release | PASS |
| CDN | `redirect` 服务 `/health` 200、`/pack` 302 → Location=最新资产 | PASS |
| | 跟随 302 实际下载到合法 zip（PK 头） | PASS |

```
36 passed, 0 failed
```

> 该脚本会先清空 `e2e/` 目录再重建，本轮回归时这一步被环境策略拦下，未重跑；
> 上表是最近一次通过的实跑记录。本轮改动的 4 处都在单测与真实 BDS 运行里覆盖到了（见 11.3 / 11.4）。

### 11.3 真实 BDS 消费 CDN 配置 —— `cdn_config.json` 是**扁平**的

以插件生成的 `cdn_config.json` 启动 `bedrock_server_mod.exe`，对比三种输入：

| `cdn_config.json` | BDS 日志 |
|---|---|
| 不存在 | `No CDN config file found at: cdn_config.json for dedicated server` |
| 非法 JSON | `ERR [Server] Failed to parse CDN config` |
| **插件生成** | **无任何 cdn 错误行**（被正常读取解析） |

这只能证明文件进了 `CDNConfig::_readConfigFile` 的解析路径。它的**结构语义**是靠下面
这条运行时证据反推出来的——启动后把 BDS 内存里的 `mPackCDNUrls` 打出来：

```
# 早期写法 {"CDN URL": "...", "packs": [{...}]}
cdn: pushed 12 pack(s) into BDS CDNConfig, mPackCDNUrls now 14 entries
cdn:   [CDN URL] -> http://127.0.0.1:19134/pack
cdn:   [packs] ->                                   ← 数组被退化成空串
cdn:   [0575c61f-a5da-4b7f-9961-ffda2908861e] -> https://ghfast.top/https://github.com/...
```

`("packs", "")` 这条凭空出现的记录说明：`_readConfigFile()` 把**顶层 JSON 对象的每个成员
按 key→string 塞进 `vector<pair<string,string>>`，非字符串值填空串**——`packs` 数组根本不被识别。
所以磁盘格式必须是扁平的：

```json
{
  "CDN URL": "",
  "0575c61f-a5da-4b7f-9961-ffda2908861e": "https://ghfast.top/https://github.com/o/r/releases/download/t/a.zip",
  "0575c61f-a5da-4b7f-9961-ffda2908861e_1.26.60": "https://ghfast.top/https://github.com/o/r/releases/download/t/a.zip"
}
```

改为扁平格式后重跑，垃圾条目消失：

```
cdn: pushed 12 pack(s) into BDS CDNConfig, mPackCDNUrls now 13 entries
cdn:   [CDN URL] ->
cdn:   [0575c61f-...] -> https://ghfast.top/https://github.com/...
```

13 = 12 个包 + 1 个遗留前缀键，干净。这条回归已固化为单元测试 `testBdsCdnConfigFile`
（断言顶层没有 `packs`、没有任何数组值、两种主键都在且值为含加速前缀的完整地址）。

### 11.4 mod 在真实 LLBDS 上加载、写 BDS 内存，并打通真实 GitHub 闭环

`lip install github.com/LiteLDev/LeviLamina` 环境下，`bedrock_server_mod.exe` 实跑，
并用一个极小测试包（`packrelay-probe`，3 个文件）跑通了**真实 GitHub 全链路**：

```
IFO [LeviLamina] 正在加载 packrelay v1.0.0
IFO [packrelay] loading packrelay, server root = .
IFO [packrelay] command symbol provider: SymbolProvider (linked via levilamina package)
IFO [LeviLamina] 已加载 1 个模组
IFO [packrelay] packrelay console commands registered (/relay, /relaygit)
IFO [Server] scan requested (async)
IFO [packrelay] pipeline: 1 pack(s), 3 file(s), 5 KiB
IFO [packrelay] git: staged 3 file(s) -> packs\packrelay-probe
IFO [packrelay] git: pushed 18:55:10 to origin/main                    ← 真实 push 到 GitHub
IFO [packrelay] release packrelay-probe-v1.0.0-fdd289e0 is ready after 1 poll(s)
IFO [packrelay] cdn: published packrelay-probe ->
    https://ghfast.top/https://github.com/chm413/test/releases/download/packrelay-probe-v1.0.0-fdd289e0/....zip
IFO [packrelay] cdn: pushed 1 pack(s) into BDS CDNConfig, mPackCDNUrls now 2 entries
IFO [packrelay] pipeline: done in 5948ms                               ← 整条链 5.9 秒
IFO [Server]   c2f55d81-..._1.0.0 -> https://ghfast.top/https://github.com/chm413/test/...
Quit correctly                      ← exit code 0，无崩溃、无 minidump
```

幂等验证（内容不变再扫一次）：

```
IFO [Server] OK packs=1 files=3 added=0 updated=0 removed=0 unchanged=1
IFO [Server]   - [unchanged] packrelay-probe ok : uuid+version+fileSetHash identical
                 commit=18:55:10 tag=packrelay-probe-v1.0.0-fdd289e0 cdn=https://ghfast.top/...
```

下载验证：release zip 直连 200、经 `ghfast.top` 镜像 200，`zipfile.testzip()==None`、PK 头。

也就是说「改磁盘文件不生效、必须写 BDS 内存」这条要求已经真正打通：
`getServerInstance()` → `ServerInstance::mCDNConfig.get().get()` → `CDNConfig::mPackCDNUrls`
拿到的是**活对象**，写入即时生效，不需要重启。

同时修掉了四个本轮定位到的缺陷：

| 缺陷 | 后果 | 根因 |
|---|---|---|
| 关闭时崩溃 `0xC0000409` | 每次停服都生成 minidump、被崩溃上报 | `std::thread` 成员仍是 joinable 就进了静态析构 → `std::terminate` → `abort`。崩溃栈：`~McpackSyncMod → std::thread::~thread → ucrtbase(int 0x29)` |
| `git add -A` 在 BDS 内卡满 60s 超时 | 推送链路完全不可用（standalone 0.2s） | 子进程继承了 BDS 的 stdin 管道读端，卡在启动期等待。修复：无输入时把子进程 stdin 指向 NUL 设备 |
| 插件铺的 `release-packs` workflow 失败 | release 永远建不出来，判据断在「等 release」 | `ci-release.sh` 先 `mkdir ./.releases/<tag>` 再 `cd packs/<slug>` 后用**相对路径** zip——cd 后相对路径解析到包目录里去了。e2e 没抓到是因为 `--fs` 模式传的是绝对路径。修复：cd 前转绝对路径 |
| `/relay` 永远 `Unknown command` | 控制台指令不可用 | `LoadLibraryW("bedrock_runtime.dll")` 探测永远为空（它不是磁盘文件），命令注册被永久跳过；且手写的 `__pfnDliNotifyHook2` 顶掉了 SymbolProvider。修复：删探测、删手写 hook，无条件注册 + try/catch 兜底 |
| `scanOnWorldLoad` / `shutdownTimeoutMs` 配了不生效 | 文档承诺的功能是空的 | 前者只解析没接线，后者只解析没使用。修复：世界就绪改用 `getLevel()` 轮询探测（本机 LL 上 `EventBus::addListener` 对 `ServerStartedEvent` 直接返回 `false`，事件永不派发，故不能用事件）；停服改为可中断轮询 + 有界等待，超时则故意泄漏 Pipeline 以免工作线程访问已析构对象 |
| `cdn.mode` 三个模式只有一种行为 | `static` / `staticUrl` 配了没用，`mode` 写错也不报错 | `mode` 全程没有被分支，`staticUrl` 只出现在校验里。修复：`static` 走 `staticUrl + "/" + assetName` 且不等 Release；非法取值在 `validateConfig` 拒绝；`scan`/`watch` 补上校验（原先只有 `once` 和 mod 校验） |

### 11.5 真实资源包：Kaleidoscope Tavern v1.0.3（1980 文件 / 19.1 MB）

前面用的 probe 包只有 3 个文件，名字也是纯文本，绕开了真实资源包的两个特征：
**体量**（近 2000 个文件）和 **i18n 名字**（`header.name` 是 `pack.name` 这类翻译键）。

用真实包重跑，第一条就暴露了问题：包名、slug、tag、CDN 清单里全是 `pack.name` / uuid——
`texts/*.lang` 里的真名从来没被读过。修复后（`scanner.cpp` 的 `resolveLangValue`，
语言按 en_US → en_GB → zh_CN → zh_TW → 其它 排序，跳过 `##` 注释行）整条链才可读：

| 阶段 | 结果 |
|---|---|
| 扫描 | 1 pack / 1980 files / 19148 KiB / 3.9s |
| slug | `kaleidoscope-tavern-v1.0.3-rp`（修复前是 `541650da-507a-...`） |
| tag | `kaleidoscope-tavern-v1.0.3-rp-v1.0.3-ae24301a` |
| 全量发布 | 47s（扫描 → push → 等 Release → 写 CDN） |
| Release 资产 | 4,580,878 B（19.1 MB 压到 4.4 MB） |
| zip 内容 | 1980 文件 / 19,607,824 B，抽样 sha256 与源目录逐字节一致 |
| 二次运行 | `unchanged=1`，3.1s，不重复 push、不重复发版 |
| 增量（改 1 个 lang） | `updated=1`，42s，新 tag `...-300e00eb`，**目录名不变** |
| 镜像 | 直连 302、`ghfast.top` 200，下载 4,580,917 B（正好多出新增的 39 字节） |

增量那一行是关键：slug 来自 `header.name`，而真实包名里带版本号。若 slug 跟着版本走，
每次升级都会在仓库里多一个目录、旧目录永远留着。现有实现靠 `state.json` 里记的旧 slug
在更新时沿用（`pipeline.cpp`），所以目录稳定；这条行为有单元测试锁着，不要"顺手优化"。

> 实测用 `git.authMode=token_env` + `MCPACK_GIT_TOKEN`，走 HTTPS 的 `http.extraheader`
> 注入 basic 认证，不落盘、不依赖系统凭据管理器。

真机验证（LLBDS 26.60.2 / BDS 1.26.60.6，用的是 CI 产出的 mod DLL）：

```
IFO [packrelay] state loaded: 1 pack(s) from .\.mcpack-sync/state.json
IFO [packrelay] cdn: restored 1 target(s), redirect -> .../kaleidoscope-tavern-v1.0.3-rp-.../....zip
IFO [packrelay] packrelay ready: resourceDir=.\../.tmp/real/packs, git.enabled=true
IFO [packrelay] pipeline: 1 pack(s), 1980 file(s), 19148 KiB, 963ms
IFO [packrelay] cdn: pushed 1 pack(s) into BDS CDNConfig, mPackCDNUrls now 5 entries
IFO [packrelay] cdn:   [541650da-507a-473a-b2f1-162103ffc351_1.0.3] -> https://ghfast.top/...
IFO [Server]   541650da-... v1.0.3 (ghfast) -> https://ghfast.top/...      ← BDS 自己解析出来的
IFO [Server] OK packs=1 files=1980 added=0 updated=0 removed=0 unchanged=1 elapsed=963ms
IFO [Server] cdn kaleidoscope-tavern-v1.0.3-rp -> https://ghfast.top/... (ghfast)   ← /relay cdn
Quit correctly
```

`mPackCDNUrls now 5 entries` 里有两对是 probe 包的残留（BDS 的 `cdn_config.json` 还留着），
本次真正写进去的是 `541650da-..._1.0.3` 那一条。

> 一个容易踩的点：mod 的 `reload()` 会把 `baseDir` **强制覆盖成服务器根目录**
> （避免 mod 重装后状态漂移），所以配置文件里写绝对 `baseDir` 不生效；
> `resourceDir` 只能用相对服务器根的路径（如 `../packs`）。

---

## 12. 已知限制

1. **mod 目标需 MSVC**：见 §9，MinGW 无法链接 LL 的 MSVC 修饰符号。core/CLI 全部可用 g++ 构建并已验证。
2. **`cdn_config.json` 已确认是扁平 `map<string,string>`**（见 §11.3，靠运行时反推出的结论，
   不是文档）。字段名本身不再是"猜测"，但 `"CDN URL"` 这个遗留前缀键的具体语义仍未确认——
   我们只把它当占位保留，实际地址全部写在 `<uuid>` / `<uuid>_<version>` 键里。
3. **写内存依赖 BDS 布局**：`ServerInstance::mCDNConfig` 是 public 成员（`ServerInstance.h` 第 191 行的
   `public:` 段覆盖到第 235 行），`CDNConfig::mPackCDNUrls` 也是 public，因此可直接写入；
   但这属于二进制布局依赖，**BDS 大版本升级后要用 `/relay dump` 复验一次**。
   若哪天成员被收回私有，可退回"拼地址 + 写 `cdn_config.json` + 重启生效"的纯文件路线。
4. **`mPackCDNUrls` 的主键格式仍未确认**：我们同时写 `<uuid>_<version>` 与裸 `<uuid>` 两种键；
   键不匹配时不会崩溃，只是该包会退回服务端直传。用 `/relay dump` 可看到 BDS 实际持有的键。
5. **控制台指令依赖 SymbolProvider（链接期能力，不是环境依赖）**。
   命令 API（`CommandOutput::success` / `CommandRegistry::parse` / `Bedrock::type_id`）的导入由
   `prelink` 生成的 `bedrock_runtime_api.lib` 指向 `bedrock_runtime.dll`，而后者不是磁盘文件，
   是 prelink 写入的假模块名；真正把它解析到 `bedrock_server_mod.exe` 真实地址的是
   **SymbolProvider**（它替换了 `__delayLoadHelper2`，内部调 `ll::memory::resolveSymbol`）。

   SymbolProvider 是 `levilamina` 包的传递依赖（`symbolprovider v1.3.0`），xmake 构建自动链入
   （CI 产物的 pdb 里可验证）。所以：

   - 插件**不做运行期探测**。早期版本用 `LoadLibraryW("bedrock_runtime.dll")` 探测，永远返回空，
     导致 `registerCommands()` 被永久跳过、`/relay` 全部 `Unknown command`——那是个错误结论。
   - 也**不要手写 `__pfnDliNotifyHook2`**：SymbolProvider 已经实现了它，重复定义会 `LNK2005`，
     且会把正确的解析顶掉（这正是 `0xC06D007E` 的真正来源）。
   - 命令注册包在 `try/catch(...)` 里（xmake 用 `/EHa`，也能接住 SEH），最坏情况是
     「命令不可用 + 一条 WARN」，不会把服务器带走；**扫描 / git 推送 / release / 写 BDS 内存
     全部不经过这条路**，不受影响。

   同等能力随时可用 CLI：`packrelay git remote|token|test`、`packrelay cdn show|render`、
   `packrelay scan|once`。
6. 单进程单实例：`.mcpack-sync/` 目录未加文件锁，同一服务器根目录不要同时跑两个 `packrelay`。
7. **仓库 description / topics 不受版本控制**：见 §9.3。写它们要 Administration:write，
   `git push` 用的是 contents:write，两套权限不通用，流水线也带不动它。
   内容已声明在 `scripts/repo-metadata.conf`，用 `scripts/sync-repo-metadata.sh` 幂等应用；
   `--check` 可以对线上做只读比对，用于确认"声明"和"实际"没有漂移。
