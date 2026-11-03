"""Deadline behavior without a Windows process or game data."""
import pathlib
import sys
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / 'tools' / 're'))
import observe_battle_timing as timing


class SamplingTests(unittest.TestCase):
    def test_slow_snapshot_or_report_crosses_deadline(self):
        observer = Mock()
        observer.snapshot.return_value = {'frame_counter': 1}
        with patch.object(timing.time, 'monotonic', side_effect=[0, 0, 1.01]), patch.object(timing.time, 'sleep') as sleep:
            self.assertEqual(len(list(timing.sample_rows(observer, 1, .5))), 1)
            sleep.assert_not_called()

    def test_last_wait_is_bounded_by_remaining_time(self):
        observer = Mock()
        observer.snapshot.return_value = {}
        with patch.object(timing.time, 'monotonic', side_effect=[0, 0, .8, 1, 1]), patch.object(timing.time, 'sleep') as sleep:
            rows = list(timing.sample_rows(observer, 1, .5))
            self.assertEqual([r['wall_seconds'] for r in rows], [0, 1])
            self.assertAlmostEqual(sleep.call_args.args[0], .2)
            self.assertEqual(sleep.call_count, 1)

    def test_zero_duration_still_takes_one_sample(self):
        observer = Mock()
        observer.snapshot.return_value = {}
        with patch.object(timing.time, 'monotonic', return_value=5), patch.object(timing.time, 'sleep') as sleep:
            self.assertEqual(list(timing.sample_rows(observer, 0, 1)), [{'wall_seconds': 0}])
            sleep.assert_not_called()

    def test_invalid_limits_fail_before_reading_process(self):
        observer = Mock()
        for seconds, interval in [(-1, 1), (1, 0), (1, -1), (float('nan'), 1), (float('inf'), 1), (1, float('nan')), (1, float('inf'))]:
            with self.subTest(seconds=seconds, interval=interval), self.assertRaises(ValueError):
                list(timing.sample_rows(observer, seconds, interval))
        observer.snapshot.assert_not_called()


if __name__ == '__main__':
    unittest.main()
