// packrelay 端到端验证：真实 git + 真实进程 + 模拟 GitHub API/Actions
//
// 覆盖：扫描识别 -> uuid/版本/哈希比对 -> git push -> Actions 打包发布 -> CDN 地址更新 -> 302 生效
// 运行：node scripts/e2e.mjs
import { spawn, spawnSync } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import http from 'node:http';
import { fileURLToPath } from 'node:url';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const ROOT = path.resolve(__dirname, '..');
const E2E = path.join(ROOT, 'e2e');
const REMOTE = path.join(E2E, 'remote.git');
const CIWORK = path.join(E2E, 'ci-work');
const SERVER = path.join(E2E, 'server');
const RELEASES = path.join(E2E, 'releases');
const MCP = path.join(ROOT, 'build', 'packrelay.exe');
const PACKSRC = path.resolve(ROOT, '..', 'Kaleidoscope Tavern v1.0.3 [RP]');
const GH_PORT = 8731;
const REDIRECT_PORT = 19134;

let pass = 0;
let fail = 0;
const failures = [];

function check(cond, label, extra = '') {
  if (cond) {
    pass++;
    console.log(`  PASS  ${label}`);
  } else {
    fail++;
    failures.push(label + (extra ? ` (${extra})` : ''));
    console.log(`  FAIL  ${label}${extra ? ` (${extra})` : ''}`);
  }
}

function sh(cmd, args, opts = {}) {
  const r = spawnSync(cmd, args, {
    encoding: 'utf8',
    env: { ...process.env, ...(opts.env || {}) },
    cwd: opts.cwd || ROOT,
    maxBuffer: 64 * 1024 * 1024,
  });
  return { code: r.status ?? -1, out: (r.stdout || '') + (r.stderr || '') };
}

// 异步执行，避免 spawnSync 阻塞事件循环导致 CI watcher 停摆
function runAsync(cmd, args, opts = {}) {
  return new Promise((resolve) => {
    const child = spawn(cmd, args, {
      cwd: opts.cwd || ROOT,
      env: { ...process.env, ...(opts.env || {}) },
      stdio: ['ignore', 'pipe', 'pipe'],
    });
    let out = '';
    child.stdout.on('data', (d) => (out += d.toString()));
    child.stderr.on('data', (d) => (out += d.toString()));
    child.on('close', (code) => resolve({ code: code ?? -1, out }));
    child.on('error', (e) => resolve({ code: -1, out: out + String(e) }));
  });
}

function git(args, cwd = ROOT) {
  const r = sh('git', ['-c', 'user.name=e2e', '-c', 'user.email=e2e@local', ...args], { cwd });
  return r;
}

function rmrf(p) {
  fs.rmSync(p, { recursive: true, force: true });
}

function sleep(ms) {
  return new Promise((r) => setTimeout(r, ms));
}

function get(url, redirects = 0) {
  return new Promise((resolve, reject) => {
    const req = http.get(url, (res) => {
      if ([301, 302, 303, 307].includes(res.statusCode) && res.headers.location && redirects < 3) {
        res.resume();
        return resolve(get(new URL(res.headers.location, url).href, redirects + 1));
      }
      const chunks = [];
      res.on('data', (c) => chunks.push(c));
      res.on('end', () =>
        resolve({ status: res.statusCode, headers: res.headers, body: Buffer.concat(chunks) })
      );
    });
    req.on('error', reject);
    req.setTimeout(15000, () => req.destroy(new Error('timeout')));
  });
}

async function waitFor(fn, timeoutMs, label) {
  const t0 = Date.now();
  for (;;) {
    const v = await fn();
    if (v) return v;
    if (Date.now() - t0 > timeoutMs) throw new Error(`timeout waiting for ${label}`);
    await sleep(300);
  }
}

// ---------------------------------------------------------------- setup
console.log('\n[1/7] 准备环境');
rmrf(E2E);
fs.mkdirSync(E2E, { recursive: true });
fs.mkdirSync(SERVER, { recursive: true });
fs.mkdirSync(RELEASES, { recursive: true });

