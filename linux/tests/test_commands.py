# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

from __future__ import annotations

import asyncio
import json
import logging
import os
import signal
import stat
import tempfile
import sys
import time

import pytest

from musegadget import commands, config
from musegadget.commands import InvalidCommand
from musegadget.executor import COMMAND_SPECS, Account, Executor

SPEC = {
    "description": "Switch the pump.",
    "exec": ["/usr/local/bin/pump", "--quiet"],
    "required": {"on": {"type": "boolean", "description": "true for on."}},
    "optional": {"minutes": {"type": "integer", "description": "Run time."}},
    "timeout_ms": 10000,
}


def write(directory, name: str, data, mode: int = 0o644):
    path = directory / f"{name}.json"
    path.write_text(data if isinstance(data, str) else json.dumps(data))
    path.chmod(mode)
    return path


def script(tmp_path, body: str) -> list:
    """``exec`` for a Python script, so the tests need nothing but Python."""
    path = tmp_path / "cmd.py"
    path.write_text(f"import json, sys, time\n{body}\n")
    return [sys.executable, str(path)]


@pytest.fixture
def account(tmp_path):
    current = Account.current()
    return Account(current.name, current.uid, current.gid, str(tmp_path))


def drop_in(argv, **overrides):
    return commands.parse("pump.set", {**SPEC, "exec": argv, **overrides})


# -- Parsing ------------------------------------------------------------------

def test_parse_registers_the_spec_without_exec():
    command = commands.parse("pump.set", SPEC)
    assert command.argv == ("/usr/local/bin/pump", "--quiet")
    assert command.spec == {
        "description": "Switch the pump.",
        "required": SPEC["required"],
        "optional": SPEC["optional"],
        "timeout_ms": 10000 + commands.TIMEOUT_GRACE_MS,
    }


def test_parse_defaults():
    command = commands.parse("pump.set", {"description": "d", "exec": ["/bin/true"]})
    assert (command.required, command.optional) == ({}, {})
    assert command.timeout_ms == commands.DEFAULT_TIMEOUT_MS


@pytest.mark.parametrize("name", [
    "pump", "Pump.set", "pump..set", "pump.set.", "1pump.set", "pump-set.x", "a." + "b" * 64,
])
def test_parse_rejects_bad_names(name):
    with pytest.raises(InvalidCommand, match="command name"):
        commands.parse(name, SPEC)


@pytest.mark.parametrize("name", ["system.reboot", "file.delete", "device.health", "link.x"])
def test_parse_rejects_reserved_names(name):
    with pytest.raises(InvalidCommand, match="reserved"):
        commands.parse(name, SPEC)


def test_builtin_names_are_all_reserved():
    assert all(name.startswith(commands.RESERVED_PREFIXES) for name in COMMAND_SPECS)


@pytest.mark.parametrize("change, message", [
    ({"description": ""}, "description"),
    ({"description": None}, "description"),
    ({"exec": "/usr/local/bin/pump"}, "exec"),
    ({"exec": []}, "exec"),
    ({"exec": ["/bin/x", 3]}, "exec"),
    ({"exec": ["pump"]}, "absolute"),
    ({"required": []}, "required"),
    ({"required": {"on": {"type": "bool", "description": "d"}}}, "required.on"),
    ({"required": {"on": {"type": "boolean"}}}, "required.on"),
    ({"optional": {"x": {"type": "string", "default": 1}}}, "optional.x"),
    ({"optional": {"on": {"type": "boolean", "description": "d"}}}, "both required and optional"),
    ({"timeout_ms": 0}, "timeout_ms"),
    ({"timeout_ms": 600_001}, "timeout_ms"),
    ({"timeout_ms": "10"}, "timeout_ms"),
    ({"timeout_ms": True}, "timeout_ms"),
    ({"timeout": 10}, "unknown keys: timeout"),
])
def test_parse_rejects_bad_specs(change, message):
    with pytest.raises(InvalidCommand, match=message):
        commands.parse("pump.set", {**SPEC, **change})


def test_parse_rejects_a_non_object():
    with pytest.raises(InvalidCommand, match="JSON object"):
        commands.parse("pump.set", ["not", "an", "object"])


