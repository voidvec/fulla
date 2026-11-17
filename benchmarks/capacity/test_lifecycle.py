"""Owned child cleanup, resumable transfer and retained shutdown evidence."""
from contextlib import contextmanager
import json
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

import fixture
from faults import stop_with_evidence
from session import Experiment
from protocol import ProtocolError


class ChildCleanupTests(unittest.TestCase):
    def test_first_cleanup_interrupt_is_propagated_after_reap(self):
        child = Mock()
        child.poll.return_value = None
        child.wait.side_effect = [KeyboardInterrupt(), KeyboardInterrupt(), 0]
        exp = Experiment.__new__(Experiment)
        exp.current_child = child
        with self.assertRaises(KeyboardInterrupt):
            exp.stop_child()
        self.assertIsNone(exp.current_child)
        self.assertEqual(child.wait.call_count, 3)
        self.assertTrue(child.kill.called)

    def test_parent_only_interrupt_reaps_before_server_cleanup_and_unlock(self):
        self.exercise_parent_interrupt()

    def test_assignment_gap_interrupt_also_reaps_before_cleanup(self):
        self.exercise_parent_interrupt(assignment_gap=True)

    def exercise_parent_interrupt(self, assignment_gap=False):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'logs').mkdir()
            class InterruptedAssignment(Experiment):
                def __setattr__(self, name, value):
                    super().__setattr__(name, value)
                    if name == 'current_child' and value is not None:
                        raise KeyboardInterrupt()
            cls = InterruptedAssignment if assignment_gap else Experiment
            exp = cls.__new__(cls)
            exp.out = root
            exp.rows = []
            exp.args = SimpleNamespace(fixture=root / 'fixture.json', max_attempts=10)
            exp.reset = Mock()
            exp.start_server = Mock()
            exp.db = Mock()
            exp.db.stats.return_value = {}
            exp.save_manifest = Mock()
            events = []
            exp.server = SimpleNamespace(stop=lambda: events.append('server-stop'), shutdown_events=[])
            child = Mock()
            state = {'reaped': False, 'waits': 0, 'cleanup_interrupt': False}
            child.poll.side_effect = lambda: 0 if state['reaped'] else None

            def wait(*args, **kwargs):
                state['waits'] += 1
                if state['waits'] == 1 and not assignment_gap:
                    raise KeyboardInterrupt()
                if kwargs.get('timeout') == 15:
                    raise subprocess.TimeoutExpired('owned', 15)
                if not kwargs and not state['cleanup_interrupt']:
                    state['cleanup_interrupt'] = True
                    raise KeyboardInterrupt()
                state['reaped'] = True
                events.append('reaped')
                return 0

            child.wait.side_effect = wait
            exp.current_child = None

            @contextmanager
            def lease():
                try:
                    yield
                finally:
                    events.append('unlock')

            with patch('session.subprocess.Popen', return_value=child):
                with self.assertRaises(KeyboardInterrupt):
                    with lease():
                        exp.run_row('default', 'discovery', 1, 1, 'smoke', 0, 0.1)
            self.assertTrue(child.send_signal.called)
            self.assertTrue(child.kill.called)
            self.assertIsNone(exp.current_child)
            self.assertLess(events.index('reaped'), events.index('server-stop'))
            self.assertLess(events.index('reaped'), events.index('unlock'))

    def test_execute_still_cleans_server_proxy_monitor_and_manifest_after_interrupt(self):
        events = []
        exp = Experiment.__new__(Experiment)
        exp.args = SimpleNamespace(mode='smoke')
        exp.manifest = {'status': 'incomplete'}
        exp.monitor = Mock(ident=1)
        exp.monitor.is_alive.return_value = False
        exp.initialize = Mock(side_effect=ValueError('abort initialization'))
        exp.server = SimpleNamespace(stop=lambda: events.append('server-stop'))
        exp.proxy = Mock()
        exp.proxy.terminate.side_effect = lambda: events.append('proxy-stop')
        exp.proxy_log = Mock()
        exp.monitor_stop = Mock()
        exp.space_violation = Mock()
        exp.space_violation.is_set.return_value = False
        exp.minimum_free_bytes = 123
        exp.resource_monitor_error = None
        exp.save_manifest = lambda: events.append('save')
        def cleanup():
            events.append('reaped')
            raise KeyboardInterrupt()
        exp.stop_child = cleanup
        try:
            with self.assertRaises(KeyboardInterrupt):
                exp.execute()
        finally:
            events.append('unlock')
        self.assertEqual(events, ['reaped', 'server-stop', 'proxy-stop', 'save', 'unlock'])
        exp.monitor_stop.set.assert_called_once()
        exp.monitor.join.assert_called_once_with(timeout=2)

    def test_real_owned_child_is_reaped(self):
        child = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(30)'],
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.addCleanup(lambda: child.kill() if child.poll() is None else None)
        exp = Experiment.__new__(Experiment)
        exp.current_child = child
        exp.stop_child()
        self.assertIsNotNone(child.returncode)
        self.assertIsNone(exp.current_child)


