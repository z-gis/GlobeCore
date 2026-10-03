# GlobeCore HarmonyOS HAR 发布指南（ohpm）

本文件记录把鸿蒙渲染引擎 HAR 包 `@zys/globecore` 发布到 **OpenHarmony 三方库中心仓**（`https://ohpm.openharmony.cn`）的完整流程。

> 说明：HAR 属于 ohpm 生态，**不能发布到 Maven**（Maven 只解析 JVM/AAR 坐标，DevEco/ohpm 无法从 Maven 仓按坐标拉取 `.har`）。Android 侧 AAR 走 Maven（见 `操作说明.md` / `.github/workflows/publish-maven.yml`），鸿蒙侧 HAR 走本文的 ohpm 流程，两套并行。

- 发布目标源：`https://ohpm.openharmony.cn/ohpm/`（默认已在 `~/.ohpm/.ohpmrc` 的 `publish_registry`）
- 包名：`@zys/globecore`（作用域名 = 已认证组织 `zys`）
- 版本号：**三段式 semver**（如 `0.1.1`）。四段式 `0.1.1.1` 会被 ohpm 判为非法版本；与 Android 的四段版本无法一一对应，取 `x.y.z` 对齐。

---

## 一、一次性准备（换机器/换账号才需重做）

### 1. 注册 + 组织认证
1. 访问 https://ohpm.openharmony.cn/ ，用 OpenHarmony 账号登录并完成**实名认证**。
2. **个人中心 → 组织管理 → 新增组织**：组织名填 `zys`（须与包名 scope 完全一致，小写），**描述必填**，提交并等待审核**通过**。
   - 包名规则为 `@组织名/组件名`；组织未认证或你不在该组织 developer 列表内，发布会报 `Failed to verify the OHPM package group`。

### 2. 生成 SSH 密钥对并上传公钥
```powershell
ssh-keygen -m PEM -t RSA -b 4096 -f C:\Users\<你>\.ohpm\ohpm_key
```
- **`-m PEM` 必须**：ohpm 只认 PEM 格式 RSA 私钥，否则报 `Not supported private key`。
- **passphrase 必须非空**：留空发布时报 `must config an encrypted private key using a non-empty passphrase`。此密码即发布时要输入的密码。
- 生成两文件：`ohpm_key`（私钥，保密不外传）、`ohpm_key.pub`（公钥）。
- 复制公钥内容，粘贴到 **个人中心 → 认证管理 → 新增**（标题任意）保存：
  ```powershell
  Get-Content C:\Users\<你>\.ohpm\ohpm_key.pub
  ```

### 3. 配置发布参数
在 **个人中心头像下方** 复制**发布码（publish_id）**，然后：
```powershell
$env:Path = 'D:\Program Files\Huawei\DevEco Studio\tools\ohpm\bin;D:\Program Files\Huawei\DevEco Studio\tools\node;' + $env:Path
ohpm config set key_path   C:\Users\<你>\.ohpm\ohpm_key
ohpm config set publish_id 你的发布码
```

---

## 二、每次发布：构建 release HAR

> 关键点：**必须用 release 模式**（`buildMode=release`）。debug HAR 含 ArkTS 源码，会泄露代码且审核不友好。

```powershell
cd D:\Android\MobileMap\GlobeCore\globecore-harmonyos
$env:Path = 'D:\Program Files\Huawei\DevEco Studio\tools\node;' + $env:Path
$env:DEVECO_SDK_HOME = 'D:\Program Files\Huawei\DevEco Studio\sdk'

# 若改过 native target 名 / 删过库，先 clean，避免残留孤儿 .so（如历史 libworldwind.so）被打包
& 'D:\Program Files\Huawei\DevEco Studio\tools\hvigor\bin\hvigorw.bat' clean --no-daemon

# 打 release HAR，并由 createVersionedHar 任务产出版本化文件名
& 'D:\Program Files\Huawei\DevEco Studio\tools\hvigor\bin\hvigorw.bat' `
  --mode module -p module=globecore@default -p product=default -p buildMode=release `
  createVersionedHar --no-daemon
```