# -- Parameters -----------------------------------------------------------------

def test_check_params_keeps_only_declared_parameters():
    command = commands.parse("pump.set", SPEC)
    assert command.check_params({"on": True, "minutes": 5, "extra": 1}) == {"on": True, "minutes": 5}


@pytest.mark.parametrize("params, message", [
    ({}, "on is required"),
    ({"on": "yes"}, "on must be a boolean"),
    ({"on": True, "minutes": 1.5}, "minutes must be an integer"),
    ({"on": True, "minutes": False}, "minutes must be an integer"),
])
def test_check_params_rejects(params, message):
    with pytest.raises(InvalidCommand, match=message):
        commands.parse("pump.set", SPEC).check_params(params)


def test_number_accepts_integers_but_not_booleans():
    command = commands.parse("x.y", {"description": "d", "exec": ["/bin/true"],
                                     "required": {"n": {"type": "number", "description": "n"}}})
    assert command.check_params({"n": 2}) == {"n": 2}
    assert command.check_params({"n": 2.5}) == {"n": 2.5}
    with pytest.raises(InvalidCommand):
        command.check_params({"n": True})


# -- Loading the directory --------------------------------------------------------

def test_load_a_missing_directory(tmp_path):
    assert commands.load(tmp_path / "nope") == []


def test_load_skips_bad_files_and_keeps_good_ones(tmp_path, caplog):
    write(tmp_path, "pump.set", SPEC)
    write(tmp_path, "aaa.broken", "{not json")
    write(tmp_path, "system.reboot", SPEC)
    write(tmp_path, "lamp.set", {**SPEC, "exec": ["lamp"]})
    (tmp_path / "notes.txt").write_text("ignored")
    with caplog.at_level(logging.WARNING):
        loaded = commands.load(tmp_path)
    assert [c.name for c in loaded] == ["pump.set"]
    warnings = "\n".join(r.getMessage() for r in caplog.records)
    assert "aaa.broken.json: not valid JSON" in warnings
    assert "system.reboot.json: names starting with" in warnings
    assert "lamp.set.json: exec must start with" in warnings


def test_load_skips_deeply_nested_json(tmp_path, caplog):
    write(tmp_path, "pump.set", "[" * 60000)
    assert commands.load(tmp_path) == []
    assert "pump.set.json: not valid JSON" in caplog.text


def test_load_skips_symlinks_and_special_files(tmp_path, caplog):
    target = write(tmp_path, "real.target", SPEC)
    (tmp_path / "pump.set.json").symlink_to(target)
    os.mkfifo(tmp_path / "lamp.set.json")
    names = [c.name for c in commands.load(tmp_path)]
    assert names == ["real.target"]
    assert "pump.set.json: can't read it" in caplog.text
    assert "lamp.set.json: not a regular file" in caplog.text


def test_load_skips_oversize_files(tmp_path, caplog):
    write(tmp_path, "pump.set", {**SPEC, "description": "x" * commands.MAX_FILE_BYTES})
    assert commands.load(tmp_path) == []
    assert "larger than" in caplog.text


def test_load_skips_files_root_does_not_own(tmp_path, monkeypatch, caplog):
    # The service runs as root on a device; the files must be root's alone.
    write(tmp_path, "pump.set", SPEC)
    if os.geteuid() == 0:
        os.chown(tmp_path / "pump.set.json", 12345, 12345)
    monkeypatch.setattr(commands.os, "geteuid", lambda: 0)
    assert commands.load(tmp_path) == []
    assert "owned by root" in caplog.text


@pytest.mark.skipif(os.geteuid() != 0, reason="needs root to own the files")
def test_load_as_root_checks_the_mode(tmp_path, caplog):
    write(tmp_path, "pump.set", SPEC)
    write(tmp_path, "lamp.set", SPEC, mode=0o664)
    assert [c.name for c in commands.load(tmp_path)] == ["pump.set"]
    assert "lamp.set.json: must be owned by root" in caplog.text


