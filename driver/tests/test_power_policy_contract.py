#!/usr/bin/env python3

import pathlib
import sys
import unittest


def function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    opening_brace = source.index("{", start)
    depth = 0
    for offset, character in enumerate(source[opening_brace:], opening_brace):
        if character == "{":
            depth += 1
        elif character == "}":
            depth -= 1
            if depth == 0:
                return source[opening_brace : offset + 1]
    raise AssertionError(f"unterminated function: {signature}")


class PowerPolicyContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.source = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8")

    def test_filter_role_precedes_device_creation(self) -> None:
        body = function_body(self.source, "NTSTATUS evt_device_add(")
        filter_call = "WdfFdoInitSetFilter(device_init);"
        self.assertEqual(body.count(filter_call), 1)
        self.assertLess(body.index(filter_call), body.index("WdfDeviceCreate("))

    def test_upper_filter_queue_is_not_power_managed(self) -> None:
        body = function_body(self.source, "NTSTATUS evt_device_add(")
        queue_start = body.index("WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(")
        queue_end = body.index("WdfIoQueueCreate(", queue_start)
        queue_setup = body[queue_start:queue_end]

        self.assertIn("queue_config.PowerManaged = WdfFalse;", queue_setup)
        self.assertNotIn("queue_config.PowerManaged = WdfTrue;", body)
        self.assertIn("queue_config.EvtIoDeviceControl = evt_io_device_control;", queue_setup)

    def test_non_power_managed_queue_keeps_manual_lifecycle_gates(self) -> None:
        add_body = function_body(self.source, "NTSTATUS evt_device_add(")
        prepare_body = function_body(self.source, "NTSTATUS evt_prepare_hardware(")
        release_body = function_body(self.source, "NTSTATUS evt_release_hardware(")

        self.assertIn(
            "pnp_callbacks.EvtDevicePrepareHardware = evt_prepare_hardware;",
            add_body,
        )
        self.assertIn(
            "pnp_callbacks.EvtDeviceReleaseHardware = evt_release_hardware;",
            add_body,
        )
        self.assertIn("context->stopping = false;", prepare_body)
        self.assertIn("ensure_vhf_target_open(context)", prepare_body)
        self.assertIn("stop_owned_controllers(context, false);", release_body)
        self.assertIn("context->vhf_target_open = false;", release_body)
        self.assertIn("WdfIoTargetClose(target);", release_body)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
