// mcpack-sync : 目标仓库模板落地
//
// 插件首次 push 前会把 workflow 与发布脚本写入目标仓库并一并提交，
// 保证"仓库预置 GitHub Actions"这条链路不依赖人工操作。
// 模板内容以字符串内嵌，避免运行期依赖安装路径。
#include "mcpsync/repo_template.hpp"
#include "mcpsync/fsutil.hpp"
#include "mcpsync/logger.hpp"
#include "mcpsync/sha256.hpp"

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace mcpsync {

namespace {

namespace fs = std::filesystem;

char const* kWorkflow = R"MCPACKTEMPLATE(# mcpack-sync: 由插件自动生成，请勿手工修改（升级插件时会被覆盖）
name: release-packs

on:
  push:
    branches: [ "main" ]
    paths:
      - "packs/**"
      - "index.json"
  workflow_dispatch:

permissions:
  contents: write

concurrency:
  group: release-packs-${{ github.ref }}
  cancel-in-progress: false

jobs:
  release:
    runs-on: ubuntu-latest
    steps:
      - name: Checkout
        uses: actions/checkout@v4
        with:
          fetch-depth: 2        # 需要 HEAD~1 做变更识别

      - name: Detect changed packs and publish releases
        env:
          GH_TOKEN: ${{ secrets.GITHUB_TOKEN }}
          GITHUB_REPOSITORY: ${{ github.repository }}
        run: bash scripts/ci-release.sh --gh
)MCPACKTEMPLATE";

char const* kReleaseScript = R"MCPACKTEMPLATE(#!/usr/bin/env bash
# mcpack-sync: 资源包发布脚本（与 .github/workflows/release.yml 同源）
#
# 职责：
#   1) 识别本次 push 改了哪些 packs/<slug>
#   2) 从 index.json 读取插件预先渲染好的 tag / assetName（保证与插件取回的名字一致）
#   3) 打包为 zip
#   4) 创建 GitHub Release（--gh），或落地到本地目录（--fs，供离线端到端验证）
#
# 用法：
#   bash scripts/ci-release.sh --gh
#   bash scripts/ci-release.sh --fs [输出目录]
set -euo pipefail

MODE="${1:---gh}"
OUT_DIR="${2:-./.releases}"

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_dir"

json_field() { # json_field <json> <field>
  node -e 'const o=JSON.parse(process.argv[1]);process.stdout.write(String(o[process.argv[2]]??""))' "$1" "$2"
}

lookup_pack() { # lookup_pack <slug> -> index.json 中的条目 JSON
  node -e '
    const fs=require("fs");
    const slug=process.argv[1];
    if(!fs.existsSync("index.json")) process.exit(0);
    const idx=JSON.parse(fs.readFileSync("index.json","utf8"));
    const p=(idx.packs||[]).find(x=>x.slug===slug);
    if(p) process.stdout.write(JSON.stringify(p));
  ' "$1"
}

# rev-parse 不带 --verify 时会把非法修订原样回显到 stdout，必须加 --verify
prev="$(git rev-parse --verify -q "HEAD~1" 2>/dev/null || true)"
if [ -z "$prev" ]; then
  # 首次提交没有 HEAD~1：直接列 packs/ 下的一级目录
  changed="$(git ls-tree -d --name-only "HEAD:packs" 2>/dev/null || true)"
else
  changed="$(git diff --name-only "$prev" HEAD -- packs | awk -F/ 'NF>1{print $2}' | sort -u)"
fi

if [ -z "${changed:-}" ]; then
  echo "::notice::no pack changes in this push"
  exit 0
fi

echo "changed packs: $(echo "$changed" | tr '\n' ' ')"
mkdir -p "$OUT_DIR"

for slug in $changed; do
  meta="$(lookup_pack "$slug")"
  if [ -z "$meta" ]; then
    echo "::warning::slug '$slug' not found in index.json, skipped"
    continue
  fi

  tag="$(json_field "$meta" tag)"
  asset="$(json_field "$meta" assetName)"
  if [ -z "$tag" ] || [ -z "$asset" ]; then
    echo "::warning::missing tag/assetName for '$slug', skipped"
    continue
  fi

  zip_path="$OUT_DIR/$tag/$asset"
  mkdir -p "$OUT_DIR/$tag"
  rm -f "$zip_path"

  # 下面要 cd 进包目录打包，相对路径 $zip_path 会解析到包目录下面去（实测踩坑：
  # "./.releases/<tag>/<asset>.zip" 在 cd 后变成 packs/<slug>/.releases/...，zip 直接
  # "Could not create output file"）。先把输出路径转成绝对路径再进子 shell。
  zip_abs="$(cd "$(dirname "$zip_path")" && pwd)/$(basename "$zip_path")"

  # 优先系统 zip；否则用 mcpack-sync 自带的 store 打包器；最后退回 tar
  if command -v zip >/dev/null 2>&1; then
    ( cd "packs/$slug" && zip -qr "$zip_abs" . )
  elif command -v mcpsync >/dev/null 2>&1; then
    mcpsync package --input "packs/$slug" --output "$zip_abs" >/dev/null
  else
    ( cd "packs/$slug" && tar --force-local -a -c -f "$zip_abs" . )
  fi

  size="$(wc -c < "$zip_abs" | tr -d ' ')"
  sha="$(sha256sum "$zip_abs" | cut -d' ' -f1)"
  echo "packaged $slug -> $zip_path (${size} bytes, sha256=$sha)"

  if [ "$MODE" = "--gh" ]; then
    if gh release view "$tag" >/dev/null 2>&1; then
      echo "::notice::release $tag already exists, uploading asset if missing"
      gh release upload "$tag" "$zip_abs" --clobber
    else
      gh release create "$tag" "$zip_abs" \
        --title "$tag" \
        --notes "Automated release for \`$slug\` by mcpack-sync." \
        --latest
    fi
  else
    base="${MCPACK_RELEASE_BASE_URL:-http://127.0.0.1:8731/downloads}"
    node -e '
      const fs=require("fs");
      const [tag,asset,size,url,out,slug]=process.argv.slice(1);
      const published=new Date().toISOString().replace(/\.[0-9]+Z$/,"Z");
      const rel={
        tag_name:tag, name:tag, html_url:url, published_at:published,
        draft:false, prerelease:false,
        body:"Automated release for "+slug+" by mcpack-sync.",
        assets:[{name:asset, browser_download_url:url, size:Number(size), updated_at:published}]
      };
      fs.writeFileSync(out, JSON.stringify(rel,null,2));
    ' "$tag" "$asset" "$size" "$base/$tag/$asset" "$OUT_DIR/$tag/release.json" "$slug"
    echo "wrote $OUT_DIR/$tag/release.json"
  fi
done
)MCPACKTEMPLATE";

