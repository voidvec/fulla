"""Only 108 independent, intact measurement artifacts form a complete matrix."""
import json
import shutil
import tempfile
import unittest
from pathlib import Path

from evidence_test_support import write_measurement
from report import export
from run import SCENARIOS


class ReportTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.rows = []
        for profile in ('default', 'tuned'):
            for scenario in SCENARIOS:
                for vus in (1, 4):
                    for round_number in (1, 2, 3):
                        name = f'{profile}-{scenario}-{vus}-{round_number}'
                        row = {'id': name, 'kind': 'steady', 'status': 'passed', 'profile': profile,
                               'scenario': scenario, 'vus': vus, 'round': round_number,
                               'result_path': name + '/result.json'}
                        write_measurement(self.root / name, row)
                        self.rows.append(row)
        self.save()

    def save(self):
        (self.root / 'session.json').write_text(json.dumps(
            {'status': 'passed', 'mode': 'matrix', 'rows': self.rows,
             'driver_sha256': 'driver', 'binary_sha256': 'binary', 'fixture_nonce_sha256': 'nonce'}))

    def check(self, name, complete):
        report = export(self.root, self.root / (name + '.csv'))
        self.assertEqual(report['complete_steady_matrix'], complete)
        self.assertEqual(report['rows_exported'], 108)

    def test_missing_result_cannot_report_complete_matrix(self):
        self.check('complete', True)
        path = self.root / self.rows[0]['result_path']
        saved = path.read_text()
        path.unlink()
        self.check('missing', False)
        self.assertIn('missing_or_unreadable_result', (self.root / 'missing.csv').read_text())
        result = json.loads(saved)
        del result['result']['http']['cohort_wall_seconds']
        path.write_text(json.dumps(result))
        self.check('malformed', False)

    def test_missing_truncated_and_same_size_corruption_are_incomplete(self):
        directory = (self.root / self.rows[0]['result_path']).parent
        for filename in ('attempts.jsonl.gz', 'telemetry.jsonl.gz'):
            path = directory / filename
            saved = path.read_bytes()
            for name, data in [('missing', None), ('truncated', saved[:-3]),
                               ('same-size', bytes([saved[0] ^ 1]) + saved[1:]),
                               ('deflate', saved[:10] + b'\x07' + saved[-8:])]:
                with self.subTest(filename=filename, corruption=name):
                    if data is None:
                        path.unlink()
                    else:
                        path.write_bytes(data)
                    self.check(filename + name, False)
                    path.write_bytes(saved)

    def test_summary_and_sample_counts_must_match_raw(self):
        path = self.root / self.rows[0]['result_path']
        original = path.read_text()
        for field in ('stats', 'samples', 'seal', 'round', 'bad-build', 'bad-reasons'):
            result = json.loads(original)
            if field == 'stats':
                result['result']['http']['latency_ns_all_attempts']['p99'] += 1
            elif field == 'samples':
                result['observer']['samples'] += 1
            elif field == 'seal':
                del result['artifacts']
            elif field == 'bad-build':
                result['build'] = []
            elif field == 'bad-reasons':
                result['incomplete_reasons'] = None
            else:
                result['round'] = 9
            path.write_text(json.dumps(result))
            self.check(field, False)
        path.write_text(original)

    def test_36_results_cannot_impersonate_three_rounds(self):
        for i in range(0, len(self.rows), 3):
            for offset in (1, 2):
                self.rows[i + offset]['result_path'] = self.rows[i]['result_path']
        self.save()
        self.check('reused-paths', False)

    def test_alias_paths_and_copied_run_ids_are_rejected(self):
        first, second = self.rows[:2]
        saved = second.copy()
        second['result_path'] = './' + first['result_path']
        self.save()
        self.check('alias', False)
        second.update(saved)
        path = self.root / second['result_path']
        result = json.loads(path.read_text())
        result['run_id'] = first['run_id']
        second['run_id'] = first['run_id']
        path.write_text(json.dumps(result))
        self.save()
        self.check('copied-id', False)

    def test_result_path_cannot_escape_session(self):
        self.rows[0]['result_path'] = '../result.json'
        self.save()
        self.check('outside', False)

    def test_artifact_symlink_cannot_import_external_evidence(self):
        directory = (self.root / self.rows[0]['result_path']).parent
        for filename in ('attempts.jsonl.gz', 'telemetry.jsonl.gz'):
            path = directory / filename
            external = self.root / ('external-' + filename)
            path.rename(external)
            path.symlink_to(external)
            self.check('symlink-' + filename, False)
            path.unlink()
            external.rename(path)


if __name__ == '__main__':
    unittest.main()
