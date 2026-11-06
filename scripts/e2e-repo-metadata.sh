#!/usr/bin/env bash
#
# 用本地 mock 回归 scripts/sync-repo-metadata.sh 的全部路径。
# 不联网、不需要真 token、不碰真实仓库。
#
#   bash scripts/e2e-repo-metadata.sh
#
# 覆盖：dry-run / check 漂移 / 缺 token / 403 无权限 / 正常应用 / 幂等 /
#       UTF-8 与 " \ \t 的 JSON 往返（这两个曾出过静默污染仓库的 bug）
#
set -uo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
SYNC="$SCRIPT_DIR/sync-repo-metadata.sh"
MOCK="$SCRIPT_DIR/mock-gh-meta-server.mjs"
PORT=${MOCK_META_PORT:-$((8800 + RANDOM % 200))}
BASE="http://127.0.0.1:$PORT"
ADMIN=admin-token
TMP=${TMPDIR:-/tmp}

PASS=0
FAIL=0
ok()   { PASS=$((PASS + 1)); printf '  ok   %s\n' "$*"; }
bad()  { FAIL=$((FAIL + 1)); printf '  FAIL %s\n' "$*"; }
head_() { printf '\n== %s\n' "$*"; }

MOCK_PID=''
cleanup() {
  [ -n "$MOCK_PID" ] && kill "$MOCK_PID" 2>/dev/null
  rm -f -- "$TMP/meta-e2e.conf" "$TMP/meta-e2e2.conf" 2>/dev/null || true
}
trap cleanup EXIT

command -v node >/dev/null || { echo "需要 node" >&2; exit 2; }
command -v curl >/dev/null || { echo "需要 curl" >&2; exit 2; }

MOCK_META_PORT=$PORT MOCK_META_ADMIN_TOKEN=$ADMIN node "$MOCK" >"$TMP/meta-e2e-mock.log" 2>&1 &
MOCK_PID=$!

for _ in $(seq 1 50); do
  curl -sS "$BASE/__state" >/dev/null 2>&1 && break
  sleep 0.1
done
curl -sS "$BASE/__state" >/dev/null 2>&1 || { echo "mock 起不来，见 $TMP/meta-e2e-mock.log" >&2; exit 2; }

cat >"$TMP/meta-e2e.conf" <<'EOF'
REPO_SLUG='chm413/packrelay'
REPO_DESCRIPTION='中文"引号"\反斜杠	制表符 与 → 箭头'
REPO_HOMEPAGE='https://example.com/x'
REPO_TOPICS='zeta alpha,zeta mid'
EOF

run() { # run <expect_exit> <conf> <token> <args...>  -> 结果写 $OUT/$ERR
  local expect=$1 conf=$2 token=$3
  shift 3
  OUT=$(PACKRELAY_META_CONF=$conf GITHUB_TOKEN=$token bash "$SYNC" --base-url "$BASE" "$@" 2>"$TMP/meta-e2e.err")
  RC=$?
  ERR=$(cat "$TMP/meta-e2e.err")
  if [ "$RC" = "$expect" ]; then return 0; else return 1; fi
}

reset_mock() { curl -sS -X POST "$BASE/__reset" >/dev/null; }
writes() { curl -sS "$BASE/__state" | sed -n 's/.*"dirtyWrites":\[\(.*\)\]}$/\1/p' | grep -o '"method"' | wc -l | tr -d ' '; }

head_ "1. dry-run 不写、不需要 token"
if run 0 "$TMP/meta-e2e.conf" '' --dry-run && [[ $OUT == *"PATCH $BASE"* ]] && [ "$(writes)" = 0 ]; then
  ok "打印请求体且零写入"
else
  bad "dry-run 行为不符（rc=$RC writes=$(writes)）"
fi

head_ "2. --check 空仓库报漂移"
if run 1 "$TMP/meta-e2e.conf" '' --check && [[ $OUT == *drift* ]]; then
  ok "exit 1 且输出 drift"
else
  bad "--check 未按预期报漂移（rc=$RC）"
fi

head_ "3. apply 缺 token"
if run 2 "$TMP/meta-e2e.conf" '' && [[ $ERR == *"需要 token"* ]]; then
  ok "exit 2 并提示设置 GITHUB_TOKEN"
else
  bad "缺 token 分支不符（rc=$RC）"
fi

head_ "4. apply 无权限 token（模拟 403）"
if run 2 "$TMP/meta-e2e.conf" 'readonly-token' && [[ $ERR == *Administration:write* ]]; then
  ok "exit 2 且给出权限指引"
else
  bad "403 分支不符（rc=$RC）"
fi

head_ "5. apply 成功（含 UTF-8 / 引号 / 反斜杠 / 制表符）"
if run 0 "$TMP/meta-e2e.conf" "$ADMIN"; then
  ok "exit 0"
else
  bad "apply 失败（rc=$RC $ERR）"
fi

head_ "6. 往返一致（--check 应 0 差异）"
if run 0 "$TMP/meta-e2e.conf" '' --check; then
  ok "写入后再读回，声明值与服务端一致"
else
  bad "读回不一致——JSON 转义或编码有回归（rc=$RC $OUT）"
fi

head_ "7. 幂等：再 apply 一次不产生新写入"
before=$(writes)
if run 0 "$TMP/meta-e2e.conf" "$ADMIN" && [ "$(writes)" = "$before" ]; then
  ok "写入数保持 $before"
else
  bad "重复 apply 又写了一次（before=$before after=$(writes)）"
fi

head_ "8. 只改 topics 时不动 description"
cat >"$TMP/meta-e2e2.conf" <<'EOF'
REPO_SLUG='chm413/packrelay'
REPO_DESCRIPTION='中文"引号"\反斜杠	制表符 与 → 箭头'
REPO_HOMEPAGE='https://example.com/x'
REPO_TOPICS='zeta alpha mid extra'
EOF
before=$(writes)
if run 0 "$TMP/meta-e2e2.conf" "$ADMIN" && [ "$(writes)" = "$((before + 1))" ] && [[ $OUT != *"description/homepage 已更新"* ]]; then
  ok "只发 PUT topics"
else
  bad "增量写入判定有误（before=$before after=$(writes)）"
fi

printf '\n%d passed, %d failed\n' "$PASS" "$FAIL"
[ "$FAIL" = 0 ]
