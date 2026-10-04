# ARM64 Linux board simulator

This Docker stand-in exercises the Linux role of a Nuvoton MA35 board: its own
Muse identity, SSH enrollment receiver, persistent device credentials and
outbound Muse connector. It runs ARM64 Debian through Docker's ARM emulation
on an Intel Mac. No Bluetooth, BlueZ or USB hardware is required inside Docker.

It does not emulate Nuvoton's boot ROM, USB controller, LCD/touch hardware,
flash chips, Cortex-M4 or CPU clock speed. Container memory limits include
256 MB of additional allowance for ARM emulation and simultaneous SSH processes.
An initial 128 MB hard limit caused an out-of-memory exit during H0 SSH access
on the Intel Mac. These software tests do not establish RAM fit on a physical
board with its kernel and display stack.

## Four independent boards

Nuvoton's official board pages distinguish these configurations:

| Instance | HMI board | DDR budget | Display / networking | SSH port |
| --- | --- | --- | --- | --- |
| `ma35h0-a1` | [MA35H0-A1](https://www.nuvoton.com/products/gui-solution/gui-platform/numaker-hmi-ma35h0-a1/) | 128 MB | Resistive touch; 10/100 Ethernet | 2226 |
| `ma35h0-a2` | [MA35H0-A2](https://www.nuvoton.com/products/gui-solution/gui-platform/numaker-hmi-ma35h0-a2/) | 128 MB | Capacitive touch; 10/100 Ethernet | 2227 |
| `ma35d1-a1` | [MA35D1-A1](https://www.nuvoton.com/products/gui-solution/gui-platform/numaker-hmi-ma35d1-a1/) | 512 MB | 7-inch resistive touch; dual Gigabit Ethernet | 2224 |
| `ma35d1-s1` | [MA35D1-S1](https://www.nuvoton.com/products/gui-solution/gui-platform/numaker-hmi-ma35d1-s1/) | 256 MB | SOM + baseboard; 7-inch resistive touch; dual Gigabit Ethernet | 2225 |

MA35H0 is a separate processor family from MA35D1. The A1/A2 MA35H0 boards
share the MA35H04F764C processor, DDR capacity and the same relay software.
Check the label on the physical board before selecting a BSP or USB port.

## Start

With Docker Desktop running, from the repository root:

```sh
sh docker-mac/board-sim/start.sh
```

The initial build can take several minutes on an Intel Mac. The simulator has
two virtual CPUs per board and no swap. Docker memory caps are 768 MB for D1-A1,
512 MB for D1-S1 and 384 MB for each H0. SSH listens
only on the four localhost ports above. The start script generates a private client key under
the ignored `.board-sim/` directory and pins the host key obtained directly
through Docker. It prints the ARM64 architecture and fresh board identity.

All four instances have independent persistent identity, pairing, home-directory
and SSH host-key volumes. The existing desktop connector uses a separate
container and state. Starting an already enrolled instance preserves its pairing.

## Test without a phone or a live Muse account

```sh
.mac-venv/bin/python docker-mac/board-sim/smoke.py
```

This sends real P-256/AES-GCM provisioning packets through the Mac relay and
verified SSH connection to the ARM64 SDK. It uses the SDK's published synthetic
phone vectors and a stubbed Muse API. The fixture stores state in an isolated
temporary directory, verifies identity retention and 0700/0600 permissions,
then removes that fixture. It leaves the simulator's real enrollment unchanged.
The default tests all four boards; pass an instance name to test only one.
Passing this test does not establish live Muse registration or physical USB
compatibility.

## Enroll the simulated board with the phone

Build the Mac BLE helper using `sh docker-mac/setup-mac.sh`, then:

```sh
sh docker-mac/board-sim/enroll.sh ma35h0-a1 --sdk-token-prompt
```

Replace `ma35h0-a1` with the instance to enroll. Enroll one board at a time;
the Mac advertises the selected simulated board's MuseGadget name. Add that name in the
Muse app with Developer mode enabled and choose **Use current connection**.
The simulated board connects to the internet through Docker's Mac network.
Credentials persist in its own Docker volume. An SDK token alone does not
complete enrollment; the phone must approve it.

For token-file and other options see [board enrollment](../BOARD-ENROLLMENT.md).
After live enrollment, verify `registered with the Muse` in container logs:

```sh
docker compose -f docker-mac/board-sim/compose.yaml logs --tail 40 ma35h0-a1
```

Stop without deleting enrollment:

```sh
docker compose -f docker-mac/board-sim/compose.yaml stop
```

## Live AI chat test

Reply streaming is supplied by the Linux SDK changes in the
[macOS companion PR #62](https://github.com/facebookincubator/muse-gadget-sdk/pull/62).
With that checkout available and the selected boards enrolled, enable its
chat modules on idle simulators (this restarts the selected connectors):

```sh
sh docker-mac/board-sim/enable-ai.sh /path/to/companion-checkout/linux ma35d1-a1 ma35d1-s1
.mac-venv/bin/python docker-mac/board-sim/ai-smoke.py ma35d1-a1 ma35d1-s1
```

The test starts a separate side chat for each board and checks a greeting and
`17 × 23 = 391`. Add `--commands` to also ask Muse to run `uname -m` and `id -un`
on that board. The optional command check requires both the reported output
and a matching invocation in that target's logs; it is separate from basic AI
chat. Results, including failed turns, are saved under the ignored `.board-sim/`
directory. AI inference runs remotely in Muse; the Linux gadget executes its
requested commands. This does not benchmark a local model on Nuvoton hardware.

The optional AI image contains code only; each board retains its own credential
volume. Use both Compose files when starting that image again. Running the base
`start.sh` restores the base enrollment runtime and preserves pairing.
