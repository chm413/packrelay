// 本地模拟 GitHub REST API + 资源下载站，用于端到端验证。
// 端点：
//   GET /repos/:owner/:repo/releases/tags/:tag   -> 读取 <releases>/<tag>/release.json
//   GET /repos/:owner/:repo/releases/latest      -> 最新一个
//   GET /downloads/:tag/:asset                   -> 返回 zip 文件
//   GET /mirror/<encoded absolute url>           -> 302 到解码后的地址（模拟加速镜像）
import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';

const port = Number(process.env.MOCK_GH_PORT || 8731);
const releasesDir = process.env.MOCK_GH_RELEASES || path.resolve('.releases');

function json(res, code, obj) {
  const body = JSON.stringify(obj);
  res.writeHead(code, { 'content-type': 'application/json', 'content-length': Buffer.byteLength(body) });
  res.end(body);
}

function tagsNewestFirst() {
  if (!fs.existsSync(releasesDir)) return [];
  return fs
    .readdirSync(releasesDir, { withFileTypes: true })
    .filter((d) => d.isDirectory() && fs.existsSync(path.join(releasesDir, d.name, 'release.json')))
    .map((d) => ({
      tag: d.name,
      mtime: fs.statSync(path.join(releasesDir, d.name, 'release.json')).mtimeMs,
    }))
    .sort((a, b) => b.mtime - a.mtime)
    .map((x) => x.tag);
}

const server = http.createServer((req, res) => {
  const url = new URL(req.url, `http://127.0.0.1:${port}`);
  const p = decodeURIComponent(url.pathname);

  let m = p.match(/^\/repos\/[^/]+\/[^/]+\/releases\/tags\/(.+)$/);
  if (m) {
    const file = path.join(releasesDir, m[1], 'release.json');
    if (fs.existsSync(file)) {
      const body = fs.readFileSync(file, 'utf8');
      res.writeHead(200, { 'content-type': 'application/json' });
      return res.end(body);
    }
    return json(res, 404, { message: 'Not Found' });
  }

  if (/^\/repos\/[^/]+\/[^/]+\/releases\/latest$/.test(p)) {
    const newest = tagsNewestFirst()[0];
    if (!newest) return json(res, 404, { message: 'Not Found' });
    const body = fs.readFileSync(path.join(releasesDir, newest, 'release.json'), 'utf8');
    res.writeHead(200, { 'content-type': 'application/json' });
    return res.end(body);
  }

  m = p.match(/^\/downloads\/([^/]+)\/([^/]+)$/);
  if (m) {
    const file = path.join(releasesDir, m[1], m[2]);
    if (!fs.existsSync(file)) return json(res, 404, { message: 'asset not found' });
    const buf = fs.readFileSync(file);
    res.writeHead(200, { 'content-type': 'application/zip', 'content-length': buf.length });
    return res.end(buf);
  }

  m = p.match(/^\/mirror\/(.+)$/);
  if (m) {
    res.writeHead(302, { location: m[1] });
    return res.end();
  }

  if (p === '/health') return json(res, 200, { status: 'ok', releases: tagsNewestFirst() });
  return json(res, 404, { message: 'Not Found', path: p });
});

server.listen(port, '127.0.0.1', () => {
  console.log(`[mock-gh] listening on http://127.0.0.1:${port}, releases=${releasesDir}`);
});
