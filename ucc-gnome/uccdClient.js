/*
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * D-Bus client for the uccd daemon.
 *
 * Mirrors the functionality of libucc-dbus/UccdClient but in pure GJS/Gio.
 * All calls go to the system bus: com.uniwill.uccd /com/uniwill/uccd.
 */

import Gio from 'gi://Gio';
import GLib from 'gi://GLib';

const BUS_NAME    = 'com.uniwill.uccd';
const OBJECT_PATH = '/com/uniwill/uccd';
const IFACE_NAME  = 'com.uniwill.uccd';

/**
 * Asynchronous D-Bus wrapper; daemon delays must never block GNOME Shell.
 *
 * Connection state is tracked via Gio.bus_watch_name_on_connection().
 */
export class UccdClient {
    constructor() {
        this._bus = Gio.bus_get_sync(Gio.BusType.SYSTEM, null);
        this._connected = false;
        this._watchId = 0;
        this._onConnectionChanged = null;
        this._cancellable = new Gio.Cancellable();
        this._generation = 0;
        this._pendingReads = new Map();
        this._pendingWrites = new Map();
    }

    get connected() { return this._connected; }

    /**
     * Start watching the daemon service on the system bus.
     * @param {function(boolean)} cb Invoked with true/false on appear/vanish
     */
    watch(cb) {
        this._onConnectionChanged = cb;
        this._watchId = Gio.bus_watch_name_on_connection(
            this._bus, BUS_NAME, Gio.BusNameWatcherFlags.NONE,
            () => {
                this._cancellable.cancel();
                this._cancellable = new Gio.Cancellable();
                this._generation++;
                this._connected = true;
                cb?.(true);
            },
            () => {
                this._connected = false;
                this._generation++;
                this._cancellable.cancel();
                cb?.(false);
            },
        );
    }

    // Low-level helpers

    /** Return a promise; cancellation and daemon disappearance discard stale replies. */
    _request(method, args, signature, voidResult = false) {
        if (!this._connected) return Promise.resolve(voidResult ? false : null);
        const cancellable = this._cancellable;
        const generation = this._generation;
        return new Promise(resolve => {
            const failed = voidResult ? false : null;
            try {
                const params = args !== null
                    ? new GLib.Variant(`(${signature})`, args) : null;
                this._bus.call(
                    BUS_NAME, OBJECT_PATH, IFACE_NAME, method, params, null,
                    Gio.DBusCallFlags.NONE, 2000, cancellable,
                    (connection, asyncResult) => {
                        try {
                            const result = connection.call_finish(asyncResult);
                            if (!this._connected || generation !== this._generation ||
                                cancellable.is_cancelled()) {
                                resolve(failed);
                                return;
                            }
                            const value = result && result.n_children() > 0
                                ? result.get_child_value(0).recursiveUnpack() : null;
                            resolve(voidResult ? value !== false : value);
                        } catch (_e) { resolve(failed); }
                    },
                );
            } catch (_e) { resolve(failed); }
        });
    }

    _call(method, args = null, signature = null) {
        if (args !== null) return this._request(method, args, signature);
        const existing = this._pendingReads.get(method);
        if (existing?.generation === this._generation) return existing.promise;
        const entry = {generation: this._generation};
        entry.promise = this._request(method, args, signature).finally(() => {
            if (this._pendingReads.get(method) === entry) this._pendingReads.delete(method);
        });
        this._pendingReads.set(method, entry);
        return entry.promise;
    }

    _callVoid(method, args = null, signature = null) {
        // Slider motion may outpace BLE/sysfs writes. Keep one request in flight
        // and only its latest replacement; do not coalesce profile saves/actions.
        if (!['SetDisplayBrightness', 'SetWaterCoolerFanSpeed'].includes(method))
            return this._request(method, args, signature, true);
        return new Promise(resolve => {
            let state = this._pendingWrites.get(method);
            if (state && state.generation === this._generation) {
                state.next?.resolve(false); // superseded before it reached hardware
                state.next = {args, signature, resolve};
                return;
            }
            state?.next?.resolve(false);
            state = {generation: this._generation, next: {args, signature, resolve}};
            this._pendingWrites.set(method, state);
            const drain = async () => {
                while (state.next) {
                    const command = state.next;
                    state.next = null;
                    if (state.generation !== this._generation || !this._connected) {
                        command.resolve(false);
                        break;
                    }
                    command.resolve(await this._request(method, command.args, command.signature, true));
                }
                if (this._pendingWrites.get(method) === state) this._pendingWrites.delete(method);
            };
            drain();
        });
    }

    /**
     * Extract a value from fan data maps (GetFanDataCPU / GPU1 / GPU2).
     * Returns null when data is missing or has timestamp==0.
     *
     * The daemon returns a{sv} -> { speed: a{sv}{timestamp:x, data:i},
     * temp: a{sv}{timestamp:x, data:i} }
     */
    async _readFanData(method, key) {
        const outer = await this._call(method);
        if (!outer) return null;
        const inner = outer[key];
        if (!inner) return null;
        // Treat timestamp==0 as missing data (daemon not yet populated)
        if (inner.timestamp === 0 || inner.timestamp === 0n) return null;
        const v = Number(inner.data);
        return v >= 0 ? v : null;
    }

    /** Parse a JSON-returning D-Bus method and extract a numeric key. */
    async _readJsonNum(method, key) {
        const raw = await this._call(method);
        if (!raw) return null;
        try {
            const v = JSON.parse(raw)[key];
            return typeof v === 'number' && v >= 0 ? v : null;
        } catch { return null; }
    }

    // Monitoring - fast poll (temperatures, frequencies, power, fans)

    async getCpuTemperature() {
        return await this._readFanData('GetFanDataCPU', 'temp') ?? -1;
    }

