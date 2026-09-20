// Exercise the actual indicator refresh methods without a live GNOME Shell.
const fs = require('node:fs');
const vm = require('node:vm');
const assert = require('node:assert/strict');
const source = fs.readFileSync(`${__dirname}/../ucc-gnome/extension.js`, 'utf8')
    .replace(/^import .*;\n/gm, '')
    .replace('export default class', 'class');
const context = vm.createContext({
    GObject: {registerClass: klass => klass},
    PanelMenu: {Button: class {destroy() {}}}, Extension: class {},
    logError: error => {throw error;},
});
vm.runInContext(source + '\nglobalThis.Indicator = UccIndicator;', context);
function indicator(client) {
    return Object.assign(new context.Indicator(), {
        _client: client, _pendingReads: new Map(), _connectionGeneration: 0,
        _destroyed: false, _state: {}, _capabilitiesLoaded: true, _profilesLoaded: true,
    });
}
async function main() {
    const timers = [];
    context.GLib = {PRIORITY_DEFAULT: 0, SOURCE_CONTINUE: true,
        timeout_add(_priority, _interval, callback) {timers.push(callback); return timers.length;}};
    const popup = indicator({connected: true});
    popup.menu = {isOpen: false};
    let hiddenPolls = 0;
    popup._pollMetrics = popup._pollSlowState = () => {hiddenPolls++;};
    popup._startTimers();
    timers.forEach(callback => callback());
    assert.equal(hiddenPolls, 0, 'Closed popup must not repeatedly poll hidden hardware controls');
    popup.menu.isOpen = true;
    timers.forEach(callback => callback());
    assert.equal(hiddenPolls, 2, 'Visible popup must keep refreshing');

    let finish, requests = 0, renders = 0;
    const delayed = new Promise(resolve => {finish = resolve;});
    const client = new Proxy({connected: true, destroy() {this.connected = false;}}, {
        get(target, name) {
            return name in target ? target[name] : () => {requests++; return delayed;};
    }});
    const ui = indicator(client);
    ui._updateDashboard = () => {renders++;};
    const refresh = ui._pollMetrics();
    for (let i = 0; i < 10; i++) await ui._pollMetrics();
    assert.equal(requests, 12, 'a slow poll must not accumulate duplicate batches');
    finish(42);
    await refresh;
    assert.equal(ui._state.cpuTemp, 42, 'UI state must receive values, not promises');
    assert.equal(renders, 1);

    let lateReply;
    const late = new Promise(resolve => {lateReply = resolve;});
    const pending = ui._readState('late', () => late, () => {renders++;});
    ui.destroy();
    lateReply([1]);
    await pending;
    assert.equal(renders, 1, 'destroyed UI must not receive late replies');

    const failingClient = new Proxy({connected: true}, {get(target, name) {
        return name in target ? target[name] : async () => null;
    }});
    const failureUI = indicator(failingClient);
    failureUI._state = {displayBrightness: 83, webcamEnabled: true, fnLock: true,
        wcEnabled: true, waterCoolerSupported: true};
    await failureUI._pollSlowState();
    assert.equal(failureUI._state.displayBrightness, 83, 'timeout must not reset brightness');
    assert.equal(failureUI._state.wcEnabled, true, 'timeout must not disable the cooler in UI');
    let keyboardPayload;
    const keyboardUI = indicator({setKeyboardBacklight: value => {keyboardPayload = value;}});
    keyboardUI._state = {keyboardProfileIds: ['saved'],
        keyboardProfilesData: [{json: {brightness: 26, states: []}}]};
    keyboardUI._applyKeyboardProfile('saved');
    assert.equal(typeof keyboardPayload, 'string', 'D-Bus keyboard setter requires a JSON string');
    assert.equal(JSON.parse(keyboardPayload).brightness, 26);
    console.log('PASS: bounded polling, resolved values, unload safety and failed-read preservation');
}
main().catch(error => {console.error(error); process.exitCode = 1;});
