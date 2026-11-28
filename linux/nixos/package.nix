# musegadget — Nix package for the Muse Gadget SDK Linux daemon.
#
# The SDK test suite runs in the build (pytestCheckHook). Most tests
# pass as-is against nixpkgs' Python 3.14 / cryptography / websockets,
# which are much newer than the SDK's pyproject floors. The exceptions
# are tests that probe the *host* environment; the Nix sandbox has no
# host to probe, so those are disabled below, each with its reason:
#
#   - test_install_sh.py: the Debian installer's behavior against a
#     host trust store and package manager. Meaningless in the sandbox
#     (it reports the sandbox's missing python3-cryptography, etc.).
#   - test_tls.py: the host system trust store, which the sandbox
#     deliberately does not expose.
#   - test_wakeword_esp32_sync.py / test_wakeword_setup.py: the ESP32
#     firmware tree and host build tools (unzip, …); the linux/
#     package source does not contain the esp32/ tree.
#   - test_executor.py system_run tests, test_commands.py,
#     test_command_guard.py: process-group / FHS-path semantics that
#     differ inside the sandbox.
#   - test_file_write_drops_setuid_setgid_and_sticky_bits: setuid
#     operations are not permitted in the sandbox.
#
# Verified on NixOS 26.05 (x86_64): 1314 passed, 0 failed with the
# disables below, and the daemon paired and registered against the
# Muse service from a live install.
{
  lib,
  buildPythonApplication,
  setuptools,
  cryptography,
  websockets,
  dbus-python,
  pygobject3,
  pytestCheckHook,
  libopus,
  makeWrapper,
  cacert,
  python,
  runCommand,
  musegadget-src,
}:

let
  # musegadget refuses to connect while its trust store trusts the
  # roots in tls.py's COMPROMISED_ROOTS (the eMudhra intermediates
  # whose keys became public, and the Comodo AAA root that
  # cross-signed them). nixpkgs' cacert still ships the AAA root, so
  # an unmodified system store fails the check and the daemon exits
  # EX_CONFIG. The fix applied here: the daemon and CLI run with a
  # filtered copy of nixpkgs' own bundle — the same Mozilla store
  # minus the distrusted roots, matched by certificate hash, key
  # (SPKI) hash, or subject CN, mirroring tls.py's own matching.
  # Nothing else on the system is affected.
  #
  # Note the bundle mixes plain CERTIFICATE blocks with TRUSTED
  # CERTIFICATE blocks (287 blocks, 169 unique certs); the AAA root
  # is one of the trusted-format ones, so the filter must parse both.
  pythonWithCrypto = python.withPackages (ps: [ ps.cryptography ]);
  musegadgetCABundle = runCommand "musegadget-ca-bundle" { } ''
    ${pythonWithCrypto}/bin/python3 ${./filter-ca-bundle.py} \
      ${cacert}/etc/ssl/certs/ca-bundle.crt $out
  '';
in
buildPythonApplication {
  pname = "musegadget";
  version = "0.1.0";
  pyproject = true;

  # The SDK's linux/ directory (pyproject.toml + src/ layout).
  # The flake passes the repo checkout in; pin it there, not here.
  src = musegadget-src;
  patches = [ ./executor-bash-lookup.patch ];

  build-system = [ setuptools ];

  # cryptography + websockets are the runtime deps (pyproject floors:
  # cryptography>=3.3.2, websockets>=13 — nixpkgs is newer; the test
  # suite in checkPhase is what proves that is fine).
  # dbus-python + pygobject3 are imported ONLY by ble_server.py, which
  # cli.py loads lazily inside `musegadget pair` — the long-running
  # service never touches them. They ride along so the pair CLI works
  # from the same closure.
  dependencies = [
    cryptography
    websockets
    dbus-python
    pygobject3
  ];

  # libopus is ctypes-loaded by the video-call path, so it has to be
  # findable at runtime: buildInputs alone does not put it on the
  # loader path. preCheck exports LD_LIBRARY_PATH for the test run
  # (one wakeword/video test needs it), and postFixup wraps the
  # installed binary with it — plus the filtered CA bundle above, so
  # the service and the CLI see the same trust store with no
  # environment plumbing on the host.
  buildInputs = [ libopus ];

  nativeBuildInputs = [ makeWrapper ];

  preCheck = ''
    export LD_LIBRARY_PATH=${libopus}/lib:$LD_LIBRARY_PATH
  '';

  postFixup = ''
    wrapProgram $out/bin/musegadget \
      --prefix LD_LIBRARY_PATH : ${libopus}/lib \
      --set SSL_CERT_FILE ${musegadgetCABundle}
  '';

  passthru = {
    # The filtered trust store the binary runs with, for inspection
    # (e.g. comparing against the system bundle).
    caBundle = musegadgetCABundle;
  };

  nativeCheckInputs = [ pytestCheckHook ];

  # Host-environment test files (see header note): they assert
  # against a Debian host's trust store, installer script, firmware
  # tree or build tools — none of which exist in the sandbox.
  disabledTestPaths = [
    "tests/test_install_sh.py"
    "tests/test_tls.py"
    "tests/test_wakeword_esp32_sync.py"
    "tests/test_wakeword_setup.py"
  ];

  # Individual sandbox-environment failures (see header note).
  disabledTests = [
    "test_system_run_returns_output_and_exit_code"
    "test_system_run_defaults_to_the_account_home"
    "test_system_run_times_out_and_kills_the_process_group"
    "test_system_run_timeout_does_not_wait_for_a_detached_process"
    "test_system_run_returns_when_the_shell_exits_but_a_child_holds_the_pipes"
    "test_system_run_truncates_large_output"
    "test_file_write_drops_setuid_setgid_and_sticky_bits"
    "test_drop_in_times_out_and_kills_the_process_group"
    "test_guarded_service_without_guard_delegates"
  ];

  # Tests need no Bluetooth, device or network (SDK AGENTS.md).
  pythonImportsCheck = [ "musegadget" ];

  meta = {
    description = "Make any Linux computer into a Muse Home Link";
    license = lib.licenses.asl20;
    mainProgram = "musegadget";
  };
}