char const* kRepoReadme = R"MCPACKTEMPLATE(# Resource pack releases

本仓库由 **mcpack-sync** 自动维护：

- `packs/<slug>/` — 资源包全量文件（由服务端插件从资源目录镜像而来）
- `index.json` — 资源包清单（uuid / version / 内容哈希 / tag / asset 名）
- `.github/workflows/release.yml` — 收到 push 后自动打包并创建 Release

> `packs/` 与 `index.json` 均为机器生成内容，请勿手工编辑；
> 需要修改资源请改动源资源目录，插件会自动提交。

## 发布流程

1. 插件扫描资源目录，按 `uuid + version + 全量文件哈希` 判定变更
2. 变更写回本仓库并 push
3. GitHub Actions 识别变更 → 打 zip → 创建 Release（tag 由插件在 `index.json` 中预渲染）
4. 插件轮询 Release，取回资产地址并更新 CDN 重定向目标
)MCPACKTEMPLATE";

// 模板写入策略（按内容哈希，不依赖人工维护版本号）：
//   .packrelay/templates.lock 记录「插件上次写进去的内容哈希」。
//   - 文件不存在                -> 写入模板
//   - 内容与模板一致            -> 什么都不做（不产生空 diff）
//   - 文件哈希 == lock 里的哈希 -> 文件仍是插件写的，本次是模板升级 -> 备份后覆盖
//   - 否则（哈希对不上/无 lock）-> 判定为本地改动 -> 保留并 warn
//   - 无 lock 但文件已存在      -> 首次接管：备份后覆盖（保证 Actions 链路开箱可用）
char const* kLockRel      = ".packrelay/templates.lock";
char const* kBackupDirRel = ".packrelay/backup";

std::string contentHash(std::string_view data) {
    return Sha256::hex(data);
}

struct TemplateLock {
    std::vector<std::pair<std::string, std::string>> entries;

    std::string find(std::string const& key) const {
        for (auto const& [k, v] : entries) {
            if (k == key) return v;
        }
        return {};
    }
    void set(std::string key, std::string value) {
        for (auto& [k, v] : entries) {
            if (k == key) {
                v = std::move(value);
                return;
            }
        }
        entries.emplace_back(std::move(key), std::move(value));
    }
};