const r0 = git(['init', '--bare', '-q', '-b', 'main', REMOTE]);
check(r0.code === 0, 'git init --bare remote.git', r0.out.trim());

const packDst = path.join(SERVER, 'resource_packs', path.basename(PACKSRC));
fs.mkdirSync(path.dirname(packDst), { recursive: true });
fs.cpSync(PACKSRC, packDst, { recursive: true });
const fileCount = (function count(d) {
  let n = 0;
  for (const e of fs.readdirSync(d, { withFileTypes: true })) {
    if (e.isDirectory()) n += count(path.join(d, e.name));
    else n++;
  }
  return n;
})(packDst);
check(fs.existsSync(path.join(packDst, 'manifest.json')), `资源包已就位（${fileCount} 个文件）`);

const cfgDir = path.join(SERVER, 'config', 'packrelay');
fs.mkdirSync(cfgDir, { recursive: true });
const cfgPath = path.join(cfgDir, 'config.json');
const config = {
  version: 1,
  resourceDir: 'resource_packs',
  baseDir: '../..',
  server: { scanIntervalSeconds: 30, scanOnStart: true, debounceMs: 0 },
  identity: {
    requireManifest: true,
    conflictPolicy: 'highest_version',
    ignorePatterns: ['**/.DS_Store', '**/Thumbs.db', '**/*.tmp'],
  },
  git: {
    enabled: true,
    workDir: '.mcpack-sync/repo',
    remote: REMOTE.replace(/\\/g, '/'),
    branch: 'main',
    authMode: 'credential_helper',
    authorName: 'packrelay',
    authorEmail: 'bot@local',
    commitTemplate: 'chore(pack): {name} v{version} [{uuid8}]',
    pushRetries: 2,
    retryBackoffMs: 300,
    timeoutMs: 120000,
    subDir: 'packs',
  },
  release: {
    waitForRelease: true,
    pollIntervalMs: 400,
    pollTimeoutMs: 60000,
    tagTemplate: '{slug}-v{version}-{hash8}',
    assetNameTemplate: '{slug}-v{version}-{hash8}.zip',
    assetSuffix: '.zip',
    requireAsset: true,
  },
  github: { apiBase: `http://127.0.0.1:${GH_PORT}`, owner: 'e2e', repo: 'packs', timeoutMs: 10000, maxRetries: 2 },
  cdn: {
    enabled: true,
    mode: 'release_asset',
    activeMirror: 'auto',
    writeManifest: true,
    manifestFile: '.mcpack-sync/cdn.json',
    mirrors: [
      { name: 'direct', enabled: true, priority: 0, template: '{url}' },
      { name: 'ghfast', enabled: true, priority: 10, template: `http://127.0.0.1:${GH_PORT}/mirror/{url}` },
    ],
    redirectServer: { enabled: true, bind: '127.0.0.1', port: REDIRECT_PORT, path: '/pack' },
  },
  log: { level: 'info', file: 'logs/packrelay.log' },
};
fs.writeFileSync(cfgPath, JSON.stringify(config, null, 2));

// ---------------------------------------------------------------- mock github
console.log('\n[2/7] 启动模拟 GitHub API');
const mock = spawn(process.execPath, [path.join(ROOT, 'scripts', 'mock-gh-server.mjs')], {
  env: { ...process.env, MOCK_GH_PORT: String(GH_PORT), MOCK_GH_RELEASES: RELEASES },
  stdio: ['ignore', 'pipe', 'pipe'],
});
mock.stdout.on('data', (d) => process.stdout.write('    ' + d.toString().trimEnd() + '\n'));
mock.stderr.on('data', (d) => process.stdout.write('    [mock-gh:err] ' + d.toString().trimEnd() + '\n'));
await waitFor(async () => {
  try {
    const r = await get(`http://127.0.0.1:${GH_PORT}/health`);
    return r.status === 200;
  } catch {
    return false;
  }
}, 10000, 'mock gh server');