class OrganizationRecoveryTests(unittest.TestCase):
    def call(self, owner, *, lose_response=False):
        account = {'username': 'owned', 'org_client': {'id': 'owned-client'}}
        fx = {'target': 'http://127.0.0.1:1', 'accounts': [account]}
        session = Mock()
        user = Mock()
        user.userinfo.return_value = {'sub': 'owned-sub'}
        transfers = []
        state = {'owner': owner}

        def request(stage, *args, **kwargs):
            if stage == 'org_list_setup':
                body = {'organizations': [{'slug': 's07-owned', 'role': 'owner', 'id': 7}]}
            elif stage == 'app_owner_setup':
                body = {'applications': [{'client_id': 'owned-client', 'org_id': state['owner']}]}
            elif stage == 'app_transfer':
                transfers.append(stage)
                if state['owner'] is not None:
                    raise ProtocolError('already-owned', stage, 400)
                state['owner'] = 7
                if lose_response:
                    raise ProtocolError('response-lost', stage, 0)
                body = {}
            else:
                body = {}
            return {'status': 200, 'body': json.dumps(body).encode()}

        session.request.side_effect = request
        return account, fx, session, user, transfers

    def test_committed_transfer_with_lost_ack_recovers_without_replay(self):
        account, fx, session, user, transfers = self.call(None, lose_response=True)
        with patch.object(fixture, 'Session', return_value=session), patch.object(fixture, 'User', return_value=user):
            with self.assertRaises(ProtocolError):
                fixture.create_organizations(fx)
            self.assertNotIn('org_ready', account)
            fixture.create_organizations(fx)
        self.assertTrue(account['org_ready'])
        self.assertEqual(len(transfers), 1)
        self.assertEqual(session.close.call_count, 2)

    def test_personal_transfers_but_other_org_or_malformed_owner_fails(self):
        for owner in (None, 8, '7', True):
            with self.subTest(owner=owner):
                account, fx, session, user, transfers = self.call(owner)
                with patch.object(fixture, 'Session', return_value=session), patch.object(fixture, 'User', return_value=user):
                    if owner is None:
                        fixture.create_organizations(fx)
                        self.assertTrue(account['org_ready'])
                        self.assertEqual(len(transfers), 1)
                    else:
                        with self.assertRaises(ProtocolError):
                            fixture.create_organizations(fx)
                        self.assertFalse(account.get('org_ready', False))
                        self.assertEqual(transfers, [])


class ShutdownEvidenceTests(unittest.TestCase):
    def server(self, root, name, *, force=False, code=-11):
        server = fixture.Server({}, root, root / 'binary', root / name)
        server.process = Mock(pid=42)
        server.process.wait.side_effect = [subprocess.TimeoutExpired('owned', 8), code] if force else [code]
        return server

    def test_both_instances_persist_nonzero_and_forced_exit_without_duplicates(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            result = {'status': 'passed'}
            first = self.server(root, 'initial', force=True, code=-9)
            stop_with_evidence(first, result, root)
            saved = json.loads((root / 'result.json').read_text())
            self.assertEqual(saved['server_shutdowns'][0]['exit_code'], -9)
            self.assertTrue(saved['server_shutdowns'][0]['forced_kill'])
            second = self.server(root, 'restored')
            stop_with_evidence(second, result, root)
            stop_with_evidence(first, result, root)
            saved = json.loads((root / 'result.json').read_text())
            self.assertEqual(len(saved['server_shutdowns']), 2)
            self.assertNotEqual(*[e['instance'] for e in saved['server_shutdowns']])
            self.assertEqual(saved['server_shutdowns'][1]['exit_code'], -11)
            self.assertFalse(saved['server_shutdowns'][1]['forced_kill'])
            self.assertIn('#300 excluded', saved['shutdown_policy'])

    def test_collector_failure_still_persists_observed_exit(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            server = self.server(root, 'collector-failed')
            server.log_thread = Mock()
            server.log_thread.is_alive.return_value = True
            result = {'status': 'passed'}
            with self.assertRaises(RuntimeError):
                stop_with_evidence(server, result, root)
            saved = json.loads((root / 'result.json').read_text())
            self.assertEqual(saved['status'], 'incomplete')
            self.assertEqual(saved['server_shutdowns'][0]['exit_code'], -11)


if __name__ == '__main__':
    unittest.main()