def test_commands_dir(monkeypatch, tmp_path):
    monkeypatch.delenv(config.COMMANDS_DIR_ENV, raising=False)
    assert str(config.commands_dir()) == "/etc/musegadget/commands.d"
    monkeypatch.setenv(config.COMMANDS_DIR_ENV, str(tmp_path))
    assert config.commands_dir() == tmp_path


# -- Running them -------------------------------------------------------------------

def test_specs_add_drop_ins_after_the_builtins(account):
    executor = Executor(account, [commands.parse("pump.set", SPEC)])
    specs = executor.specs()
    assert list(specs) == [*COMMAND_SPECS, "pump.set"]
    assert "exec" not in specs["pump.set"]


def test_specs_are_unchanged_without_drop_ins(account):
    assert Executor(account).specs() == COMMAND_SPECS


def test_drop_in_gets_its_params_on_stdin_and_returns_json(account, tmp_path):
    argv = script(tmp_path, (
        "params = json.load(sys.stdin)\n"
        "print(json.dumps({'got': params, 'argv': sys.argv[1:]}))"
    )) + ["--flag"]
    result = Executor(account, [drop_in(argv)]).run("pump.set", {"on": True, "junk": 1})
    assert result == {"ok": True, "payload": {"got": {"on": True}, "argv": ["--flag"]}}


def test_drop_in_runs_in_the_account_home_with_the_safe_environment(account, tmp_path, monkeypatch):
    argv = script(tmp_path, (
        "import os\n"
        "print(json.dumps({'cwd': os.getcwd(), 'home': os.environ['HOME'],"
        " 'secret': os.environ.get('MUSE_TEST_SECRET')}))"
    ))
    monkeypatch.setenv("MUSE_TEST_SECRET", "leak")
    payload = Executor(account, [drop_in(argv)]).run("pump.set", {"on": False})["payload"]
    assert os.path.realpath(payload["cwd"]) == os.path.realpath(tmp_path)
    assert payload["home"] == str(tmp_path)
    assert payload["secret"] is None


def test_drop_in_plain_text_output(account, tmp_path):
    argv = script(tmp_path, "print('pump on')")
    result = Executor(account, [drop_in(argv)]).run("pump.set", {"on": True})
    assert result == {"ok": True, "payload": {"output": "pump on\n"}}


def test_drop_in_json_that_is_not_an_object_is_text(account, tmp_path):
    argv = script(tmp_path, "print('[1, 2]')")
    result = Executor(account, [drop_in(argv)]).run("pump.set", {"on": True})
    assert result["payload"] == {"output": "[1, 2]\n"}


def test_drop_in_large_output_is_truncated(account, tmp_path):
    argv = script(tmp_path, "sys.stdout.write('x' * 200000)")
    payload = Executor(account, [drop_in(argv)]).run("pump.set", {"on": True})["payload"]
    assert payload["truncated"] is True
    assert len(payload["output"]) == 96 * 1024


def test_drop_in_failure_reports_the_last_stderr_line(account, tmp_path):
    argv = script(tmp_path, (
        "print('first', file=sys.stderr)\n"
        "print('relay busy', file=sys.stderr)\n"
        "sys.exit(3)"
    ))
    result = Executor(account, [drop_in(argv)]).run("pump.set", {"on": True})
    assert result == {"ok": False, "error": "pump.set exited with 3: relay busy"}


def test_drop_in_failure_without_stderr(account, tmp_path):
    argv = script(tmp_path, "sys.exit(2)")
    result = Executor(account, [drop_in(argv)]).run("pump.set", {"on": True})
    assert result == {"ok": False, "error": "pump.set exited with 2"}


def test_drop_in_bad_params_never_start_the_program(account, tmp_path):
    marker = tmp_path / "ran"
    argv = script(tmp_path, f"open({str(marker)!r}, 'w')")
    result = Executor(account, [drop_in(argv)]).run("pump.set", {"on": "yes"})
    assert result == {"ok": False, "error": "on must be a boolean"}
    assert not marker.exists()


