// SPDX-License-Identifier: MIT
// Pull an HTML page out of a C string constant (e.g. DIAG_PAGE in diag_page.h) so the
// pages the firmware serves can be tested in a real browser.
const fs = require('fs');
const path = require('path');

const ROOT = path.resolve(__dirname, '../../..');

function cString(file, name) {
  const src = fs.readFileSync(path.join(ROOT, file), 'utf8');
  const start = src.indexOf(`${name}[] =`);
  if (start < 0) throw new Error(`${name} not found in ${file}`);
  const end = src.indexOf('";\n', start);
  const body = src.slice(start + name.length + 4, end + 1);
  let out = '';
  for (const m of body.matchAll(/"((?:[^"\\]|\\.)*)"/g)) out += m[1];
  // Undo C escapes (\" \\ \n \t \uXXXX left for JS to interpret).
  return out.replace(/\\(["\\nt'])/g, (_, c) => ({ n: '\n', t: '\t' }[c] ?? c));
}

module.exports = { ROOT, cString };