TemplateLock readLock(fs::path const& p) {
    TemplateLock lock;
    std::string  text;
    if (!fsu::exists(p) || !fsu::readFile(p, text)) return lock;

    size_t pos = 0;
    while (pos <= text.size()) {
        size_t end  = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        auto line = text.substr(pos, end - pos);
        auto eq   = line.find('=');
        if (eq != std::string::npos && eq > 0) {
            lock.entries.emplace_back(line.substr(0, eq), line.substr(eq + 1));
        }
        pos = end + 1;
    }
    return lock;
}

std::string renderLock(TemplateLock const& lock) {
    std::string out;
    for (auto const& [k, v] : lock.entries) {
        out += k;
        out += '=';
        out += v;
        out += '\n';
    }
    return out;
}

// 落地单个模板文件，返回 true 表示本次写入了新内容
Result<bool> applyTemplate(
    fs::path const&   path,
    std::string_view  content,
    std::string_view  label,
    TemplateLock&     lock,
    fs::path const&   backupDir
) {
    std::string const wantHash = contentHash(content);

    if (!fsu::exists(path)) {
        auto st = fsu::writeFileAtomic(path, content);
        if (!st) return st.error();
        lock.set(std::string(label), wantHash);
        return true;
    }

    std::string existing;
    auto        rd = fsu::readFile(path, existing);
    if (!rd) return rd.error();
    if (existing == content) {
        lock.set(std::string(label), wantHash);
        return false; // 已是当前模板，零改动
    }

    bool const knownTemplate = !lock.find(std::string(label)).empty();
    bool const untouched     = lock.find(std::string(label)) == contentHash(existing);

    if (knownTemplate && !untouched) {
        // 上次写入后文件被改过：视为用户自己的定制，绝不覆盖
        logger().warn(
            "repo templates: " + std::string(label) +
            " was modified locally and is kept as-is (delete the file to restore the built-in template)"
        );
        return false;
    }

    // 备份到 .packrelay/backup/（该目录自带 .gitignore），避免旧模板被 git add -A 提交
    auto bak = backupDir / (path.filename().string() + ".bak");
    auto st  = fsu::writeFileAtomic(bak, existing);
    if (!st) return st.error();
    logger().info(
        std::string(knownTemplate ? "repo templates: upgraded " : "repo templates: adopted ") + std::string(label) +
        " (previous copy saved to " + std::string(kBackupDirRel) + "/" + path.filename().string() + ".bak)"
    );
    st = fsu::writeFileAtomic(path, content);
    if (!st) return st.error();
    lock.set(std::string(label), wantHash);
    return true;
}

} // namespace

Status ensureRepoTemplates(std::filesystem::path const& repoRoot) {
    auto workflowPath = repoRoot / ".github" / "workflows" / "release.yml";
    auto scriptPath   = repoRoot / "scripts" / "ci-release.sh";
    auto readmePath   = repoRoot / "README.md";
    auto lockPath     = repoRoot / fsu::fromUtf8(kLockRel);
    auto backupDir    = repoRoot / fsu::fromUtf8(kBackupDirRel);

    auto st = fsu::ensureDir(workflowPath.parent_path());
    if (!st) return st;
    st = fsu::ensureDir(scriptPath.parent_path());
    if (!st) return st;
    st = fsu::ensureDir(backupDir);
    if (!st) return st;
    // 备份目录不进仓库：否则升级留下的旧模板会被 git add -A 一起提交
    auto ignorePath = backupDir / ".gitignore";
    if (!fsu::exists(ignorePath)) {
        st = fsu::writeFileAtomic(ignorePath, "*\n");
        if (!st) return st;
    }

    auto lock = readLock(lockPath);

    auto wf = applyTemplate(workflowPath, kWorkflow, "workflow", lock, backupDir);
    if (!wf) return wf.error();
    auto sc = applyTemplate(scriptPath, kReleaseScript, "script", lock, backupDir);
    if (!sc) return sc.error();

    if (!fsu::exists(readmePath)) {
        auto rd = fsu::writeFileAtomic(readmePath, kRepoReadme);
        if (!rd) return rd;
    }

    // 记录本次落地的哈希；下次运行据此区分「插件升级」与「本地改动」
    st = fsu::writeFileAtomic(lockPath, renderLock(lock));
    if (!st) return st;

    logger().debug("repo templates ensured at " + fsu::toUtf8(repoRoot));
    return {};
}

} // namespace mcpsync
