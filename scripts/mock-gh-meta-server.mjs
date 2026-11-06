// 本地模拟 GitHub「仓库元数据」端点，只用于验证 scripts/sync-repo-metadata.sh。
//
// 覆盖的真实端点：
//   GET   /repos/:owner/:repo          -> { description, homepage }
//   PATCH /repos/:owner/:repo          -> 改 description / homepage
//   GET   /repos/:owner/:repo/topics   -> { names: [] }
//   PUT   /repos/:owner/:repo/topics   -> 整体替换 topics
//
// 权限模型刻意贴近真实：推代码的 contents:write 改不了这些字段，
// 只有 Administration:write 才行。这里用 token 值区分：
//   MOCK_META_ADMIN_TOKEN  (默认 admin-token)      -> 全部允许
//   其它任何 token / 不带 token                      -> PATCH / PUT 返回 403
//                                        （GET 允许，模拟 public 仓库可匿名读）
//
// 附加端点（仅 mock 有）：
//   POST /__reset  -> 清空状态
//   GET  /__state  -> 查看 mock 内部状态
import http from 'node:http';

const port = Number(process.env.MOCK_META_PORT || 8791);
const adminToken = process.env.MOCK_META_ADMIN_TOKEN || 'admin-token';

/** @type {{description: string, homepage: string, topics: string[]}} */
const state = { description: '', homepage: '', topics: [], dirtyWrites: [] };

function json(res, code, obj) {
  const body = JSON.stringify(obj);
  res.writeHead(code, { 'content-type': 'application/json', 'content-length': Buffer.byteLength(body) });
  res.end(body);
}

function readBody(req) {
  return new Promise((resolve) => {
    const chunks = [];
    req.on('data', (c) => chunks.push(c));
    req.on('end', () => resolve(Buffer.concat(chunks).toString('utf8')));
  });
}

function bearer(req) {
  const h = req.headers.authorization || '';
  const m = h.match(/^Bearer\s+(.+)$/i);
  return m ? m[1].trim() : '';
}

const FORBIDDEN = { message: 'Resource not accessible by personal access token' };

const server = http.createServer(async (req, res) => {
  const url = new URL(req.url, `http://127.0.0.1:${port}`);
  const p = decodeURIComponent(url.pathname);

  if (p === '/__reset' && req.method === 'POST') {
    state.description = '';
    state.homepage = '';
    state.topics = [];
    state.dirtyWrites = [];
    return json(res, 200, { ok: true });
  }
  if (p === '/__state') return json(res, 200, state);

  const isRepo = /^\/repos\/[^/]+\/[^/]+$/.test(p);
  const isTopics = /^\/repos\/[^/]+\/[^/]+\/topics$/.test(p);
  if (!isRepo && !isTopics) return json(res, 404, { message: 'Not Found', path: p });

  const token = bearer(req);
  const canWrite = token === adminToken && token.length > 0;

  if (req.method === 'GET') {
    if (isRepo) return json(res, 200, { description: state.description, homepage: state.homepage });
    return json(res, 200, { names: state.topics });
  }

  if (req.method === 'PATCH' && isRepo) {
    if (!canWrite) return json(res, 403, FORBIDDEN);
    const raw = await readBody(req);
    let body;
    try {
      body = JSON.parse(raw || '{}');
    } catch {
      return json(res, 400, { message: 'Problems parsing JSON' });
    }
    if (typeof body.description === 'string') state.description = body.description;
    if (typeof body.homepage === 'string') state.homepage = body.homepage;
    state.dirtyWrites.push({ method: 'PATCH', raw });
    return json(res, 200, { description: state.description, homepage: state.homepage });
  }

  if (req.method === 'PUT' && isTopics) {
    if (!canWrite) return json(res, 403, FORBIDDEN);
    const raw = await readBody(req);
    let body;
    try {
      body = JSON.parse(raw || '{}');
    } catch {
      return json(res, 400, { message: 'Problems parsing JSON' });
    }
    if (!Array.isArray(body.names)) return json(res, 422, { message: 'Invalid request. names must be an array.' });
    state.topics = body.names;
    state.dirtyWrites.push({ method: 'PUT', raw });
    return json(res, 200, { names: state.topics });
  }

  return json(res, 405, { message: `Method ${req.method} not allowed on ${p}` });
});

server.listen(port, '127.0.0.1', () => {
  console.log(`[mock-gh-meta] listening on http://127.0.0.1:${port}, adminToken=${adminToken}`);
});
