import fs from 'node:fs';
import path from 'node:path';

import { ASSET_CACHE_SUBDIR, ASSET_SCHEMA } from '../../web/dist/assets/model.js';

function fileSchema(file) {
  const fd = fs.openSync(file, 'r');
  const buf = Buffer.alloc(8);
  const n = fs.readSync(fd, buf, 0, 8, 0);
  fs.closeSync(fd);
  if (n < 8) return null;
  return buf.readUInt32BE(4);
}

/** Resolve a file in the schema-versioned cache, then the unversioned extract. */
export function cacheFile(...parts) {
  const bases = ['cache', 'fixtures/cache'];
  const candidates = [];
  for (const base of bases) {
    candidates.push(path.join(base, ASSET_CACHE_SUBDIR, ...parts));
  }
  for (const base of bases) {
    candidates.push(path.join(base, ...parts));
  }
  for (const file of candidates) {
    if (!fs.existsSync(file)) continue;
    const schema = fileSchema(file);
    if (schema === ASSET_SCHEMA) return file;
  }
  return path.join('cache', ASSET_CACHE_SUBDIR, ...parts);
}
