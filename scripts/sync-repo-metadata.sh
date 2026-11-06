#!/usr/bin/env bash
#
# 把 scripts/repo-metadata.conf 里声明的 description / homepage / topics 同步到 GitHub。
#
# 为什么会有这个脚本：
#   仓库的 description 和 topics 不在 git 里，`git push` 带不走它们。
#   改它们的 REST 端点（PATCH /repos/{owner}/{repo}、PUT /repos/{owner}/{repo}/topics）
#   要求 Administration:write 权限，而推代码只需要 contents:write。
#   两者不是一回事，所以「能 push」不等于「能改仓库设置」。
#
# 用法：
#   GITHUB_TOKEN=ghp_xxx scripts/sync-repo-metadata.sh          # 应用（幂等）
#   scripts/sync-repo-metadata.sh --check                       # 只比对，有差异 exit 1
#   scripts/sync-repo-metadata.sh --dry-run                     # 只打印将要发送的请求体
#   scripts/sync-repo-metadata.sh --repo other/repo             # 覆盖 conf 里的 slug
#   scripts/sync-repo-metadata.sh --base-url http://127.0.0.1:8791   # 打本地 mock
#
# 退出码：
#   0 已同步（或 --check 无差异）   1 --check 发现差异   2 用法 / 环境错误   其它 见 curl / HTTP 状态
#
set -euo pipefail
set -f   # topics 逐词展开时不希望有 glob

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
CONF=${PACKRELAY_META_CONF:-$SCRIPT_DIR/repo-metadata.conf}
BASE_URL=${GH_API_BASE:-https://api.github.com}
MODE=apply

usage() {
  cat <<'EOF'
把 scripts/repo-metadata.conf 里声明的 description / homepage / topics 同步到 GitHub。

用法：
  GITHUB_TOKEN=ghp_xxx scripts/sync-repo-metadata.sh   # 应用（幂等）
  scripts/sync-repo-metadata.sh --check                # 只比对，有差异 exit 1
  scripts/sync-repo-metadata.sh --dry-run              # 只打印将要发送的请求体
  scripts/sync-repo-metadata.sh --repo other/repo      # 覆盖 conf 里的 slug
  scripts/sync-repo-metadata.sh --base-url URL         # 改用别的 API 端点（本地 mock）

退出码：0 已同步 / 1 --check 有差异 / 2 用法或权限错误
EOF
}

while [ $# -gt 0 ]; do
  case $1 in
    --check)    MODE=check ;;
    --dry-run)  MODE=dry-run ;;
    --repo)     [ $# -ge 2 ] || { echo "error: --repo 需要参数" >&2; exit 2; }; REPO_SLUG_OVERRIDE=$2; shift ;;
    --base-url) [ $# -ge 2 ] || { echo "error: --base-url 需要参数" >&2; exit 2; }; BASE_URL=$2; shift ;;
    -h|--help)  usage; exit 0 ;;
    *)          echo "error: 未知参数 $1" >&2; exit 2 ;;
  esac
  shift
done

[ -f "$CONF" ] || { echo "error: 找不到 $CONF" >&2; exit 2; }

# shellcheck disable=SC1090
. "$CONF"

if [ -n "${REPO_SLUG_OVERRIDE-}" ]; then REPO_SLUG=$REPO_SLUG_OVERRIDE; fi

: "${REPO_SLUG:?repo-metadata.conf 缺少 REPO_SLUG}"
: "${REPO_DESCRIPTION-}"
: "${REPO_HOMEPAGE-}"
: "${REPO_TOPICS-}"

TOKEN=${GITHUB_TOKEN:-${GH_TOKEN:-}}

# ---------- 小工具 ----------

json_escape() {
  local s=$1 out= c i
  for ((i = 0; i < ${#s}; i++)); do
    c=${s:i:1}
    case $c in
      '"')  out+='\"' ;;
      '\')  out+='\\' ;;
      $'\n') out+='\n' ;;
      $'\r') out+='\r' ;;
      $'\t') out+='\t' ;;
      *)    out+=$c ;;
    esac
  done
  printf '%s' "$out"
}

# 把 "a,b  c" 规范成 "a b c"（去重 + 字典序），用于稳定比较
normalize_topics() {
  local -a a=($1)
  local i j t n=${#a[@]}
  for ((i = 0; i < n; i++)); do
    for ((j = i + 1; j < n; j++)); do
      if [[ ${a[j]} < ${a[i]} ]]; then t=${a[i]}; a[i]=${a[j]}; a[j]=$t; fi
    done
  done
  # 去重
  local out=''
  for t in "${a[@]:-}"; do
    [ -n "$t" ] || continue
    case " $out " in *" $t "*) continue ;; esac
    out=${out:+$out }$t
  done
  printf '%s' "$out"
}

