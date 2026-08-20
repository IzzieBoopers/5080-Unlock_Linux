#!/usr/bin/env python3
# SPDX-License-Identifier: MIT

from __future__ import annotations

import importlib.machinery
import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts" / "omen-wmi-fan-control"
LOADER = importlib.machinery.SourceFileLoader("omen_wmi_fan_control", str(SCRIPT))
SPEC = importlib.util.spec_from_loader(LOADER.name, LOADER)
assert SPEC is not None
fan_control = importlib.util.module_from_spec(SPEC)
sys.modules[LOADER.name] = fan_control
LOADER.exec_module(fan_control)


def make_controller(directory: Path, config_text: str | None = None):
    config = directory / "fan.conf"
    if config_text is not None:
        config.write_text(config_text, encoding="utf-8")
    args = SimpleNamespace(
        sysfs=directory / "sysfs",
        nvidia_smi="nvidia-smi",
        notifier="/bin/true",
        thermal_state=directory / "thermal.state",
        interval=4,
        idle_hold=180,
        config=config,
        dry_run=True,
        once=True,
    )
    return fan_control.Controller(args)


class ConfigTests(unittest.TestCase):
    def test_default_ai_curve(self):
        with tempfile.TemporaryDirectory() as tmp:
            settings = fan_control.parse_config(Path(tmp) / "missing.conf")
        self.assertEqual(settings.fan_curve[0], (32, 5))
        self.assertEqual(settings.fan_curve[-1], (82, 100))
        self.assertEqual(settings.unlock_disable_temp_c, 85)
        self.assertEqual(settings.ramp_down_step, 1)
        self.assertEqual(settings.ramp_down_dwell_s, 12)
        self.assertEqual(settings.temp_ema_alpha_percent, 50)

    def test_custom_curve_and_safety(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "fan.conf"
            path.write_text(
                "fan_curve=30:10,60:50,80:100\n"
                "warning_temp_c=78\n"
                "unlock_disable_temp_c=86\n"
                "critical_samples=3\n",
                encoding="utf-8",
            )
            settings = fan_control.parse_config(path)
        self.assertEqual(settings.fan_curve, ((30, 10), (60, 50), (80, 100)))
        self.assertEqual(settings.warning_temp_c, 78)
        self.assertEqual(settings.critical_samples, 3)

    def test_legacy_curve_settings_are_migrated(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "fan.conf"
            path.write_text(
                "curve_min_temp_c=30\n"
                "curve_min_fan_percent=10\n"
                "active_temp_c=40\n"
                "active_fan_floor=45\n"
                "target_temp_c=65\n"
                "critical_temp_c=80\n"
                "curve_max_fan_percent=100\n"
                "failsafe_temp_c=84\n",
                encoding="utf-8",
            )
            settings = fan_control.parse_config(path)
        self.assertEqual(
            settings.fan_curve, ((30, 10), (40, 45), (65, 45), (80, 100))
        )
        self.assertEqual(settings.unlock_disable_temp_c, 84)

    def test_rejects_non_increasing_curve(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "fan.conf"
            path.write_text("fan_curve=40:20,40:80\n", encoding="utf-8")
            with self.assertRaisesRegex(
                fan_control.FanControlError, "strictly increasing"
            ):
                fan_control.parse_config(path)

    def test_rejects_curve_that_slows_fans_as_temperature_rises(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "fan.conf"
            path.write_text(
                "fan_curve=30:50,70:40,82:100\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(
                fan_control.FanControlError, "must not decrease"
            ):
                fan_control.parse_config(path)

    def test_rejects_critical_below_warning(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "fan.conf"
            path.write_text(
                "warning_temp_c=85\nunlock_disable_temp_c=84\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(
                fan_control.FanControlError, "must be > warning_temp_c"
            ):
                fan_control.parse_config(path)

    def test_rejects_invalid_ema_alpha(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "fan.conf"
            path.write_text("temp_ema_alpha_percent=101\n", encoding="utf-8")
            with self.assertRaisesRegex(
                fan_control.FanControlError, "temp_ema_alpha_percent"
            ):
                fan_control.parse_config(path)


class PolicyTests(unittest.TestCase):
    def setUp(self):
        self.tempdir = tempfile.TemporaryDirectory()
        self.addCleanup(self.tempdir.cleanup)
        self.controller = make_controller(Path(self.tempdir.name))

    def _run_tick(
        self,
        temp: int,
        util: int,
        power: float,
        clock: int,
        now: float,
        state: dict[str, str] | None = None,
    ) -> int | None:
        if state is None:
            state = {
                "manual_supported": "1",
                "mode": "manual",
                "manual_speed": "0",
                "speed_min": "0",
                "speed_max": "100",
            }
        with (
            mock.patch.object(
                self.controller, "read_gpu", return_value=(temp, util, power, clock)
            ),
            mock.patch.object(self.controller, "read_fan_state", return_value=state),
            mock.patch.object(fan_control.time, "monotonic", return_value=now),
        ):
            self.controller.tick()
        return self.controller.current_percent

    def test_piecewise_curve_interpolation_and_active_floor(self):
        self.assertEqual(self.controller.curve_percent(45), 50)
        self.assertEqual(self.controller.curve_percent(73), 60)
        self.assertEqual(self.controller.curve_percent(82), 100)
        self.assertEqual(self.controller.curve_percent(32, active=False), 5)

    def test_warning_enforces_configured_fan_floor(self):
        self.assertEqual(self.controller.target_percent(80, True, 0), 90)
        self.assertEqual(self.controller.target_percent(82, True, 25), 100)

    def test_workload_detection(self):
        self.assertTrue(self.controller.workload_busy(30, 5.0, 100))
        self.assertTrue(self.controller.workload_busy(0, 45.0, 100))
        self.assertTrue(self.controller.workload_busy(0, 5.0, 1000))
        self.assertFalse(self.controller.workload_busy(0, 5.0, 180))

    def test_rate_limit(self):
        self.controller.settings = fan_control.replace(
            self.controller.settings, ramp_up_step=20, ramp_down_step=5
        )
        self.controller.current_percent = 40
        self.assertEqual(self.controller.rate_limit(100), 60)
        self.assertEqual(self.controller.rate_limit(20), 55)

    def test_soak_accumulates_and_decays(self):
        self.controller.last_tick_at = 0
        self.controller.update_soak_score(True, 120)
        self.assertEqual(self.controller.soak_score_s, 120)
        self.assertEqual(self.controller.soak_bias_percent(), 5)
        self.controller.last_workload_at = 0
        self.controller.update_soak_score(False, 720)
        self.assertLess(self.controller.soak_score_s, 120)

    def test_active_hold_tracks_work_duration(self):
        with mock.patch.object(fan_control.time, "monotonic", side_effect=[10, 20]):
            self.assertTrue(self.controller.should_be_active(50, 0, 180))
            self.assertTrue(self.controller.should_be_active(50, 0, 180))
        self.assertEqual(self.controller.active_hold_s, 30)

    def test_critical_requires_consecutive_samples(self):
        self.assertFalse(self.controller.update_critical_count(85))
        self.assertTrue(self.controller.update_critical_count(85))
        self.assertFalse(self.controller.update_critical_count(84))
        self.assertEqual(self.controller.critical_count, 0)

    def test_warning_is_rate_limited(self):
        self.assertTrue(self.controller.warning_due(100))
        self.controller.last_warning_at = 100
        self.assertFalse(self.controller.warning_due(200))
        self.assertTrue(self.controller.warning_due(401))

    def test_manual_write_skips_unchanged_firmware_state(self):
        state = {
            "manual_supported": "1",
            "mode": "manual",
            "manual_speed": "39",
            "speed_min": "19",
            "speed_max": "60",
        }
        with mock.patch.object(self.controller, "write_sysfs") as write:
            self.controller.apply_manual(50, state)
        write.assert_not_called()

    def test_critical_trip_orders_fans_before_boost(self):
        state = {"mode": "manual"}
        calls: list[tuple[str, str]] = []
        self.controller.critical_count = 2
        self.controller.dry_run = False
        self.controller.write_sysfs = lambda name, value: calls.append((name, value))
        self.controller.write_thermal_state = mock.Mock()
        self.controller.notify = mock.Mock()

        self.controller.trip_thermal_protection(86, state)

        self.assertEqual(calls[:2], [("fan_mode", "max"), ("boost", "0")])
        self.assertTrue(self.controller.thermal_latched)

    def test_soak_not_applied_until_earned_at_target(self):
        self.controller.soak_score_s = 120
        self.assertEqual(self.controller.soak_bias_percent(), 5)
        self.controller.update_soak_earned(True, 69)
        self.assertEqual(self.controller.applied_soak_bias(True), 0)
        self.controller.update_soak_earned(True, 70)
        self.assertEqual(self.controller.applied_soak_bias(True), 5)

    def test_soak_ratchet_holds_through_dips_until_score_decays(self):
        self.controller.soak_score_s = 120
        self.controller.update_soak_earned(True, 70)
        self.assertEqual(self.controller.applied_soak_bias(True), 5)
        self.controller.update_soak_earned(True, 69)
        self.assertEqual(self.controller.applied_soak_bias(True), 5)
        self.controller.update_soak_earned(True, 59)
        self.assertEqual(self.controller.applied_soak_bias(True), 5)
        self.controller.soak_score_s = 0
        self.controller.update_soak_earned(True, 59)
        self.assertEqual(self.controller.applied_soak_bias(True), 0)
        self.controller.soak_score_s = 120
        self.controller.update_soak_earned(False, 70)
        self.assertEqual(self.controller.applied_soak_bias(False), 0)

    def test_ema_does_not_immediately_track_integer_chatter(self):
        self.controller.settings = fan_control.replace(
            self.controller.settings, temp_ema_alpha_percent=50
        )
        self.assertEqual(self.controller.update_filtered_temp(71), 71.0)
        self.assertEqual(self.controller.update_filtered_temp(72), 71.5)
        self.assertEqual(self.controller.update_filtered_temp(71), 71.25)

    def test_busy_hold_ignores_temperature_crater(self):
        self.controller.current_percent = 62
        self.controller.last_temp_rise_at = 0
        self.assertEqual(
            self.controller.command_percent(50, True, now=100, emergency=False),
            62,
        )

    def test_idle_downshift_waits_for_dwell_then_ramps_slowly(self):
        self.controller.settings = fan_control.replace(
            self.controller.settings, ramp_down_step=1, ramp_down_dwell_s=12
        )
        self.controller.current_percent = 56
        self.controller.last_temp_rise_at = 0
        self.assertEqual(
            self.controller.command_percent(50, False, now=11, emergency=False),
            56,
        )
        self.assertEqual(
            self.controller.command_percent(50, False, now=12, emergency=False),
            55,
        )

    def test_warning_bypasses_smoothing_and_applies_floor_immediately(self):
        self.controller.current_percent = 50
        self.controller.last_temp_rise_at = 100
        self.assertEqual(
            self.controller.command_percent(90, True, now=101, emergency=True),
            90,
        )
        self.assertEqual(
            self.controller.target_percent(70.0, True, 0, raw_temp=80),
            90,
        )

    def test_hunting_sequence_holds_with_earned_soak(self):
        self.controller.settings = fan_control.replace(
            self.controller.settings, temp_ema_alpha_percent=0
        )
        self.controller.soak_score_s = 120
        self.controller.update_soak_earned(True, 70)
        commands = []
        for temp in (70, 69, 70, 69):
            bias = self.controller.applied_soak_bias(True)
            self.controller.update_soak_earned(True, temp)
            desired = self.controller.target_percent(temp, True, bias)
            commands.append(
                self.controller.command_percent(
                    desired, True, now=100, emergency=False
                )
            )
        self.assertEqual(commands, [55, 55, 55, 55])

    def test_busy_crater_sequence_holds_fan_command(self):
        self.controller.settings = fan_control.replace(
            self.controller.settings, temp_ema_alpha_percent=0
        )
        self.controller.soak_score_s = 120
        self.controller.update_soak_earned(True, 72)
        bias = self.controller.applied_soak_bias(True)
        start = self.controller.target_percent(72, True, bias)
        self.controller.current_percent = start
        crater = self.controller.target_percent(59, True, bias)
        held = self.controller.command_percent(
            crater, True, now=8, emergency=False
        )
        bounce = self.controller.target_percent(70, True, bias)
        held_after_bounce = self.controller.command_percent(
            bounce, True, now=12, emergency=False
        )
        self.assertGreater(start, crater)
        self.assertEqual(held, start)
        self.assertEqual(held_after_bounce, start)

    def test_tick_holds_busy_crater(self):
        self.controller.settings = fan_control.replace(
            self.controller.settings, temp_ema_alpha_percent=0
        )
        self.controller.soak_score_s = 120
        self.controller.last_tick_at = 0
        self.controller.last_workload_at = 0
        first = self._run_tick(72, 50, 100.0, 1500, 4)
        crater = self._run_tick(59, 50, 100.0, 1500, 8)
        bounce = self._run_tick(70, 50, 100.0, 1500, 12)
        self.assertEqual(first, crater)
        self.assertEqual(crater, bounce)
        self.assertGreater(first, 50)

    def test_tick_applies_warning_floor_immediately(self):
        self.controller.current_percent = 50
        self.controller.last_temp_rise_at = 1000
        warning = self._run_tick(80, 50, 100.0, 1500, 1004)
        self.assertEqual(warning, 90)


if __name__ == "__main__":
    unittest.main()
