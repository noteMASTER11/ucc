// Run on a private D-Bus. A slow daemon must not block the client's event loop.
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import System from 'system';
import {UccdClient} from '../ucc-gnome/uccdClient.js';

const loop = new GLib.MainLoop(null, false);
const bus = Gio.DBus.session;
const xml = `<node><interface name="com.uniwill.uccd">
  <method name="GetDGpuInfoValuesJSON"><arg type="s" direction="out"/></method>
  <method name="GetActiveProfileJSON"><arg type="s" direction="out"/></method>
  <method name="SetWaterCoolerFanSpeed"><arg type="i" direction="in"/><arg type="b" direction="out"/></method>
</interface></node>`;
function check(value, message) { if (!value) throw new Error(message); }
function later(fn) {
    GLib.timeout_add(GLib.PRIORITY_DEFAULT, 300, () => { fn(); return GLib.SOURCE_REMOVE; });
}
let gpuReads = 0;
let fanWrites = [];
const service = Gio.DBusExportedObject.wrapJSObject(xml, {
    GetDGpuInfoValuesJSONAsync(_args, invocation) {
        gpuReads++;
        later(() => invocation.return_value(new GLib.Variant('(s)', ['{"temp":52,"coreFrequency":1200,"powerDraw":45}'])));
    },
    GetActiveProfileJSONAsync(_args, invocation) {
        later(() => invocation.return_value(new GLib.Variant('(s)', ['{"id":"test"}'])));
    },
    SetWaterCoolerFanSpeedAsync(args, invocation) {
        fanWrites.push(args[0]);
        later(() => invocation.return_value(new GLib.Variant('(b)', [args[0] !== 0])));
    },
});
service.export(bus, '/com/uniwill/uccd');
let beats = 0;
const heartbeat = GLib.timeout_add(GLib.PRIORITY_DEFAULT, 10, () => {
    beats++;
    return GLib.SOURCE_CONTINUE;
});
let client;
let failed = false;
async function run() {
    client = new UccdClient();
    await new Promise(resolve => client.watch(connected => { if (connected) resolve(); }));
    let before = beats;
    const read = client.getActiveProfileJSON();
    check(read?.then, 'Reading the daemon must return a promise without blocking');
    check(JSON.parse(await read).id === 'test', 'Read result must be preserved');
    check(beats - before >= 10, 'Event loop must keep running during a slow read');
    before = beats;
    const write = client.setWaterCoolerFanSpeed(73);
    check(write?.then, 'Sending a command must return a promise without blocking');
    check(await write, 'Command must finish successfully');
    check(beats - before >= 10, 'Event loop must keep running during a slow command');
    const metrics = await Promise.all([client.getGpuTemperature(), client.getGpuFrequency(), client.getGpuPower()]);
    check(metrics[0] === 52 && metrics[1] === 1200 && metrics[2] === 45, 'GPU fields must use the same source reply');
    check(gpuReads === 1, 'One refresh must share the in-flight GPU read');
    fanWrites = [];
    const commands = [];
    for (let value = 10; value <= 90; value++) commands.push(client.setWaterCoolerFanSpeed(value));
    const results = await Promise.all(commands);
    check(fanWrites.length <= 2, 'Dragging a slider must not enqueue every intermediate hardware command');
    check(fanWrites.at(-1) === 90 && results.at(-1), 'The final requested value must be delivered');
    check(!await client.setWaterCoolerFanSpeed(0), 'A false daemon reply must not be reported as success');
    const pending = client.getActiveProfileJSON();
    const runningWrite = client.setWaterCoolerFanSpeed(40);
    const queuedWrite = client.setWaterCoolerFanSpeed(50);
    client.destroy();
    check(await pending === null, 'Unloading the extension must discard pending replies');
    check(!await runningWrite && !await queuedWrite, 'Unloading must cancel pending and queued slider writes');
    check(!fanWrites.includes(50), 'A queued write must not reach hardware after unloading');
    print('PASS: slow reads, slow commands and cancellation leave the event loop responsive');
}
const owner = Gio.bus_own_name_on_connection(bus, 'com.uniwill.uccd', Gio.BusNameOwnerFlags.NONE,
    () => run().catch(error => { printerr(error.message + '\n' + error.stack); failed = true; }).finally(() => loop.quit()), null);
loop.run();
client?.destroy();
GLib.source_remove(heartbeat);
Gio.bus_unown_name(owner);
service.unexport();
System.exit(failed ? 1 : 0);