// ---------------------------------------------------------------- ci watcher（模拟 Actions 触发）
let ciBusy = false;
let lastSha = '';
async function ciTick() {
  if (ciBusy) return;
  const head = git(['-C', REMOTE, 'rev-parse', 'main']);
  const sha = head.out.trim();
  if (!sha || sha === lastSha || head.code !== 0) return;
  ciBusy = true;
  try {
    if (!fs.existsSync(path.join(CIWORK, '.git'))) {
      rmrf(CIWORK);
      const c = git(['clone', '-q', REMOTE, CIWORK]);
      if (c.code !== 0) throw new Error('clone failed: ' + c.out);
    } else {
      git(['-C', CIWORK, 'fetch', '-q', 'origin', 'main']);
    }
    const r = git(['-C', CIWORK, 'reset', '--hard', '-q', sha]);
    if (r.code !== 0) throw new Error('reset failed: ' + r.out);

    const script = path.join(CIWORK, 'scripts', 'ci-release.sh');
    if (!fs.existsSync(script)) {
      console.log('    [ci] scripts/ci-release.sh missing, skip');
      lastSha = sha;
      return;
    }
    console.log(`    [ci] detected new commit ${sha.slice(0, 8)}, running ci-release.sh`);
    const run = sh('bash', [script, '--fs', RELEASES], {
      cwd: CIWORK,
      env: {
        MCPACK_RELEASE_BASE_URL: `http://127.0.0.1:${GH_PORT}/downloads`,
        PATH: `${path.join(ROOT, 'build')}${path.delimiter}${process.env.PATH}`,
      },
    });
    console.log(`    [ci] run for ${sha.slice(0, 8)} -> exit ${run.code}`);
    run.out
      .split('\n')
      .filter((l) => l.trim())
      .forEach((l) => console.log('       ' + l));
    lastSha = sha;
  } catch (e) {
    console.log('    [ci] error: ' + e.message);
  } finally {
    ciBusy = false;
  }
}
// 用显式循环代替 setInterval：子进程退出后事件循环仍能及时推进
let ciRunning = true;
const ciLoop = (async () => {
  while (ciRunning) {
    try {
      await ciTick();
    } catch (e) {
      console.log('    [ci] tick error: ' + e.message);
    }
    await sleep(300);
  }
})();

// ---------------------------------------------------------------- run 1
console.log('\n[3/7] 首次运行：扫描 -> 推送 -> Release -> CDN');
const t1 = Date.now();
let run1 = await runAsync(MCP, ['once', '--config', cfgPath]);
console.log(run1.out.trimEnd());
check(run1.code === 0, 'mcpsync once 退出码为 0', `exit=${run1.code}`);
check(/added=1/.test(run1.out), '识别出 1 个新增资源包');
const elapsed1 = Date.now() - t1;

const head1 = git(['-C', REMOTE, 'rev-parse', 'main']).out.trim();
check(!!head1, '远端已有提交', head1.slice(0, 8));

const log1 = git(['-C', REMOTE, 'log', '--oneline']).out.trim().split('\n').filter(Boolean);
check(log1.length >= 1 && /chore\(pack\)|packrelay/.test(log1.join(' ')), '远端提交包含资源包提交', log1[0]);

const wf = git(['-C', REMOTE, 'show', 'main:.github/workflows/release.yml']);
check(wf.code === 0 && /release-packs/.test(wf.out), '仓库已预置 GitHub Actions workflow');
const ciSh = git(['-C', REMOTE, 'show', 'main:scripts/ci-release.sh']);
check(ciSh.code === 0 && /ci-release/.test(ciSh.out) && /gh release create/.test(ciSh.out),
  '仓库已预置发布脚本（含 gh release create）');