def test_drop_in_times_out_and_kills_the_process_group(account, tmp_path):
    argv = script(tmp_path, (
        "import subprocess\n"
        "subprocess.Popen(['sleep', '30'])\n"
        "time.sleep(30)"
    ))
    started = time.monotonic()
    result = Executor(account, [drop_in(argv, timeout_ms=300)]).run("pump.set", {"on": True})
    assert result == {"ok": False, "error": "pump.set timed out after 0.3s"}
    assert time.monotonic() - started < 5


def test_drop_in_that_cannot_start(account, tmp_path):
    missing = tmp_path / "missing"
    result = Executor(account, [drop_in([str(missing)])]).run("pump.set", {"on": True})
    assert not result["ok"]
    assert result["error"].startswith(f"could not start {missing}:")


def test_drop_in_cannot_replace_a_builtin(account):
    # parse() reserves the names; Executor also refuses them, whatever their source.
    fake = commands.DropInCommand("system.run", "x", ("/bin/false",), {}, {}, 1000)
    executor = Executor(account, [fake])
    assert executor.drop_ins == {}
    assert executor.specs()["system.run"] == COMMAND_SPECS["system.run"]


def test_unknown_commands_are_still_unsupported(account):
    result = Executor(account).run("pump.set", {})
    assert result == {"ok": False, "error": "unsupported command: pump.set"}


# -- Review regressions --------------------------------------------------------

@pytest.fixture
def watchdog():
    """Bound regressions which intentionally put backpressure on child pipes."""
    def expired(signum, frame):
        raise AssertionError("child execution exceeded the outer 10s watchdog")

    previous = signal.signal(signal.SIGALRM, expired)
    signal.setitimer(signal.ITIMER_REAL, 10)
    try:
        yield
    finally:
        signal.setitimer(signal.ITIMER_REAL, 0)
        signal.signal(signal.SIGALRM, previous)


@pytest.mark.parametrize("value", [["string"], {"type": "string"}, None, 1, 1.5, True])
@pytest.mark.parametrize("key", ["required", "optional"])
def test_bad_parameter_types_do_not_stop_loading(tmp_path, value, key, account, caplog):
    write(tmp_path, "aaa.bad", {**SPEC, key: {"x": {"type": value, "description": "d"}}})
    write(tmp_path, "pump.set", SPEC)
    loaded = commands.load(tmp_path)
    assert [c.name for c in loaded] == ["pump.set"]
    assert "aaa.bad.json" in caplog.text
    assert "pump.set" in Executor(account, loaded).specs()


@pytest.mark.parametrize("data", [None, 42, True, [], "{broken"])
def test_bad_json_shapes_do_not_stop_loading(tmp_path, data):
    write(tmp_path, "aaa.bad", data)
    write(tmp_path, "pump.set", SPEC)
    assert [c.name for c in commands.load(tmp_path)] == ["pump.set"]


def test_one_unreadable_file_does_not_stop_loading(tmp_path, monkeypatch, caplog):
    bad = write(tmp_path, "aaa.bad", SPEC)
    write(tmp_path, "pump.set", SPEC)
    original = commands.os.open

    def open_file(path, flags, **kwargs):
        if path == bad.name:
            raise PermissionError("secret contents must not be logged")
        return original(path, flags, **kwargs)

    monkeypatch.setattr(commands.os, "open", open_file)
    assert [c.name for c in commands.load(tmp_path)] == ["pump.set"]
    assert "PermissionError" in caplog.text
    assert "secret contents" not in caplog.text


def test_unexpected_failure_is_isolated_without_logging_its_contents(tmp_path, monkeypatch, caplog):
    write(tmp_path, "aaa.bad", SPEC)
    write(tmp_path, "pump.set", SPEC)
    original = commands._load_file

    def load_file(path, **kwargs):
        if path.stem == "aaa.bad":
            raise RuntimeError("token-value command-arguments output configuration")
        return original(path, **kwargs)

    monkeypatch.setattr(commands, "_load_file", load_file)
    assert [c.name for c in commands.load(tmp_path)] == ["pump.set"]
    assert "RuntimeError" in caplog.text
    assert "token-value" not in caplog.text


