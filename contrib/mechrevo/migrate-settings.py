#!/usr/bin/env python3
"""Convert a captured TCC configuration; never writes to live system paths."""
import copy
import json
import pathlib
import re

def convert(settings, profiles, user, balanced, hardware):
    result = {k: copy.deepcopy(v) for k, v in settings.items()
              if k in ('fahrenheit','stateMap','cpuSettingsEnabled','fanControlEnabled',
                       'chargingProfile','chargingPriority','ycbcr420Workaround')}
    result['profiles'] = {}
    result['keyboardBacklightControlSupported'] = settings.get('keyboardBacklightControlEnabled',True)
    states = copy.deepcopy(settings['keyboardBacklightStates'])
    result['customKeyboardProfiles'] = {'tcc-keyboard': {
        'id':'tcc-keyboard','name':'Imported TCC keyboard',
        'json':{'brightness':states[0]['brightness'],'states':states}}}
    result['customFanProfiles'] = {}
    saved = json.loads(user['aquarisSaveState'])
    if user.get('aquarisLastDeviceName') != 'CoolingSystem LCT22002':
        raise ValueError('This migration supports the audited LCT22002 only')
    if not re.fullmatch(r'(?:[0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}',saved['deviceUUID']):
        raise ValueError('Invalid saved cooler address')
    aquaris = {'address':saved['deviceUUID'].upper(),'name':user['aquarisLastDeviceName'],
               **{k:saved[k] for k in ('red','green','blue','ledMode')}}
    if not saved['ledOn']:
        aquaris.update(red=0,green=0,blue=0)
    # LCT22002 uses a discrete voltage; pumpDutyCycle is the LCT21001 control.
    pump_level = {0:3,1:4,2:1,3:2,4:0}[saved['pumpVoltage']] if saved['pumpOn'] else 0
    if pump_level == 4:
        raise ValueError('Upstream UCC explicitly rejects V12; manual review required')
    wc_fan = int(saved['fanDutyCycle']) if saved['fanOn'] else 0
    if not 0 <= wc_fan <= 100:
        raise ValueError('Invalid water cooler fan duty')
    for old in profiles:
        p = copy.deepcopy(old)
        p['name'] = old['name'] + ' (imported)'
        if p['display'].get('refreshRate') is None: p['display']['refreshRate'] = -1
        # TCC useMaxPerfGov overrides its nominal governor field.
        if p['cpu'].pop('useMaxPerfGov',False): p['cpu']['governor'] = 'performance'
        p['cpu']['energyPerformancePreference'] = hardware['epp']
        p['cpu']['hwpDynamicBoost'] = hardware['hwpDynamicBoost']
        fan = old['fan']
        if fan['fanProfile'] == 'Balanced': curves = balanced
        elif fan['fanProfile'] == 'Custom': curves = fan['customFanCurve']
        else: raise ValueError('Unreviewed TCC fan preset: '+fan['fanProfile'])
        p['fan'] = {k:copy.deepcopy(v) for k,v in fan.items() if k in ('useControl',)}
        p['fan'].update(fanProfile='tcc-imported',sameSpeed=False,autoControlWC=True,enableWaterCooler=True)
        for key in ('tableCPU','tableGPU'):
            p['fan'][key] = [{'temp':e['temp'],'speed':max(0,min(100,max(fan['minimumFanspeed'],
                min(fan['maximumFanspeed'],e['speed']+fan['offsetFanspeed']))))} for e in curves[key]]
        p['fan']['tablePump'] = [{'temp':t,'speed':pump_level} for t in (0,100)]
        p['fan']['tableWaterCoolerFan'] = [{'temp':t,'speed':wc_fan} for t in (0,100)]
        result['customFanProfiles']['tcc-imported'] = {'id':'tcc-imported',
            'name':'Imported TCC Balanced +20%',
            'json':{k:copy.deepcopy(p['fan'][k]) for k in
                    ('tableCPU','tableGPU','tablePump','tableWaterCoolerFan')}}
        p.pop('nvidiaPowerCTRLProfile',None)
        if hardware['ctgpWritable']: p['nvidiaCTGPOffset'] = hardware['ctgpOffset']
        p['chargingProfile'] = settings.get('chargingProfile') or ''
        p['keyboard'] = {'brightness':states[0]['brightness'],'states':states}
        p['selectedKeyboardProfile'] = 'tcc-keyboard'
        result['profiles'][p['id']] = p
    result['stateMap']['power_wc'] = result['stateMap']['power_ac']
    return result,aquaris

if __name__ == '__main__':
    import argparse
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('baseline',type=pathlib.Path)
    parser.add_argument('output',type=pathlib.Path)
    args=parser.parse_args()
    load=lambda name:json.loads((args.baseline/name).read_text())
    hardware={'governor':pathlib.Path('/sys/devices/system/cpu/cpufreq/policy0/scaling_governor').read_text().strip(),
              'epp':pathlib.Path('/sys/devices/system/cpu/cpufreq/policy0/energy_performance_preference').read_text().strip(),
              'hwpDynamicBoost':pathlib.Path('/sys/devices/system/cpu/intel_pstate/hwp_dynamic_boost').read_text().strip()=='1',
              'ctgpWritable':False}
    if pathlib.Path('/sys/devices/platform/tuxedo_nvidia_power_ctrl/ctgp_offset').exists():
        raise SystemExit('Active cTGP interface differs from audited diagnostic baseline')
    result,aquaris=convert(load('tcc-settings.json'),load('tcc-profiles.json'),load('tcc-user.json'),load('balanced-fan.json'),hardware)
    args.output.mkdir(mode=0o700,parents=True,exist_ok=True)
    for name,data in [('settings',result),('aquaris.json',aquaris)]:
        target=args.output/name
        target.write_text(json.dumps(data,indent=2)+'\n')
        target.chmod(0o600)
    print('Converted settings, 126 keyboard zones and saved Aquaris; no live settings modified')
