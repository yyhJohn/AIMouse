import assert from 'node:assert/strict';
import { execFile, spawn } from 'node:child_process';
import { once } from 'node:events';
import { createInterface } from 'node:readline';
import { promisify } from 'node:util';

const [exe, probeExe, helper] = process.argv.slice(2);
const run = promisify(execFile);
const children = [];

function stream(file, args) {
    const child = spawn(file, args, { stdio: ['pipe', 'pipe', 'inherit'], windowsHide: true });
    children.push(child);
    const lines = createInterface({ input: child.stdout });
    const history = [];
    lines.on('line', line => history.push(JSON.parse(line)));
    const wait = async (predicate, begin = 0) => {
        const existing = history.slice(begin).find(predicate);
        if (existing) return existing;
        return new Promise((resolve, reject) => {
            function cleanup() {
                clearTimeout(timer);
                lines.off('line', onLine);
                child.off('exit', onExit);
            }
            function onLine(line) {
                const value = JSON.parse(line);
                if (!predicate(value)) return;
                cleanup();
                resolve(value);
            }
            function onExit(code) {
                cleanup();
                reject(new Error(`Child exited with ${code}`));
            }
            const timer = setTimeout(() => {
                cleanup();
                reject(new Error(`Timed out waiting for ${file}. Recent: ${JSON.stringify(history.slice(-5))}`));
            }, 5000);
            lines.on('line', onLine);
            child.once('exit', onExit);
        });
    };
    return { child, history, wait };
}

const input = stream(exe, ['--stdio']);
const host = async action => {
    const args = [String(input.child.pid)];
    if (action) args.push(action);
    const { stdout } = await run(helper, args, { windowsHide: true });
    return action ? undefined : JSON.parse(stdout);
};

function systemState({ x, y, foreground, buttons, keys }) {
    return { x, y, foreground, buttons, keys };
}

let nextId = 0;
function request(method, params) {
    const id = ++nextId;
    const reply = input.wait(value => value.id === id);
    input.child.stdin.write(JSON.stringify({ jsonrpc: '2.0', id, method, params }) + '\n');
    return reply;
}

async function tool(name, args = {}, expectedError) {
    const reply = await request('tools/call', { name, arguments: args });
    assert.equal(reply.error, undefined, JSON.stringify(reply.error));
    if (expectedError) {
        assert.equal(reply.result.isError, true, name);
        assert.equal(reply.result.structuredContent.code, expectedError, name);
    } else {
        assert.equal(reply.result.isError, false, JSON.stringify(reply.result.structuredContent));
        assert.equal(reply.result.structuredContent.ok, true);
    }
    return reply.result;
}

async function startProbe(mode, more = []) {
    const probe = stream(probeExe, [mode, '--timeout-ms', '18000', ...more]);
    const ready = await probe.wait(value => value.event === 'ready');
    assert.ok(ready.hwnd.startsWith('0x'));
    return { ...probe, ready };
}

async function bindAndArm(probe) {
    await tool('ai_bind_window', { hwnd: probe.ready.hwnd });
    assert.equal((await tool('ai_arm')).structuredContent.armed, false);
    await host('arm');
    assert.equal((await tool('ai_get_status')).structuredContent.armed, true);
}

function milliseconds(started) {
    return Number(process.hrtime.bigint() - started) / 1e6;
}

