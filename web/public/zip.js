// Minimal zip reading and writing for the card builder, with no dependencies.
//
// Reading handles what GitHub release zips use: stored (0) and deflated (8) entries, no
// encryption, no zip64. Deflate goes through the browser's DecompressionStream("deflate-raw").
// Every extracted file is checked against the CRC-32 in its directory entry.
//
// Writing stores files uncompressed (method 0): the card files are already dense, and a stored
// zip is the simplest thing every unzip tool on every OS opens.

const CRC_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    t[n] = c >>> 0;
  }
  return t;
})();

export function crc32(bytes) {
  let c = 0xffffffff;
  for (let i = 0; i < bytes.length; i++) c = CRC_TABLE[(c ^ bytes[i]) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}

async function inflateRaw(bytes) {
  const stream = new Blob([bytes]).stream().pipeThrough(new DecompressionStream("deflate-raw"));
  return new Uint8Array(await new Response(stream).arrayBuffer());
}

// Returns [{ name, read() -> Promise<Uint8Array> }] for every file entry (directories skipped).
export function readZip(buffer) {
  const bytes = buffer instanceof Uint8Array ? buffer : new Uint8Array(buffer);
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);

  // The end-of-central-directory record is in the last 22 + 65535 bytes.
  let eocd = -1;
  for (let i = bytes.length - 22; i >= Math.max(0, bytes.length - 22 - 0xffff); i--) {
    if (view.getUint32(i, true) === 0x06054b50) { eocd = i; break; }
  }
  if (eocd < 0) throw new Error("not a zip file");
  const count = view.getUint16(eocd + 10, true);
  let p = view.getUint32(eocd + 16, true);
  if (count === 0xffff || p === 0xffffffff) throw new Error("zip64 is not supported");

  const dec = new TextDecoder();
  const entries = [];
  for (let i = 0; i < count; i++) {
    if (view.getUint32(p, true) !== 0x02014b50) throw new Error("broken zip directory");
    const flags = view.getUint16(p + 8, true);
    const method = view.getUint16(p + 10, true);
    const crc = view.getUint32(p + 16, true);
    const csize = view.getUint32(p + 20, true);
    const usize = view.getUint32(p + 24, true);
    const nlen = view.getUint16(p + 28, true);
    const xlen = view.getUint16(p + 30, true);
    const clen = view.getUint16(p + 32, true);
    const local = view.getUint32(p + 42, true);
    const name = dec.decode(bytes.subarray(p + 46, p + 46 + nlen));
    p += 46 + nlen + xlen + clen;
    if (name.endsWith("/")) continue;

    const rawBytes = () => {
      if (flags & 1) throw new Error(`${name}: encrypted entries are not supported`);
      if (view.getUint32(local, true) !== 0x04034b50) throw new Error(`${name}: broken local header`);
      const start = local + 30 + view.getUint16(local + 26, true) + view.getUint16(local + 28, true);
      return bytes.subarray(start, start + csize);
    };
    entries.push({
      name,
      size: usize,
      // The entry as it is stored, for copying it into another zip without inflating it:
      // writeZip takes { name, size, crc, method, compressed }. Only for a zip already checked
      // as a whole (the starter content's SHA-256), since nothing checks the CRC on the way.
      stored() {
        if (method !== 0 && method !== 8) throw new Error(`${name}: compression method ${method} is not supported`);
        return { name, size: usize, crc, method, compressed: rawBytes() };
      },
      async read() {
        const raw = rawBytes();
        let out;
        if (method === 0) out = raw.slice();
        else if (method === 8) out = await inflateRaw(raw);
        else throw new Error(`${name}: compression method ${method} is not supported`);
        if (out.length !== usize || crc32(out) !== crc) throw new Error(`${name}: CRC check failed`);
        return out;
      },
    });
  }
  return entries;
}

// files: [{ name, data: Uint8Array }], stored; or [{ name, size, crc, method: 8, compressed }],
// an entry already deflated (readZip's stored(), or web/tools/starter.mjs's own deflate), copied
// as it is. Returns a Uint8Array holding the zip. Every entry gets the same fixed time
// (2026-01-01 00:00), so a given input is byte-identical.
export function writeZip(files) {
  const enc = new TextEncoder();
  const DOS_TIME = 0;
  const DOS_DATE = ((2026 - 1980) << 9) | (1 << 5) | 1;
  const locals = [];
  const centrals = [];
  let offset = 0;

  for (const f of files) {
    const name = enc.encode(f.name);
    const deflated = f.compressed !== undefined && f.method === 8;
    const body = f.compressed !== undefined ? f.compressed : f.data;
    const crc = f.compressed !== undefined ? f.crc : crc32(f.data);
    const size = f.compressed !== undefined ? f.size : f.data.length;
    const csize = body.length;
    if (size >= 0xffffffff || offset + csize >= 0xffffffff) throw new Error(`${f.name}: too big for a zip without zip64`);

    const lh = new DataView(new ArrayBuffer(30));
    lh.setUint32(0, 0x04034b50, true);
    lh.setUint16(4, 20, true);          // version needed
    lh.setUint16(6, 0x0800, true);      // UTF-8 names
    lh.setUint16(8, deflated ? 8 : 0, true);
    lh.setUint16(10, DOS_TIME, true);
    lh.setUint16(12, DOS_DATE, true);
    lh.setUint32(14, crc, true);
    lh.setUint32(18, csize, true);
    lh.setUint32(22, size, true);
    lh.setUint16(26, name.length, true);
    lh.setUint16(28, 0, true);
    locals.push(new Uint8Array(lh.buffer), name, body);

    const ch = new DataView(new ArrayBuffer(46));
    ch.setUint32(0, 0x02014b50, true);
    ch.setUint16(4, 20, true);          // version made by
    ch.setUint16(6, 20, true);
    ch.setUint16(8, 0x0800, true);
    ch.setUint16(10, deflated ? 8 : 0, true);
    ch.setUint16(12, DOS_TIME, true);
    ch.setUint16(14, DOS_DATE, true);
    ch.setUint32(16, crc, true);
    ch.setUint32(20, csize, true);
    ch.setUint32(24, size, true);
    ch.setUint16(28, name.length, true);
    ch.setUint32(42, offset, true);
    centrals.push(new Uint8Array(ch.buffer), name);

    offset += 30 + name.length + csize;
  }

  const cdSize = centrals.reduce((n, b) => n + b.length, 0);
  const end = new DataView(new ArrayBuffer(22));
  end.setUint32(0, 0x06054b50, true);
  end.setUint16(8, files.length, true);
  end.setUint16(10, files.length, true);
  end.setUint32(12, cdSize, true);
  end.setUint32(16, offset, true);

  const parts = [...locals, ...centrals, new Uint8Array(end.buffer)];
  const out = new Uint8Array(parts.reduce((n, b) => n + b.length, 0));
  let at = 0;
  for (const b of parts) { out.set(b, at); at += b.length; }
  return out;
}

export async function sha256Hex(bytes) {
  const digest = await crypto.subtle.digest("SHA-256", bytes);
  return [...new Uint8Array(digest)].map((b) => b.toString(16).padStart(2, "0")).join("");
}
