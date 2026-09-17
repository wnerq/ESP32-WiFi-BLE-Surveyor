"""Offline regression tests for build/deployment validation; never contact hardware."""
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import validate


def healthy():
    return {
        'firmware': {'version': '45'},
        'systemHealth': {key: 'PASS' for key in ('overall', 'wifiHistoryBuffer',
            'wifiHistoryIntegrity', 'bleHistoryBuffer', 'scanConfiguration')},
        'wifiSurvey': {'observationsRetained': 241, 'observationCapacity': 482,
            'apTableUsed': 241, 'apTableCapacity': 482, 'historyIntegrityAnomalies': 0,
            'terminalBufferBytes': 0, 'plotsEnabled': False, 'inventory': True,
            'historicalMeasurements': 0}}


class ValidationTests(unittest.TestCase):
    def test_inventory_and_history(self):
        status = healthy()
        self.assertEqual(validate.check_status(status, 'V45'), [])
        status['wifiSurvey'].update(inventory=False, observationsRetained=4500,
                                   observationCapacity=5000, historicalMeasurements=4500)
        self.assertEqual(validate.check_status(status), [])

    def test_bounds_integrity_and_types(self):
        for field, value in [('observationsRetained', 483), ('apTableCapacity', 0),
                             ('historyIntegrityAnomalies', 1), ('terminalBufferBytes', 4096),
                             ('terminalBufferBytes', False), ('plotsEnabled', 'false'),
                             ('historicalMeasurements', 5), ('inventory', None)]:
            status = healthy()
            status['wifiSurvey'][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                validate.check_status(status)

    def test_health_and_version(self):
        status = healthy()
        with self.assertRaises(ValueError):
            validate.check_status(status, '41')
        status['systemHealth']['memory'] = 'WARN'
        self.assertEqual(validate.check_status(status), ['memory'])
        status['systemHealth']['overall'] = 'FAIL'
        with self.assertRaises(ValueError):
            validate.check_status(status)

    def test_get_only_smoke(self):
        responses = {'/status.json': json.dumps(healthy()),
            '/config.json': json.dumps({'terminalBufferBytes': 0, 'plotsEnabled': False}),
            '/settings': 'Developer Memory', '/help': 'developer-memory',
            '/': 'esp32-card-layout-v1:', '/terminal': 'Active terminal capture:',
            '/api/terminal?cursor=0&boot=0': ''}
        seen = []
        class Response:
            status = 200
            headers = {'X-Terminal-Capacity': '0'}
            def __init__(self, text): self.text = text
            def __enter__(self): return self
            def __exit__(self, *args): pass
            def read(self, maximum): return self.text.encode()[:maximum]
        def get(url, timeout):
            self.assertEqual(timeout, 30)
            path = url.removeprefix('http://device.test')
            seen.append(path)
            return Response(responses[path])
        with patch.object(validate, 'urlopen', side_effect=get):
            self.assertEqual(validate.check_device('http://device.test', '45'), [])
            responses['/config.json'] = json.dumps({'terminalBufferBytes': 1024, 'plotsEnabled': False})
            self.assertTrue(validate.check_device('http://device.test'))
        self.assertEqual(set(seen), set(responses))
        for url in ('file:///tmp', 'http://device.test/scan-now', 'http://user:pass@device.test'):
            with self.assertRaises(ValueError):
                validate.check_device(url)

    def test_stale_generated_sketch(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'src').mkdir()
            (root / 'arduino/WifiConnect').mkdir(parents=True)
            (root / 'src/main.cpp').write_text('source')
            (root / 'arduino/WifiConnect/WifiConnect.ino').write_text('outdated')
            with self.assertRaisesRegex(ValueError, 'stale'):
                validate.check_source(root)


if __name__ == '__main__':
    unittest.main()
