// SPDX-License-Identifier: MIT
// A simulated board on Web Serial that answers Improv exactly like components/improv
// (including our vendor status command 0xF0). Injected with page.addInitScript.
function fakeSerial(opts) {
  const { deviceVersion = 'abc1234', ssid: startSsid = '', saved = '', rejoinAfter = 0,
          password = 'correct horse', noise = '' } = opts || {};
  const HEADER = [73, 77, 80, 82, 79, 86];
  const enc = new TextEncoder();
  let ssid = startSsid, statusCalls = 0, ctrl;
  const pkt = (type, data) => {
    const p = [...HEADER, 1, type, data.length, ...data];
    p.push(p.reduce((a, b) => (a + b) & 255, 0));
    p.push(10);
    return new Uint8Array(p);
  };
  const result = (cmd, strs) => {
    const d = [];
    for (const s of strs) { const b = enc.encode(s); d.push(b.length, ...b); }
    return pkt(4, [cmd, d.length, ...d]);
  };
  const emit = (u8) => ctrl.enqueue(u8);
  const log = (t) => emit(enc.encode(t));
  const handle = async (cmd, d) => {
    if (noise) log(noise);                          // log output between packets
    if (cmd === 3) emit(result(3, ['ESP32 Voice Assistant', deviceVersion, 'ESP32-S3', 'assistant']));
    if (cmd === 0xF0) {
      statusCalls++;
      if (rejoinAfter && statusCalls > rejoinAfter) ssid = saved || 'HomeNet';
      emit(result(0xF0, ssid ? [ssid, 'http://192.168.1.77/', '-58', ssid] : ['', '', '', saved]));
    }
    if (cmd === 4) {
      for (const [s, r, a] of [['HomeNet', '-48', 'YES'], ['Cafe', '-70', 'NO'], ['Neighbor', '-80', 'YES']]) {
        emit(result(4, [s, r, a]));
      }
      emit(result(4, []));
    }
    if (cmd === 1) {
      const sl = d[0], s = new TextDecoder().decode(new Uint8Array(d.slice(1, 1 + sl)));
      const pl = d[1 + sl], p = new TextDecoder().decode(new Uint8Array(d.slice(2 + sl, 2 + sl + pl)));
      emit(pkt(2, [0])); emit(pkt(1, [3]));
      await new Promise((r) => setTimeout(r, 200));
      if (p === password) { ssid = s; emit(pkt(1, [4])); emit(result(1, ['http://192.168.1.77/'])); }
      else { emit(pkt(2, [3])); emit(pkt(1, [2])); }
    }
  };
  let buf = [];
  const port = {
    getInfo: () => ({ usbVendorId: 0x303a, usbProductId: 0x1001 }),
    async open() {
      this.readable = new ReadableStream({ start(c) { ctrl = c; log('I (1047) main_task: Started on CPU0\n'); } });
      this.writable = new WritableStream({ write(chunk) {
        buf.push(...chunk);
        for (;;) {
          const i = buf.findIndex((_, k) => HEADER.every((h, j) => buf[k + j] === h));
          if (i < 0 || buf.length < i + 10) return;
          const len = buf[i + 8];
          if (buf.length < i + 10 + len) return;
          const p = buf.splice(0, i + 10 + len + 1).slice(i);
          if (p[7] === 3) handle(p[9], p.slice(11, 9 + len));
        }
      } });
    },
    async setSignals() {}, async close() {},
  };
  navigator.serial = { requestPort: async () => port, getPorts: async () => [port], addEventListener() {} };
}

module.exports = { fakeSerial };