const idx = JSON.parse(git(['-C', REMOTE, 'show', 'main:index.json']).out);
check(idx.packs?.length === 1, 'index.json 记录了 1 个资源包');
const packEntry = idx.packs[0];
check(/^[0-9a-f-]{36}$/.test(packEntry.uuid), 'index.json 含 uuid', packEntry.uuid);
check(/^\d+\.\d+\.\d+$/.test(packEntry.versionString), 'index.json 含版本号', packEntry.versionString);
check(!!packEntry.tag && !!packEntry.assetName, 'index.json 已预渲染 tag/assetName', packEntry.tag);

const relFile = path.join(RELEASES, packEntry.tag, 'release.json');
check(fs.existsSync(relFile), 'Actions 已产出 release 元数据', packEntry.tag);
const assetPath = path.join(RELEASES, packEntry.tag, packEntry.assetName);
check(fs.existsSync(assetPath) && fs.statSync(assetPath).size > 0, 'Actions 已产出 zip 资产',
  fs.existsSync(assetPath) ? `${fs.statSync(assetPath).size} bytes` : 'missing');

if (fs.existsSync(assetPath)) {
  const buf = fs.readFileSync(assetPath);
  check(buf[0] === 0x50 && buf[1] === 0x4b, 'zip 文件头正确（PK）');
  // 用 python 校验 zip 结构与条目数（若可用）
  const py = sh('python', ['-c', `import zipfile,sys;z=zipfile.ZipFile(r"${assetPath}");print(len(z.namelist()));print(z.testzip())`]);
  if (py.code === 0) {
    const [names, bad] = py.out.trim().split('\n');
    check(Number(names) >= fileCount, `zip 条目数 >= 源文件数`, `${names} vs ${fileCount}`);
    check(bad.trim() === 'None', 'zip CRC 校验通过');
  }
}

const cdnPath = path.join(SERVER, '.mcpack-sync', 'cdn.json');
const cdn = fs.existsSync(cdnPath) ? JSON.parse(fs.readFileSync(cdnPath, 'utf8')) : { packs: [] };
check(cdn.packs?.length === 1, 'cdn.json 记录了 1 个目标');
const cdnUrl1 = cdn.packs[0]?.publicUrl || '';
check(cdnUrl1 === `http://127.0.0.1:${GH_PORT}/downloads/${packEntry.tag}/${packEntry.assetName}`,
  'CDN 地址指向最新 release 资产', cdnUrl1);

const statePath = path.join(SERVER, '.mcpack-sync', 'state.json');
const state1 = fs.existsSync(statePath) ? JSON.parse(fs.readFileSync(statePath, 'utf8')) : { packs: [] };
check(state1.packs?.length === 1 && state1.packs[0].fingerprint === packEntry.fingerprint,
  'state 与仓库索引指纹一致');
check(state1.packs?.[0]?.pushed?.commit === head1, 'state 记录了推送的 commit');

// ---------------------------------------------------------------- run 2（幂等）
console.log('\n[4/7] 二次运行：内容未变，不应重复提交/上传');
const run2 = await runAsync(MCP, ['once', '--config', cfgPath]);
console.log(run2.out.trimEnd());
check(run2.code === 0, 'mcpsync once 退出码为 0');
check(/unchanged=1/.test(run2.out), '判定为 unchanged');
check(/added=0 updated=0 removed=0/.test(run2.out), '未产生新的变更');
const head2 = git(['-C', REMOTE, 'rev-parse', 'main']).out.trim();
check(head2 === head1, '远端 HEAD 未变化（无重复提交）', `${head1.slice(0, 8)} vs ${head2.slice(0, 8)}`);

// ---------------------------------------------------------------- run 3（内容变更）
console.log('\n[5/7] 修改资源内容（版本号不变）：应识别为更新');
const texture = path.join(packDst, 'textures', 'entity', 'zombie.png');
const target = fs.existsSync(texture) ? texture : path.join(packDst, 'pack_icon.png');
const orig = fs.readFileSync(target);
fs.writeFileSync(target, Buffer.concat([orig, Buffer.from('MCPACK-SYNC-E2E-PATCH')]));
check(true, `已修改 ${path.relative(packDst, target)}`);

