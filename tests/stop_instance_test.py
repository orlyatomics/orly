#!/usr/bin/env python3
"""Disk-instance isolation checks for the stop/restart drivers; no server or root required."""

import importlib.util
from pathlib import Path
import sys
from types import SimpleNamespace
import unittest
from unittest.mock import MagicMock, mock_open, patch


def load_driver(name):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(f'{name}.py'))
    module = importlib.util.module_from_spec(spec)
    with patch.dict(sys.modules, {'orly': MagicMock()}):
        spec.loader.exec_module(module)
    return module


DRIVERS = [load_driver('graceful_stop_test'), load_driver('kill_campaign')]


class StopInstanceTest(unittest.TestCase):
    def args(self):
        return SimpleNamespace(volume_gb=1, orly_out='out', port=19940,
                               orlyi_prefix='', extra_args='', update_pool_size=100000,
                               startup_timeout=1)

    def test_instances_are_unique_and_fit_disk_header(self):
        names = [driver.Server(self.args(), 'work').instance
                 for driver in DRIVERS for _ in range(100)]
        self.assertEqual(len(names), len(set(names)))
        for name in names:
            # TVolumeId stores the name in a 24-byte character array, including the terminator.
            self.assertLess(len(name.encode('ascii')), 24)

    def test_volume_and_all_restarts_use_the_same_instance(self):
        for driver in DRIVERS:
            with self.subTest(driver=driver.__name__):
                srv = driver.Server(self.args(), 'work')
                with patch.object(driver.subprocess, 'check_call') as check_call, \
                        patch.object(driver.subprocess, 'check_output', return_value='/dev/loop7\n'), \
                        patch.object(driver.subprocess, 'Popen') as popen, \
                        patch.object(driver.socket, 'create_connection'), \
                        patch('builtins.open', mock_open()):
                    popen.return_value.poll.return_value = None
                    srv.create_volume()
                    volume_cmd = check_call.call_args_list[-1].args[0]
                    self.assertIn(f'--instance-name={srv.instance}', volume_cmd)
                    self.assertEqual(volume_cmd[-1], 'loop7')
                    for create in (True, False, False):
                        self.assertIsNone(srv.start(create))
                        server_cmd = popen.call_args.args[0]
                        self.assertIn(f'--instance_name={srv.instance}', server_cmd)
                        self.assertIn(f'--create={str(create).lower()}', server_cmd)

    def test_cleanup_signals_and_detaches_only_owned_resources(self):
        for driver in DRIVERS:
            with self.subTest(driver=driver.__name__):
                srv = driver.Server(self.args(), 'work')
                proc = MagicMock(pid=12345)
                proc.poll.return_value = None
                proc.wait.return_value = 0
                srv.proc, srv.log, srv.loop = proc, MagicMock(), '/dev/loop7'
                with patch.object(driver.os, 'kill') as kill, \
                        patch.object(driver.subprocess, 'call') as call:
                    if driver.__name__ == 'graceful_stop_test':
                        srv.stop(1)
                    else:
                        srv.kill(driver.signal.SIGTERM)
                    srv.detach()
                    srv.detach()
                    kill.assert_called_once_with(12345, driver.signal.SIGTERM)
                    call.assert_called_once_with(['losetup', '-d', '/dev/loop7'])
                self.assertIsNone(srv.proc)
                self.assertIsNone(srv.loop)
                srv.log.close.assert_called_once()


if __name__ == '__main__':
    unittest.main()
