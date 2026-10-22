#!/usr/bin/env python3
"""Build real KM bodies with injected defects in a disposable Unix source copy.

Requires CMake and a C++20 toolchain on PATH. No installed driver or Windows API
is used. This is intentionally opt-in, not part of the fast CTest suite.
"""
import pathlib
import shutil
import subprocess
import tempfile

SOURCE = pathlib.Path(__file__).resolve().parents[2]


def omit_d0_retirement(text):
    start = text.index("void power_down_controllers(")
    end = text.index("NTSTATUS evt_d0_entry", start)
    changed = text[start:end].replace(
        "  stop_keyboard_mouse_admission_locked(context->keyboard_mouse);", ""
    ).replace("  delete_stopped_keyboard_mouse(context, true);", "")
    return text[:start] + changed + text[end:]


def allow_oversize_requests(text):
    start = text.index(
        "template<class request_t>\n[[nodiscard]] NTSTATUS retrieve_keyboard_mouse_request"
    )
    end = text.index("template<class request_t>", start + 10)
    changed = text[start:end].replace(
        "input_length != sizeof(request_t)", "input_length < sizeof(request_t)"
    ).replace("length != sizeof(request_t)", "length < sizeof(request_t)")
    return text[:start] + changed + text[end:]


NATIVE = "driver/src/keyboard_mouse_native.inc"
CASES = [
    ("remove power admission", NATIVE,
     lambda t: t.replace(" && context->powered", "").replace(
         " || !context->powered", "").replace("context->powered && ", ""),
     "--power-only"),
    ("omit KM D0 retirement", "driver/src/driver.cpp", omit_d0_retirement,
     "--retirement-only"),
    ("accept stale enumeration", NATIVE,
     lambda t: t.replace("slot.generation != generation || child_present", "child_present"),
     "--power-only"),
    ("allow oversized requests", NATIVE, allow_oversize_requests, None),
    ("expose transport busy as retryable", "client/client.cpp",
     lambda t: t.replace("return status == ERROR_BUSY ? ERROR_NOT_READY : status;",
                         "return status;"), None),
    ("reset retained storage before deletion", NATIVE,
     lambda t: t.replace("  if (handle != nullptr) VhfDelete(handle, TRUE);",
                         "  slot.core.reset_after_device_deleted();\n"
                         "  if (handle != nullptr) VhfDelete(handle, TRUE);"), None),
]


def main():
    with tempfile.TemporaryDirectory(prefix="lvg-km-mutations-") as directory:
        root = pathlib.Path(directory)
        for folder in ("driver", "client", "include", "linux"):
            shutil.copytree(SOURCE / folder, root / folder)

        def run(command, timeout=120):
            return subprocess.run(command, cwd=root, text=True, capture_output=True,
                                  timeout=timeout, check=False)

        def require_success(result, stage):
            if result.returncode != 0:
                raise RuntimeError(f"{stage} failed:\n{result.stdout}\n{result.stderr}")

        require_success(run(["cmake", "-S", "driver/tests", "-B", "build",
                             "-DCMAKE_BUILD_TYPE=Debug"]), "configure")
        build = ["cmake", "--build", "build", "--target",
                 "test_keyboard_mouse_transport", "-j2"]
        binary = str(root / "build/test_keyboard_mouse_transport")
        require_success(run(build), "unmutated build")
        require_success(run([binary], timeout=30), "unmutated baseline")
        original = {path: (root / path).read_text() for _, path, _, _ in CASES}
        for name, path, mutate, selector in CASES:
            for source, contents in original.items():
                (root / source).write_text(contents)
            mutated = mutate(original[path])
            if mutated == original[path]:
                raise RuntimeError(f"Mutation no longer matches source: {name}")
            (root / path).write_text(mutated)
            require_success(run(build), f"{name} build")
            result = run([binary] + ([selector] if selector else []), timeout=30)
            # A compile error, crash, or watchdog timeout is not a behavioral
            # assertion rejection. The fixture must complete and return 1.
            if result.returncode != 1 or "failures" not in result.stdout:
                raise RuntimeError(f"{name}: expected assertion failure, got "
                                   f"{result.returncode}\n{result.stdout}\n{result.stderr}")
            print(f"Rejected {name}: {result.stdout.strip().splitlines()[-1]}", flush=True)
        print("All six compiled mutations were rejected by behavioral assertions.")


if __name__ == "__main__":
    main()