try {
    assert.equal((await request('tools/list')).error.code, -32002);
    const initialized = await request('initialize', {
        protocolVersion: '2025-06-18', capabilities: {}, clientInfo: { name: 'AIMouse tests', version: '0.1.0' },
    });
    assert.equal(initialized.result.protocolVersion, '2025-06-18');
    input.child.stdin.write('{"jsonrpc":"2.0","method":"notifications/initialized"}\n');
    assert.equal((await request('tools/list')).result.tools.length, 27);
    await tool('ai_create_user');
    const baseline = systemState(await host());
    const win32 = await startProbe('win32', ['--capturable']);
    const other = await startProbe('win32');
    const windows = (await tool('ai_list_windows')).structuredContent.windows;
    assert.ok(windows.some(window => window.hwnd === win32.ready.hwnd));
    await tool('ai_bind_window', { hwnd: win32.ready.hwnd });
    await tool('ai_click', {}, 'disarmed');
    await tool('ai_select_mode', { mode: 'isolated' }, 'guest_required');
    await tool('ai_move_absolute', { x: 1.5, y: 10 }, 'invalid_arguments');
    await tool('ai_move_absolute', { x: 1, y: 2, surprise: true }, 'invalid_arguments');
    await bindAndArm(win32);
    const delivered = (await tool('ai_move_absolute', { x: 80, y: 90 })).structuredContent;
    assert.equal(delivered.effect_verified, false);
    const cursor = await host();
    assert.equal(cursor.overlay, 1);
    assert.deepEqual(systemState(cursor), baseline);
    await tool('ai_set_cursor', { visible: false });
    assert.equal((await host()).overlay, 0);
    await tool('ai_set_cursor', { visible: true, rgb: 0x42c55e });
    await tool('ai_mouse_down');
    await tool('ai_move_absolute', { x: 180, y: 90 });
    await tool('ai_key_down', { vk: 0x57 });
    assert.deepEqual(systemState(await host()), baseline);
    await tool('ai_mouse_up');
    await tool('ai_key_up', { vk: 0x57 });
    await tool('ai_click', { button: 'right' });
    await tool('ai_double_click');
    await tool('ai_drag', { x: 230, y: 170, steps: 6 });
    await tool('ai_scroll', { delta: 120 });
    await tool('ai_type_text', { text: 'Hi' });
    await tool('ai_capture_screen', {}, 'screen_capture_denied');
    const screenshot = await tool('ai_capture_window');
    const image = screenshot.content.find(item => item.type === 'image');
    assert.equal(image.mimeType, 'image/png');
    const png = Buffer.from(image.data, 'base64');
    assert.equal(png.subarray(0, 8).toString('hex'), '89504e470d0a1a0a');
    assert.equal(png.readUInt32BE(16), screenshot.structuredContent.width);
    assert.equal(png.readUInt32BE(20), screenshot.structuredContent.height);
    const events = ['mouse_move', 'mouse_down', 'mouse_up', 'mouse_double_click',
        'mouse_wheel', 'key_down', 'key_up', 'text'];
    for (const event of events) await win32.wait(value => value.event === event);
    const desktopSample = await win32.wait(value => value.event === 'sample' && value.drag_moves >= 7);
    assert.equal(desktopSample.active, false);
    assert.ok(!other.history.some(value => events.includes(value.event)));

    const startIndex = win32.history.length;
    const latency = [];
    for (let index = 0; index < 250; index++) {
        const start = process.hrtime.bigint();
        await tool('ai_move_absolute', { x: 100 + index, y: 100 });
        latency.push(milliseconds(start));
    }
    await win32.wait(value => value.event === 'mouse_move' && value.x === 349 && value.y === 100, startIndex);
    const actualMoves = win32.history.slice(startIndex).filter(value => value.event === 'mouse_move').length;
    assert.equal(actualMoves, 250);
    latency.sort((a, b) => a - b);
    const percentile = p => Number(latency[Math.ceil(p * latency.length) - 1].toFixed(3));
    await tool('ai_mouse_down');
    await tool('ai_key_down', { vk: 0x57 });
    const cleanup = win32.history.length;
    await host('stop');
    assert.equal((await tool('ai_get_status')).structuredContent.armed, false);
    await win32.wait(value => value.event === 'mouse_up', cleanup);
    await win32.wait(value => value.event === 'key_up', cleanup);
    await tool('ai_click', {}, 'disarmed');
    assert.deepEqual(systemState(await host()), baseline);

    const raw = await startProbe('raw', ['--input-sink', '--borderless']);
    const rawBefore = await raw.wait(value => value.event === 'sample');
    await bindAndArm(raw);
    await tool('ai_move_relative', { dx: 40, dy: -15 });
    await tool('ai_click');
    const rawAfter = await raw.wait(value => value.event === 'sample' && value.moves === 1 && value.downs === 1);
    assert.equal(rawAfter.raw_mouse, rawBefore.raw_mouse);
    assert.equal(rawAfter.yaw, rawBefore.yaw);
    assert.equal(rawAfter.pitch, rawBefore.pitch);
    await tool('ai_select_mode', { mode: 'game' });
    await tool('ai_bind_window', { hwnd: raw.ready.hwnd });
    await tool('ai_arm', {}, 'unsupported_backend');
    const game = await startProbe('cooperative', ['--borderless']);
    await bindAndArm(game);
    assert.equal((await tool('ai_get_input_capabilities')).structuredContent.supports_relative_motion, true);
    assert.equal((await tool('ai_move_relative', { dx: 100, dy: -25 })).structuredContent.receiver_ack, true);
    const camera = await game.wait(value => value.event === 'cooperative_input' && value.op === 1);
    assert.equal(camera.yaw, 15);
    assert.equal(camera.pitch, 3.75);
    await tool('ai_drag', { x: 20, y: -5, steps: 5 });
    await tool('ai_scroll', { delta: 120 });
    await tool('ai_key_down', { vk: 0x57 });
    await tool('ai_mouse_down');
    const held = await game.wait(value => value.event === 'sample' && value.cooperative_keys_down === 1 &&
        value.cooperative_buttons === 1);
    assert.equal(held.cooperative_drag_moves, 5);
    assert.equal(held.cooperative_wheel, 120);
    assert.equal(held.active, false);
    assert.deepEqual(systemState(await host()), baseline);
    const resetIndex = game.history.length;
    const closed = once(input.child, 'exit');
    input.child.stdin.end();
    assert.equal((await closed)[0], 0);
    const reset = await game.wait(value => value.event === 'cooperative_input' && value.op === 5, resetIndex);
    assert.equal(reset.cooperative_keys_down, 0);
    assert.equal(reset.cooperative_buttons, 0);
    assert.deepEqual(systemState(await host()), baseline);

    console.log(JSON.stringify({
        test: 'AIMouse MCP end to end', win32_receiver_events: events, other_receiver_input_events: 0,
        commands: latency.length, received_moves: actualMoves, latency_ms: {
            p50: percentile(.50), p95: percentile(.95), p99: percentile(.99),
        },
        window_png: screenshot.structuredContent, system_state_unchanged_at_samples: true,
        raw_probe_desktop: { legacy_moves: rawAfter.moves, raw_mouse_delta: rawAfter.raw_mouse - rawBefore.raw_mouse,
            yaw_change: rawAfter.yaw - rawBefore.yaw, pitch_change: rawAfter.pitch - rawBefore.pitch },
        raw_probe_game_mode: 'unsupported_backend', cooperative_camera: camera,
        cooperative_drag_moves: held.cooperative_drag_moves, cleanup_on_stop_and_eof: true,
    }));
} finally {
    for (const child of children) {
        if (!child.pid || child.exitCode !== null || child.signalCode !== null) continue;
        if (child === input.child) child.stdin.end();
        else child.kill();
    }
}
