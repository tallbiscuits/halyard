// SPDX-License-Identifier: GPL-3.0-or-later
// The update page's own <script> (src/update_page.h), run in Node, which has the browser's
// CompressionStream and DecompressionStream. Checks that it turns Mainsail's release zip, and
// a Halyard card zip, into the tar ./run.sh webui would send, and that it tells a firmware.bin
// from images that must not go to /halyard/update.
//   node test_update_page.mjs MAINSAIL_ZIP [BUILD_DIR]
import { readFileSync, existsSync, mkdtempSync, rmSync } from "node:fs";
import { execFileSync } from "node:child_process";
import { gunzipSync } from "node:zlib";
import { tmpdir } from "node:os";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";
import assert from "node:assert/strict";

const here = dirname(fileURLToPath(import.meta.url));
const page = readFileSync(join(here, "../../src/update_page.h"), "utf8");
const script = page.slice(page.lastIndexOf("<script>") + 8, page.lastIndexOf("</script>"));
const { checkFirmware, unzip, mainsailFiles, tar } =
  new Function(script + "\nreturn { checkFirmware, unzip, mainsailFiles, tar };")();

const [zipPath, build] = process.argv.slice(2);
let passed = 0;
const test = async (name, fn) => { await fn(); passed++; console.log("  ok  " + name); };

// A ustar archive read back independently: [name, bytes] in order, checksums verified.
function untar(t) {
  const out = [];
  for (let p = 0; p + 512 <= t.length; ) {
    const h = t.subarray(p, p + 512);
    if (h.every((b) => b === 0)) break;
    const str = (o, n) => Buffer.from(h.subarray(o, o + n)).toString().replace(/\0.*$/s, "");
    let sum = 0;
    for (let i = 0; i < 512; i++) sum += i >= 148 && i < 156 ? 32 : h[i];
    assert.equal(parseInt(str(148, 8), 8), sum, "header checksum");
    assert.equal(str(257, 5), "ustar");
    const prefix = str(345, 155), size = parseInt(str(124, 12), 8);
    out.push([(prefix ? prefix + "/" : "") + str(0, 100), t.subarray(p + 512, p + 512 + size)]);
    p += 512 + Math.ceil(size / 512) * 512;
  }
  return out;
}
const plain = ([n, d]) => (n.endsWith(".gz") ? [n.slice(0, -3), gunzipSync(d)] : [n, Buffer.from(d)]);

await test("Mainsail's release zip becomes the tar run.sh webui sends", async () => {
  const zip = new Uint8Array(readFileSync(zipPath));
  const entries = await unzip(zip);
  const files = untar(tar(await mainsailFiles(entries)));
  // The same names, gzipped the same way, and the same bytes once unpacked.
  const dir = mkdtempSync(join(tmpdir(), "halyard-ui-"));
  try {
    execFileSync("unzip", ["-q", zipPath, "-d", dir]);
    const listing = execFileSync("find", [".", "-type", "f"], { cwd: dir }).toString().trim().split("\n");
    assert.equal(files.length, listing.length);
    for (const [n, d] of files.map(plain)) {
      const original = readFileSync(join(dir, n));
      assert.ok(Buffer.compare(original, d) === 0, n);
    }
  } finally { rmSync(dir, { recursive: true }); }
  const gz = files.filter(([n]) => n.endsWith(".gz")).length;
  assert.ok(files.some(([n]) => n === "index.html.gz"));
  assert.ok(gz > files.length / 2, "most files are gzipped");
  // Long names go into the ustar prefix.
  const long = [["a/".repeat(60) + "file.js", new Uint8Array([1, 2, 3])]];
  assert.deepEqual(untar(tar(long)).map(([n, d]) => [n, [...d]]), [[long[0][0], [1, 2, 3]]]);
});

await test("a Halyard card zip gives its gzipped files as they are", async () => {
  const dir = mkdtempSync(join(tmpdir(), "halyard-card-"));
  try {
    const mainsail = join(dir, "m");
    execFileSync("unzip", ["-q", zipPath, "-d", mainsail]);
    const card = join(dir, "card.zip");
    execFileSync("python3", [join(here, "../../../tools/make_card.py"), mainsail, card]);
    const entries = await unzip(new Uint8Array(readFileSync(card)));
    const files = await mainsailFiles(entries);
    const want = entries.filter(([n]) => n.startsWith(".halyard/www/"));
    assert.equal(files.length, want.length);
    assert.ok(!files.some(([n]) => n === "halyard.cfg"), "never the settings");
    for (let i = 0; i < files.length; i++) assert.equal(files[i][0], want[i][0].slice(13));
  } finally { rmSync(dir, { recursive: true }); }
});

await test("a zip without Mainsail, or no zip at all, is refused", async () => {
  await assert.rejects(unzip(new Uint8Array(100)), /not a zip/);
  const dir = mkdtempSync(join(tmpdir(), "halyard-x-"));
  try {
    execFileSync("python3", ["-c", "import zipfile,sys; z=zipfile.ZipFile(sys.argv[1],'w',zipfile.ZIP_DEFLATED); z.writestr('readme.txt','hi'*100)", join(dir, "x.zip")]);
    const entries = await unzip(new Uint8Array(readFileSync(join(dir, "x.zip"))));
    assert.equal(Buffer.from(entries[0][1]).toString(), "hi".repeat(100));
    await assert.rejects(mainsailFiles(entries), /No index.html/);
  } finally { rmSync(dir, { recursive: true }); }
});

if (build && existsSync(join(build, "firmware.bin"))) {
  await test("firmware.bin passes; the bootloader, a merged image and junk do not", () => {
    const fw = new Uint8Array(readFileSync(join(build, "firmware.bin")));
    const c = checkFirmware(fw);
    assert.equal(c.error, undefined);
    const boot = new Uint8Array(readFileSync(join(build, "bootloader.bin")));
    assert.match(checkFirmware(boot).error, /not an app image/);  // also how a merged image starts
    assert.match(checkFirmware(new Uint8Array(4096)).error, /not an ESP32 firmware/);
    const other = fw.slice(); other[12] = 0;  // an ESP32 (not S3) image
    assert.match(checkFirmware(other).error, /another ESP32 chip/);
  });
} else {
  console.log("  --  no firmware build: the firmware image check was skipped");
}
console.log(`update page: ${passed} passed`);
