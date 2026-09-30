// Native bridge ordering regression; no browser, user profile or printer access.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const { test } = require('node:test');
const source = fs.readFileSync(path.join(__dirname, '../resources/web/guide/0/load.js'), 'utf8');

function fixture(target = '23') {
    let now = 0, nextId = 0;
    const timers = new Map(), navigations = [], sent = [], tip = { textContent: 'Loading' };
    const ctx = vm.createContext({
        Date: { now: () => now }, TranslatePage() {}, GetQueryString: () => target,
        SendWXMessage: value => sent.push(JSON.parse(value)),
        setTimeout: (fn, delay) => { const id = ++nextId; timers.set(id, {fn, at: now + delay}); return id; },
        clearTimeout: id => timers.delete(id),
        document: { getElementById: () => tip },
        window: { open: url => navigations.push(url) }
    });
    vm.runInContext(source, ctx);
    return { ctx, navigations, sent, tip, timers, advance(ms) {
        now += ms;
        for (const [id, timer] of [...timers]) {
            if (timer.at <= now) { timers.delete(id); timer.fn(); }
        }
    }};
}
const ready = { command: 'userguide_profile_load_finish' };
test('early completion is retained until the target is initialized', () => {
    const f = fixture(); f.ctx.HandleStudio(ready);
    assert.equal(f.navigations.length, 0);
    f.ctx.OnInit(); f.ctx.HandleStudio(ready);
    assert.deepEqual(f.navigations, ['../23/index.html']);
    assert.equal(f.timers.size, 0);
});
test('a lost completion is recovered by polling native readiness', () => {
    const f = fixture(); f.ctx.OnInit(); f.advance(1000);
    assert.equal(f.sent.length, 2);
    assert.equal(f.sent[1].command, 'request_userguide_load_status');
    f.ctx.HandleStudio(ready);
    assert.deepEqual(f.navigations, ['../23/index.html']);
    assert.equal(f.timers.size, 0);
});
test('an error leaves a readable failure, not endless Loading', () => {
    const f = fixture(); f.ctx.OnInit();
    f.ctx.HandleStudio({command: 'userguide_profile_load_error', message: 'Preset load failed'});
    assert.equal(f.tip.textContent, 'Preset load failed');
    assert.equal(f.timers.size, 0);
    assert.equal(f.navigations.length, 0);
});
test('timeout never navigates into incomplete profile data', () => {
    const f = fixture(); f.ctx.OnInit(); f.advance(180000);
    assert.equal(f.navigations.length, 0);
    assert.match(f.tip.textContent, /timed out/);
    assert.equal(f.timers.size, 0);
});
test('all supported destinations are retained and invalid ones rejected', () => {
    for (const target of ['1', '11', '21', '22', '23', '24']) {
        const f = fixture(target); f.ctx.OnInit(); f.ctx.HandleStudio(ready);
        assert.deepEqual(f.navigations, [`../${target}/index.html`]);
    }
    const f = fixture('../bad'); f.ctx.OnInit(); f.ctx.HandleStudio(ready);
    assert.equal(f.navigations.length, 0);
});