# 把规范化的 topics 串转成 JSON 数组字面量
topics_json_array() {
  local out='' t
  for t in $WANT_TOPICS; do out=${out:+$out,}\"$t\"; done
  printf '[%s]' "$out"
}

HTTP_CODE=''
HTTP_BODY=''
do_api() {
  local method=$1 path=$2 body=${3-}
  local -a args=(
    -sS -X "$method"
    -H 'Accept: application/vnd.github+json'
    -H 'X-GitHub-Api-Version: 2022-11-28'
    -H 'User-Agent: packrelay-sync-repo-metadata'
  )
  if [ -n "$TOKEN" ]; then args+=(-H "Authorization: Bearer $TOKEN"); fi

  # 请求体必须走文件，绝不能直接放在命令行参数里：
  # Windows 上 curl 从命令行取参数要经过代码页转换，UTF-8 的中文会被转成 GBK 再发出去，
  # 服务端按 UTF-8 解码就是一串乱码，而且返回 200——属于会静默污染仓库的坑。
  local bodyfile=''
  if [ -n "$body" ]; then
    bodyfile=$(mktemp "${TMPDIR:-/tmp}/packrelay-meta.XXXXXX")
    printf '%s' "$body" >"$bodyfile"
    args+=(-H 'Content-Type: application/json' --data-binary "@$bodyfile")
  fi

  local out rc=0
  out=$(curl "${args[@]}" -w $'\n%{http_code}' "$BASE_URL$path") || rc=$?
  if [ -n "$bodyfile" ]; then rm -f -- "$bodyfile" 2>/dev/null || true; fi
  if [ "$rc" != 0 ]; then
    echo "error: 请求失败 $method $BASE_URL$path (curl exit $rc)" >&2
    exit 2
  fi
  HTTP_CODE=${out##*$'\n'}
  HTTP_BODY=${out%$'\n'*}
}

# 从 JSON 里抠字符串字段（只用于 GitHub 返回的单行 JSON）。
# 必须认转义：description 里带 " 时服务端返回的是 \"，简单地取到下一个 " 会截断，
# 结果是永远判定「有差异」，每次 apply 都重复写一遍。
json_field() {
  local body=$1 key=$2
  local re="\"$key\"[[:space:]]*:[[:space:]]*\""
  [[ $body =~ $re ]] || return 0
  local rest=${body#*"${BASH_REMATCH[0]}"}
  local out='' c i n
  n=${#rest}
  for ((i = 0; i < n; i++)); do
    c=${rest:i:1}
    if [ "$c" = '\' ]; then
      i=$((i + 1))
      c=${rest:i:1}
      case $c in
        n)   out+=$'\n' ;;
        r)   out+=$'\r' ;;
        t)   out+=$'\t' ;;
        b)   out+=$'\b' ;;
        f)   out+=$'\f' ;;
        '"') out+='"' ;;
        "\\") out+='\' ;;
        '/') out+='/' ;;
        u)   # GitHub 对非 ASCII 是原样 UTF-8，正常走不到这里；遇到就保留原文，宁可误报也不误写
             out+="\\u${rest:i+1:4}"; i=$((i + 4)) ;;
        *)   out+=$c ;;
      esac
    elif [ "$c" = '"' ]; then
      break
    else
      out+=$c
    fi
  done
  printf '%s' "$out"
}

# 从 /topics 的 {"names":["a","b"]} 里抠词
json_names() {
  local body=$1 rest
  rest=${body#*\"names\"}
  rest=${rest#*[}
  rest=${rest%%]*}
  printf '%s' "${rest//\"/}" | tr ',' ' '
}

explain_http_failure() {
  local code=$1 body=$2 what=$3
  local msg
  msg=$(json_field "$body" message)
  echo "error: $what 失败（HTTP $code）${msg:+: $msg}" >&2
  case $code in
    401)
      echo "       token 无效或已过期。用一个能读该仓库的 token 重试。" >&2 ;;
    403|404)
      cat >&2 <<EOF
       仓库元数据需要 Administration:write，推代码只需要 contents:write，两者不等价。
       fine-grained PAT：Repository access 必须包含 ${REPO_SLUG}，
                         Permissions → Administration = Read and write。
       classic PAT：勾上 repo 即可。
       注意：403/404 在无权限时都可能出现（GitHub 会用 404 隐藏私有仓库）。