产物（`globecore-harmonyos\library\build\default\outputs\default\`）：
- `globecore.har`（hvigor 默认产物，按模块名命名）
- `@zys-globecore-<version>.har`（版本化副本，`hvigorfile.ts` 里 `createVersionedHar` 任务生成；作用域 `/` 替换为 `-`）

**发布前自检**（可选但推荐）：
```powershell
# 解开 har 看包内是否含 LICENSE/README/CHANGELOG，libs 是否只有 libglobecore.so + libc++_shared.so
tar -xzf '.\@zys-globecore-0.1.1.har' -C $env:TEMP\gc_check
Get-ChildItem "$env:TEMP\gc_check\package" -File | Select-Object Name
Get-ChildItem "$env:TEMP\gc_check\package\libs" -Recurse -File | Select-Object FullName
```

---

## 三、发布到中心仓

```powershell
$env:Path = 'D:\Program Files\Huawei\DevEco Studio\tools\ohpm\bin;D:\Program Files\Huawei\DevEco Studio\tools\node;' + $env:Path
ohpm publish 'D:\Android\MobileMap\GlobeCore\globecore-harmonyos\library\build\default\outputs\default\@zys-globecore-0.1.1.har'
# 提示 what is your passphrase of the private key: → 输入第 1 步设置的 passphrase（不回显）
```

成功标志：
```
+@zys/globecore 0.1.1
Thanks for your contribution, the submitted OHPM library is under review ...
```
提交后进入**人工审核**（工作日数分钟到数小时）。**审核上架前**按名安装会 404，属正常。

---

## 四、发布格式校验清单（HttpCode 400 逐项排错）

服务端按固定顺序校验，任一不满足即失败。补齐后需**重打 release 包**再发（改动只在模块根 `library/`）：

| 报错关键词 | 原因 | 修复 |
|---|---|---|
| `Failed to verify the OHPM package group` | scope 对应组织未认证 / 你非其 developer | 见「一、1」建组织并等审核 |
| `no author url or author email` | `author` 是纯字符串 | `oh-package.json5` 的 `author` 改为 `{name,email,url}` 对象 |
| `repository ... must start with https` | `repository` 用了 `{type,url}` 对象 | 改为**字符串 URL**：`"repository": "https://....git"` |
| `must contain a non-empty license file` | 开源包缺 LICENSE | 模块根放 `LICENSE`（Apache-2.0 全文） |
| `must contain a non-empty changelog.md file` | 缺 CHANGELOG | 模块根放 `CHANGELOG.md` |
| （README 相关） | 缺 README | 模块根放 `README.md` |

> `hvigorw` 的 `PackageHar` 会自动收录模块根 `library/` 下的 `LICENSE`、`README.md`、`CHANGELOG.md`，新增后重打即进包，无需改打包配置。
> 版本占用：`name + version` 审核通过后**永久占用**，改元数据重发须**递增 version**。

---

## 五、CI 自动发布（可选）

工作流：`.github/workflows/publish-harmony.yml`。策略：**本地/手动把 `.har` 附到 GitHub Release → CI 只负责把现成 HAR 推到 ohpm**（runner 仅需 `@ohos/ohpm-cli`，不装 SDK/native，绕开 GDAL 现编）。

需在仓库 **Settings → Secrets and variables → Actions** 配置：
- `OHPM_PUBLISH_ID`：发布码
- `OHPM_KEY`：私钥文件完整内容（`-----BEGIN RSA PRIVATE KEY-----` 开头）
- `OHPM_KEY_PASSPHRASE`：私钥 passphrase

触发：发布一个携带 `.har` 附件的 GitHub Release，或手动 `workflow_dispatch` 填 tag。
> ⚠️ 该工作流尚未实跑验证；`expect` 匹配的提示文案与 `@ohos/ohpm-cli` 版本行为可能需按首次日志微调。

---

## 六、消费方安装（审核上架后）

```powershell
ohpm install @zys/globecore
```
或在 `oh-package.json5` 写：`"@zys/globecore": "0.1.1"`。首次使用需在 Ability 初始化时：
```ts
import { NativeSrs, NativeNet } from '@zys/globecore';
NativeSrs.initProjData(context);
NativeNet.initCaBundle(context);
```

---

## 附：关键环境说明
- 项目根**无 `hvigorw` wrapper**，须调用 DevEco 自带命令行：`D:\Program Files\Huawei\DevEco Studio\tools\hvigor\bin\hvigorw.bat`；且**必须**把 `tools\node` 加入 `Path`（该 bat 直接调 `node.exe`，否则报 `NODE_HOME is not set`）。
- hvigor 模块名 `globecore`（目录仍为 `library/`，二者独立）；构建命令用 `-p module=globecore@default`。