@pytest.mark.parametrize("failure", [KeyboardInterrupt, SystemExit, asyncio.CancelledError])
def test_loader_does_not_swallow_process_cancellation(tmp_path, monkeypatch, failure):
    write(tmp_path, "pump.set", SPEC)

    def interrupted(*args, **kwargs):
        raise failure()

    monkeypatch.setattr(commands, "_load_file", interrupted)
    with pytest.raises(failure):
        commands.load(tmp_path)


def root_directory_seam(monkeypatch, *, uid=0, mode=0o755):
    """Simulated ownership only; this does not verify a real privilege drop."""
    original = commands.os.fstat

    def fstat(fd):
        result = original(fd)
        values = list(result)
        values[4] = uid if stat.S_ISDIR(result.st_mode) else 0
        if stat.S_ISDIR(result.st_mode):
            values[0] = stat.S_IFDIR | mode
        return os.stat_result(values)

    monkeypatch.setattr(commands.os, "geteuid", lambda: 0)
    monkeypatch.setattr(commands.os, "fstat", fstat)


@pytest.mark.parametrize("uid, mode, safe", [
    (0, 0o700, True), (0, 0o755, True), (12345, 0o755, False),
    (0, 0o775, False), (0, 0o777, False), (0, 0o757, False),
])
def test_directory_trust_boundary(tmp_path, monkeypatch, caplog, uid, mode, safe):
    write(tmp_path, "pump.set", SPEC)
    root_directory_seam(monkeypatch, uid=uid, mode=mode)
    assert [c.name for c in commands.load(tmp_path)] == (["pump.set"] if safe else [])
    if not safe:
        assert "skipping commands directory" in caplog.text
        assert "owned by root" in caplog.text


def test_non_root_development_can_use_its_own_writable_directory(tmp_path, monkeypatch):
    write(tmp_path, "pump.set", SPEC, mode=0o666)
    tmp_path.chmod(0o777)
    monkeypatch.setattr(commands.os, "geteuid", lambda: 12345)
    assert [c.name for c in commands.load(tmp_path)] == ["pump.set"]


@pytest.mark.parametrize("kind", ["file", "symlink"])
def test_load_refuses_non_directory_and_directory_symlink(tmp_path, kind, caplog):
    path = tmp_path / "commands.d"
    if kind == "file":
        path.write_text("not a directory")
    else:
        actual = tmp_path / "actual"
        actual.mkdir()
        write(actual, "pump.set", SPEC)
        path.symlink_to(actual, target_is_directory=True)
    assert commands.load(path) == []
    assert "can't read commands" in caplog.text


def test_drop_in_large_unread_stdin_obeys_deadline(account, tmp_path, watchdog):
    argv = script(tmp_path, "time.sleep(30)")
    command = drop_in(argv, required={"blob": {"type": "string", "description": "d"}},
                      optional={}, timeout_ms=300)
    started = time.monotonic()
    result = Executor(account, [command]).run("pump.set", {"blob": "x" * 1_000_000})
    assert result == {"ok": False, "error": "pump.set timed out after 0.3s"}
    assert time.monotonic() - started < 3


def test_drop_in_drains_both_outputs_while_writing_large_stdin(account, tmp_path, watchdog):
    argv = script(tmp_path, (
        "sys.stdout.write('o' * 200000); sys.stdout.flush()\n"
        "sys.stderr.write('e' * 200000); sys.stderr.flush()\n"
        "params = json.load(sys.stdin)\n"
        "assert len(params['blob']) == 1_000_000"
    ))
    command = drop_in(argv, required={"blob": {"type": "string", "description": "d"}},
                      optional={}, timeout_ms=3000)
    result = Executor(account, [command]).run("pump.set", {"blob": "x" * 1_000_000})
    assert result["ok"], result
    assert result["payload"] == {"output": "o" * (96 * 1024), "truncated": True}


def test_run_child_handles_closed_stdin_before_request_is_written(account, tmp_path, watchdog):
    argv = script(tmp_path, "sys.stdin.close(); print('done')")
    stdout, stderr, code, timed_out = Executor(account)._run_child(
        argv, str(tmp_path), 2, stdin=b"x" * 1_000_000)
    assert (stdout, stderr, code, timed_out) == (b"done\n", b"", 0, False)