EOF
      ;;
  esac
  exit 2
}

# ---------- 期望值 ----------

WANT_DESC=$REPO_DESCRIPTION
WANT_HOME=$REPO_HOMEPAGE
WANT_TOPICS=$(normalize_topics "${REPO_TOPICS//,/ }")

[ ${#WANT_DESC} -le 350 ] || { echo "error: description 超过 350 字符（GitHub 上限）" >&2; exit 2; }

# ---------- dry-run ----------

if [ "$MODE" = dry-run ]; then
  echo "PATCH $BASE_URL/repos/$REPO_SLUG"
  printf '  {"description":"%s","homepage":"%s"}\n' "$(json_escape "$WANT_DESC")" "$(json_escape "$WANT_HOME")"
  echo "PUT $BASE_URL/repos/$REPO_SLUG/topics"
  printf '  {"names":%s}\n' "$(topics_json_array)"
  exit 0
fi

# ---------- 读当前值 ----------

do_api GET "/repos/$REPO_SLUG"
case $HTTP_CODE in
  200) ;;
  *)   explain_http_failure "$HTTP_CODE" "$HTTP_BODY" "读取仓库 $REPO_SLUG" ;;
esac

CUR_DESC=$(json_field "$HTTP_BODY" description)
CUR_HOME=$(json_field "$HTTP_BODY" homepage)

do_api GET "/repos/$REPO_SLUG/topics"
case $HTTP_CODE in
  200) ;;
  *)   explain_http_failure "$HTTP_CODE" "$HTTP_BODY" "读取 topics" ;;
esac
CUR_TOPICS=$(normalize_topics "$(json_names "$HTTP_BODY")")

DESC_DIRTY=0; [ "$CUR_DESC" = "$WANT_DESC" ] || DESC_DIRTY=1
HOME_DIRTY=0; [ "$CUR_HOME" = "$WANT_HOME" ] || HOME_DIRTY=1
TOPICS_DIRTY=0; [ "$CUR_TOPICS" = "$WANT_TOPICS" ] || TOPICS_DIRTY=1

report() {
  [ "$DESC_DIRTY" = 0 ] || { echo "  description"; echo "    - 现状: ${CUR_DESC:-(空)}"; echo "    + 目标: $WANT_DESC"; }
  [ "$HOME_DIRTY" = 0 ] || { echo "  homepage"; echo "    - 现状: ${CUR_HOME:-(空)}"; echo "    + 目标: ${WANT_HOME:-(空)}"; }
  [ "$TOPICS_DIRTY" = 0 ] || { echo "  topics"; echo "    - 现状: ${CUR_TOPICS:-(空)}"; echo "    + 目标: $WANT_TOPICS"; }
}

if [ "$DESC_DIRTY$HOME_DIRTY$TOPICS_DIRTY" = 000 ]; then
  echo "ok: $REPO_SLUG 的 description / homepage / topics 已与 $CONF 一致"
  exit 0
fi

if [ "$MODE" = check ]; then
  echo "drift: $REPO_SLUG 与 $CONF 不一致"
  report
  exit 1
fi

[ -n "$TOKEN" ] || {
  echo "error: 需要 token。设置 GITHUB_TOKEN（或 GH_TOKEN）后重试。" >&2
  echo "       只读校验可以用 --check：public 仓库无需 token。" >&2
  exit 2
}

echo "sync: $REPO_SLUG"
report

if [ "$DESC_DIRTY$HOME_DIRTY" != 00 ]; then
  BODY=$(printf '{"description":"%s","homepage":"%s"}' "$(json_escape "$WANT_DESC")" "$(json_escape "$WANT_HOME")")
  do_api PATCH "/repos/$REPO_SLUG" "$BODY"
  case $HTTP_CODE in
    200) echo "  description/homepage 已更新" ;;
    *)   explain_http_failure "$HTTP_CODE" "$HTTP_BODY" "更新 description/homepage" ;;
  esac
fi

if [ "$TOPICS_DIRTY" = 1 ]; then
  BODY=$(printf '{"names":%s}' "$(topics_json_array)")
  do_api PUT "/repos/$REPO_SLUG/topics" "$BODY"
  case $HTTP_CODE in
    200) echo "  topics 已更新" ;;
    *)   explain_http_failure "$HTTP_CODE" "$HTTP_BODY" "更新 topics" ;;
  esac
fi

echo "done: 重新跑一次 --check 应为 0 差异"