    async getGpuTemperature() {
        // Prefer dGPU JSON, fall back to iGPU JSON
        return await this._readJsonNum('GetDGpuInfoValuesJSON', 'temp')
            ?? await this._readJsonNum('GetIGpuInfoValuesJSON', 'temp')
            ?? -1;
    }

    async getCpuFrequency() { return await this._call('GetCpuFrequencyMHz') ?? -1; }

    async getGpuFrequency() {
        return await this._readJsonNum('GetDGpuInfoValuesJSON', 'coreFrequency')
            ?? await this._readJsonNum('GetDGpuInfoValuesJSON', 'coreFreq')
            ?? -1;
    }

    async getCpuPower() {
        return await this._readJsonNum('GetCpuPowerValuesJSON', 'powerDraw') ?? -1;
    }

    async getGpuPower() {
        return await this._readJsonNum('GetDGpuInfoValuesJSON', 'powerDraw')
            ?? await this._readJsonNum('GetIGpuInfoValuesJSON', 'powerDraw')
            ?? -1;
    }

    async getFanSpeedRPM() {
        const pct = await this._readFanData('GetFanDataCPU', 'speed');
        return pct !== null ? pct * 60 : -1;
    }

    async getGpuFanSpeedRPM() {
        const g1 = await this._readFanData('GetFanDataGPU1', 'speed');
        const g2 = await this._readFanData('GetFanDataGPU2', 'speed');
        if (g1 !== null && g2 !== null) return Math.round((g1 + g2) / 2) * 60;
        if (g1 !== null) return g1 * 60;
        if (g2 !== null) return g2 * 60;
        return -1;
    }

    async getFanSpeedPercent() {
        return await this._readFanData('GetFanDataCPU', 'speed') ?? -1;
    }

    async getGpuFanSpeedPercent() {
        const g1 = await this._readFanData('GetFanDataGPU1', 'speed');
        const g2 = await this._readFanData('GetFanDataGPU2', 'speed');
        if (g1 !== null && g2 !== null) return Math.round((g1 + g2) / 2);
        return g1 ?? g2 ?? -1;
    }

    async getWaterCoolerFanSpeed()  { return await this._call('GetWaterCoolerFanSpeed')  ?? -1; }
    async getWaterCoolerPumpLevel() { return await this._call('GetWaterCoolerPumpLevel') ?? -1; }

    // Slow poll - profiles, state, hardware toggles

    async getActiveProfileJSON()   { return await this._call('GetActiveProfileJSON'); }
    async getPowerState()          { return await this._call('GetPowerState'); }
    async getDefaultProfilesJSON() { return await this._call('GetDefaultProfilesJSON'); }
    async getCustomProfilesJSON()  { return await this._call('GetCustomProfilesJSON'); }
    async getFanProfileNames()     { return await this._call('GetFanProfileNames'); }

    async getCustomFanProfiles()      { return await this._call('GetCustomFanProfilesJSON'); }
    async getCustomKeyboardProfiles() { return await this._call('GetCustomKeyboardProfilesJSON'); }

    async getWebcamEnabled()      { return await this._call('GetWebcamSWStatus'); }
    async getFnLock()             { return await this._call('GetFnLockStatus'); }
    async getDisplayBrightness()  { return await this._call('GetDisplayBrightness'); }

    async getAvailableODMProfiles()           { return await this._call('ODMProfilesAvailable') ?? []; }
    async getWaterCoolerSupported()           { return await this._call('GetWaterCoolerSupported'); }
    async isWaterCoolerEnabled()              { return await this._call('IsWaterCoolerEnabled'); }
    async isDeviceSupported()                 { return await this._call('IsDeviceSupported'); }
    async getKeyboardBacklightControlEnabled(){ return await this._call('GetKeyboardBacklightControlEnabled') ?? false; }
    async getSystemInfoJSON()                 { return await this._call('GetSystemInfoJSON'); }

    async getFanProfile(name) { return await this._call('GetFanProfile', [name], 's'); }

    // Setters

    async setActiveProfile(id) {
        return await this._callVoid('SetActiveProfile', [id], 's');
    }

    async applyFanProfiles(json) {
        return await this._callVoid('ApplyFanProfiles', [json], 's');
    }

    async setKeyboardBacklight(json) {
        return await this._callVoid('SetKeyboardBacklightStatesJSON', [json], 's');
    }

    async setWebcamEnabled(v) {
        // Try both method names for compatibility
        return await this._callVoid('SetWebcam', [v], 'b');
    }

    async setFnLock(v) {
        return await this._callVoid('SetFnLockStatus', [v], 'b');
    }

    async setDisplayBrightness(v) {
        return await this._callVoid('SetDisplayBrightness', [v], 'i');
    }

    async enableWaterCooler(v) {
        return await this._callVoid('EnableWaterCooler', [v], 'b');
    }

    async setWaterCoolerFanSpeed(percent) {
        return await this._callVoid('SetWaterCoolerFanSpeed', [percent], 'i');
    }

    async setWaterCoolerPumpVoltage(code) {
        return await this._callVoid('SetWaterCoolerPumpVoltage', [code], 'i');
    }

    async setWaterCoolerLEDColor(r, g, b, mode) {
        return await this._callVoid('SetWaterCoolerLEDColor', [r, g, b, mode], 'iiii');
    }

    async turnOffWaterCoolerLED() {
        return await this._callVoid('TurnOffWaterCoolerLED');
    }

    // Cleanup

    destroy() {
        this._generation++;
        this._cancellable.cancel();
        if (this._watchId) {
            Gio.bus_unwatch_name(this._watchId);
            this._watchId = 0;
        }
        this._connected = false;
    }
}
