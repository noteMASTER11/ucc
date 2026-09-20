"""Exercise the real converter with synthetic data, without sysfs or private files."""
import copy
import importlib.util
import json
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('migration', Path(__file__).resolve().parents[1] / 'migrate-settings.py')
migration = importlib.util.module_from_spec(spec)
spec.loader.exec_module(migration)


class MigrationTest(unittest.TestCase):
    def setUp(self):
        self.settings = {
            'stateMap': {'power_ac': 'example', 'power_wc': 'old'},
            'chargingProfile': 'stationary',
            'keyboardBacklightStates': [{'brightness': 26, 'red': 1, 'green': 2, 'blue': 3} for _ in range(126)],
        }
        self.profiles = [{
            'id': 'example', 'name': 'Example', 'display': {'refreshRate': None},
            'cpu': {'governor': 'powersave', 'useMaxPerfGov': True},
            'fan': {'fanProfile': 'Balanced', 'useControl': True,
                    'minimumFanspeed': 0, 'maximumFanspeed': 100, 'offsetFanspeed': 20},
            'odmPowerLimits': {'tdpValues': [100, 120, 140]},
            'nvidiaPowerCTRLProfile': {'offset': 25},
        }]
        self.saved = {'deviceUUID': '02:00:00:00:00:01', 'red': 1, 'green': 2, 'blue': 3,
                      'ledMode': 0, 'ledOn': True, 'pumpOn': True, 'pumpVoltage': 0,
                      'fanOn': True, 'fanDutyCycle': 75}
        self.curves = {k: [{'temp': 60, 'speed': 33}, {'temp': 100, 'speed': 90}]
                       for k in ('tableCPU', 'tableGPU')}
        self.hardware = {'epp': 'default', 'hwpDynamicBoost': False, 'ctgpWritable': False}

    def convert(self):
        return migration.convert(self.settings, self.profiles,
            {'aquarisLastDeviceName': 'CoolingSystem LCT22002', 'aquarisSaveState': json.dumps(self.saved)},
            self.curves, self.hardware)

    def test_preserves_effective_controls_without_activating_gpu_writes(self):
        before = copy.deepcopy(self.profiles)
        settings, cooler = self.convert()
        profile = settings['profiles']['example']
        self.assertEqual(profile['fan']['tableCPU'], [{'temp': 60, 'speed': 53}, {'temp': 100, 'speed': 100}])
        self.assertEqual(profile['fan']['tablePump'], [{'temp': 0, 'speed': 3}, {'temp': 100, 'speed': 3}])
        self.assertEqual(profile['fan']['tableWaterCoolerFan'], [{'temp': 0, 'speed': 75}, {'temp': 100, 'speed': 75}])
        self.assertEqual(profile['cpu']['governor'], 'performance')
        self.assertEqual(profile['cpu']['energyPerformancePreference'], 'default')
        self.assertEqual(profile['odmPowerLimits']['tdpValues'], [100, 120, 140])
        self.assertEqual(profile['chargingProfile'], 'stationary')
        self.assertNotIn('nvidiaCTGPOffset', profile)
        self.assertNotIn('nvidiaPowerCTRLProfile', profile)
        self.assertEqual(settings['stateMap']['power_wc'], 'example')
        self.assertEqual(len(settings['customKeyboardProfiles']['tcc-keyboard']['json']['states']), 126)
        self.assertEqual(cooler['address'], '02:00:00:00:00:01')
        self.assertEqual(cooler['green'], 2)
        self.assertEqual(self.profiles, before)

    def test_preserves_disabled_cooler_outputs(self):
        self.saved.update(pumpOn=False, fanOn=False, ledOn=False)
        settings, cooler = self.convert()
        fan = settings['profiles']['example']['fan']
        self.assertEqual(fan['tablePump'], [{'temp': 0, 'speed': 0}, {'temp': 100, 'speed': 0}])
        self.assertEqual(fan['tableWaterCoolerFan'], [{'temp': 0, 'speed': 0}, {'temp': 100, 'speed': 0}])
        self.assertEqual((cooler['red'], cooler['green'], cooler['blue']), (0, 0, 0))

    def test_rejects_unsupported_voltage(self):
        self.saved['pumpVoltage'] = 1
        with self.assertRaisesRegex(ValueError, 'V12'):
            self.convert()

    def test_rejects_invalid_saved_address(self):
        self.saved['deviceUUID'] = 'not-an-address'
        with self.assertRaisesRegex(ValueError, 'address'):
            self.convert()


if __name__ == '__main__':
    unittest.main()