const run3 = await runAsync(MCP, ['once', '--config', cfgPath]);
console.log(run3.out.trimEnd());
check(run3.code === 0, 'mcpsync once 退出码为 0');
check(/updated=1/.test(run3.out), '识别为 updated（内容变化但版本不变）');
const head3 = git(['-C', REMOTE, 'rev-parse', 'main']).out.trim();
check(head3 !== head1, '产生新 commit', `${head3.slice(0, 8)}`);

const idx3 = JSON.parse(git(['-C', REMOTE, 'show', 'main:index.json']).out);
const tag3 = idx3.packs[0].tag;
check(tag3 !== packEntry.tag, '新内容对应新 tag', `${packEntry.tag} -> ${tag3}`);

const cdn3 = JSON.parse(fs.readFileSync(path.join(SERVER, '.mcpack-sync', 'cdn.json'), 'utf8'));
const cdnUrl3 = cdn3.packs[0].publicUrl;
check(cdnUrl3 !== cdnUrl1 && cdnUrl3.includes(tag3), 'CDN 地址已切换到新 release', cdnUrl3);

// ---------------------------------------------------------------- redirect 服务
console.log('\n[6/7] 校验 CDN 302 重定向服务');
const redir = spawn(MCP, ['redirect', '--config', cfgPath], { stdio: ['ignore', 'pipe', 'pipe'] });
redir.stdout.on('data', (d) => process.stdout.write('    ' + d.toString().trimEnd() + '\n'));
try {
  await waitFor(async () => {
    try {
      const r = await get(`http://127.0.0.1:${REDIRECT_PORT}/health`);
      return r.status === 200;
    } catch {
      return false;
    }
  }, 10000, 'redirect server');

  const health = await get(`http://127.0.0.1:${REDIRECT_PORT}/health`);
  check(health.status === 200, '/health 返回 200');

  const plain = await new Promise((resolve, reject) => {
    http
      .get(`http://127.0.0.1:${REDIRECT_PORT}/pack`, (res) => {
        res.resume();
        resolve({ status: res.statusCode, location: res.headers.location });
      })
      .on('error', reject);
  });
  check(plain.status === 302, '/pack 返回 302');
  check(plain.location === cdnUrl3, '302 Location 指向最新 release', plain.location);

  // 跟随重定向下载，验证 CDN 链路真的能取到 zip
  const dl = await get(`http://127.0.0.1:${REDIRECT_PORT}/pack`);
  check(dl.status === 200 && dl.body.length > 0, '跟随 302 可下载到资产', `${dl.body.length} bytes`);
  check(dl.body[0] === 0x50 && dl.body[1] === 0x4b, '下载内容为合法 zip（PK 头）');
} finally {
  redir.kill();
}

// ---------------------------------------------------------------- summary
ciRunning = false;
await ciLoop;
mock.kill();
await sleep(300);

console.log('\n[7/7] 结果汇总');
const anyZip = fs.existsSync(assetPath) ? assetPath : path.join(RELEASES, tag3, idx3.packs[0].assetName);
const zipMiB = fs.existsSync(anyZip) ? (fs.statSync(anyZip).size / 1048576).toFixed(1) : 'n/a';
console.log(`  首次全量耗时: ${elapsed1}ms（${fileCount} 文件 / 产物约 ${zipMiB} MiB）`);
console.log(`  提交数: ${git(['-C', REMOTE, 'rev-list', '--count', 'main']).out.trim()}`);
console.log(`  产物: ${path.relative(ROOT, E2E)}/`);
console.log(`\n  ${pass} passed, ${fail} failed`);
if (fail) {
  console.log('  failed cases:');
  failures.forEach((f) => console.log('   - ' + f));
}
process.exit(fail === 0 ? 0 : 1);