def test_run_child_bounds_continuous_output_and_timeout(account, tmp_path, watchdog):
    argv = script(tmp_path, (
        "import os\n"
        "while True:\n"
        "    os.write(1, b'o' * 65536)\n"
        "    os.write(2, b'e' * 65536)"
    ))
    started = time.monotonic()
    out, err, code, timed_out = Executor(account)._run_child(argv, str(tmp_path), 0.3)
    assert timed_out and code == -signal.SIGKILL
    assert len(out) == len(err) == 96 * 1024 + 1
    assert time.monotonic() - started < 3


def test_drop_in_returns_when_parent_exits_with_descendant_holding_pipes(account, tmp_path, watchdog):
    pidfile = tmp_path / "descendant.pid"
    argv = script(tmp_path, (
        "import subprocess\n"
        "child = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(30)'])\n"
        f"open({str(pidfile)!r}, 'w').write(str(child.pid))\n"
        "print('{\"done\": true}')"
    ))
    try:
        started = time.monotonic()
        result = Executor(account, [drop_in(argv, timeout_ms=1500)]).run("pump.set", {"on": True})
        assert result == {"ok": True, "payload": {"done": True}}
        assert time.monotonic() - started < 1
    finally:
        if pidfile.exists():
            try:
                os.kill(int(pidfile.read_text()), signal.SIGKILL)
            except ProcessLookupError:
                pass


REGISTER_METADATA = {"node_id": "homelink-abcdef", "display_name": "pi", "version": "0.1.0"}


def registration_size(specs, metadata=REGISTER_METADATA):
    from musegadget.link_client import DeviceDescription, REGISTER_ID_BYTES, encode_message
    device = DeviceDescription(commands=specs, **metadata)
    return len(encode_message(device.register_message("0" * REGISTER_ID_BYTES)))


def described(name, description):
    return commands.parse(name, {"description": description, "exec": [sys.executable, "-c", "pass"]})


@pytest.mark.parametrize("offset", [-1, 0, 1])
def test_registration_below_at_and_above_boundary(account, offset, caplog):
    from musegadget.link_client import MAX_CONTROL_MESSAGE_BYTES
    # Every definition would fit on disk; together they fill the message.
    first = [described(f"custom.a{i}", "x" * 60000) for i in range(4)]
    last = described("custom.z", "x")
    trial = {**COMMAND_SPECS, **{c.name: c.spec for c in [*first, last]}}
    remaining = MAX_CONTROL_MESSAGE_BYTES - registration_size(trial)
    last = described("custom.z", "x" * (1 + remaining + offset))
    assert len(json.dumps({"description": last.description, "exec": ["/bin/true"]}).encode()) < commands.MAX_FILE_BYTES
    executor = Executor(account, [last, *reversed(first)])
    specs = executor.specs(**REGISTER_METADATA)
    assert set(COMMAND_SPECS) <= specs.keys()
    if offset <= 0:
        assert "custom.z" in specs
        assert registration_size(specs) == MAX_CONTROL_MESSAGE_BYTES + offset
        assert executor.run("custom.z", {})["ok"]
    else:
        assert "custom.z" not in specs
        assert executor.run("custom.z", {}) == {"ok": False, "error": "unsupported command: custom.z"}
        assert "skipping command custom.z" in caplog.text
    assert registration_size(specs) <= MAX_CONTROL_MESSAGE_BYTES


def test_registration_many_small_definitions_is_deterministic(account):
    from musegadget.link_client import MAX_CONTROL_MESSAGE_BYTES
    definitions = [described(f"custom.a{i:04d}", "x" * 200) for i in range(1200)]
    a = Executor(account, definitions)
    b = Executor(account, reversed(definitions))
    specs = a.specs(**REGISTER_METADATA)
    assert specs == b.specs(**REGISTER_METADATA)
    assert 0 < len(a.drop_ins) < len(definitions)
    assert set(specs) == set(COMMAND_SPECS) | set(a.drop_ins)
    assert registration_size(specs) <= MAX_CONTROL_MESSAGE_BYTES


def test_registration_accounts_for_escaping_and_metadata(account, tmp_path, caplog):
    from musegadget.link_client import MAX_CONTROL_MESSAGE_BYTES
    # This fits the file limit with literal UTF-8, but not the outgoing ensure_ascii JSON.
    for name in ["aaa.large", "bbb.large", "ccc.large"]:
        write(tmp_path, name, json.dumps({"description": "界" * 21000,
                                         "exec": ["/bin/true"]}, ensure_ascii=False))
    write(tmp_path, "zzz.small", {"description": 'say "hi"\n\t\\ 😀', "exec": ["/bin/true"]})
    metadata = {**REGISTER_METADATA, "display_name": '界"\\' * 5000}
    executor = Executor(account, commands.load(tmp_path))
    specs = executor.specs(**metadata)
    assert list(specs)[len(COMMAND_SPECS):] == ["aaa.large", "zzz.small"]
    assert registration_size(specs, metadata) <= MAX_CONTROL_MESSAGE_BYTES
    assert set(specs) == set(COMMAND_SPECS) | set(executor.drop_ins)
    assert "bbb.large" in caplog.text and "ccc.large" in caplog.text
    assert "say" not in caplog.text


def test_registration_rejects_oversize_metadata_without_removing_builtins(account):
    with pytest.raises(ValueError, match="metadata and built-in"):
        Executor(account).specs(display_name="界" * 50000)


def test_no_drop_in_registration_remains_byte_identical(account):
    from musegadget.link_client import DeviceDescription, encode_message
    device = DeviceDescription(commands=Executor(account).specs(**REGISTER_METADATA), **REGISTER_METADATA)
    original = {"type": "req", "id": "0" * 36, "method": "link.register", "params": {
        **REGISTER_METADATA, "platform": "linux", "device_family": "homehub", "model_id": "linux",
        "is_wakeup_supported": False, "commands_v2": COMMAND_SPECS,
    }}
    # Object-key order in the original wire is the register_params order.
    original["params"] = DeviceDescription(commands=COMMAND_SPECS, **REGISTER_METADATA).register_params()
    assert encode_message(device.register_message("0" * 36)) == encode_message(original)


@pytest.mark.parametrize("uid, mode", [(12345, 0o644), (0, 0o664), (0, 0o646)])
def test_file_trust_checks_still_apply_in_a_safe_directory(tmp_path, monkeypatch, caplog, uid, mode):
    write(tmp_path, "pump.set", SPEC)
    original = commands.os.fstat

    def fstat(fd):
        result = original(fd)
        values = list(result)
        values[4] = 0 if stat.S_ISDIR(result.st_mode) else uid
        if stat.S_ISREG(result.st_mode):
            values[0] = stat.S_IFREG | mode
        return os.stat_result(values)

    monkeypatch.setattr(commands.os, "geteuid", lambda: 0)
    monkeypatch.setattr(commands.os, "fstat", fstat)
    assert commands.load(tmp_path) == []
    assert "pump.set.json: must be owned by root" in caplog.text


@pytest.mark.skipif(os.geteuid() != 0, reason="needs real root inside the Linux test container")
def test_disk_loaded_drop_in_really_runs_as_unprivileged_account(watchdog):
    from pathlib import Path

    nobody = Account.lookup("nobody")
    with tempfile.TemporaryDirectory(prefix="muse-run-as-") as directory:
        path = Path(directory)
        path.chmod(0o755)
        write(path, "identity.show", {
            "description": "Return process identity.",
            "exec": [sys.executable, "-c", "import os,json; print(json.dumps({"
                     "'uid':os.getuid(),'gid':os.getgid(),'groups':os.getgroups()}))"],
        })
        executor = Executor(Account(nobody.name, nobody.uid, nobody.gid, directory), commands.load(path))
        assert "identity.show" in executor.specs()
        result = executor.run("identity.show", {})
        assert result["ok"], result
        assert result["payload"]["uid"] == nobody.uid != 0
        assert result["payload"]["gid"] == nobody.gid
        assert 0 not in result["payload"]["groups"]
