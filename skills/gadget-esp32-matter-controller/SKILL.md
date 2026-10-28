---
name: gadget-esp32-matter-controller
description: >-
  Commission, control, read and subscribe to Matter devices (Wi-Fi, and Thread through the
  home's border router) with chip-tool-remote, through an ESP32 gadget that forwards its Matter
  messages to the devices. Use when the gadget registered matter.connect and the user wants to
  add a Matter device from its setup code, or control, read or watch one.
---

# ESP32 Matter Controller

Use this skill when the gadget registers `matter.connect`. The gadget is the home's Matter controller: it commissions devices (over its own Bluetooth, or on the network). In the VM, `chip-tool-remote` sends ordinary chip-tool commands to it over one TCP connection through the gadget's tunnel. Matter's UDP, multicast discovery and the IPv6 that Thread devices need stay in the home.

## Prerequisites

Follow the Home Link safety and approval rules in `~/docs/devices/home_link.md`. Matter devices are reached only through the gadget: don't discover or contact them directly. `chip-tool-remote` connects to one address, the gadget's, from `matter.connect`; the first connection waits for the user to approve it (don't time out or retry).

## Setup

Install `chip-tool-remote` in the VM and connect it to the gadget, once:

1. Call `matter.connect`. The payload has `endpoint`, `secret` and `client`, the `chip-tool-remote` release that matches the gadget's firmware.
2. `chip-tool-remote` is chip-tool built to reach Matter devices through the gadget, with its `chip-tool-remote.sh` wrapper. It isn't in the VM: unless `chip-tool-remote/VERSION` already says the `client` release, download that release, check it, and unpack it into `chip-tool-remote/`:

   ```bash
   v=<client>
   [[ $v =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ]]   # stop if this fails: a release looks like v1.2.3
   f=chip-tool-remote-$v-linux-x86_64.tgz
   base=https://github.com/dhavalgujar/chip-tool-remote-forwarder/releases/download/$v
   curl -fsSL -O "$base/$f" -O "$base/$f.sha256"
   sha256sum -c "$f.sha256"   # stop if this fails
   mkdir -p chip-tool-remote && tar -xzf "$f" -C chip-tool-remote --strip-components=1
   ```

   Unpacking over an existing `chip-tool-remote/` keeps `storage/` (the fabric's CA, see Limits) and `connect.json`.

3. Save the payload as `connect.json` next to `chip-tool-remote.sh` (in the `chip-tool-remote/` directory), with `umask 077`. The secret authorises control of the home's devices: never print it, echo it back or store it anywhere else.
4. Check with `./chip-tool-remote.sh forwarder call matter.gateway_status`. If `fabric_ca` is `not_enrolled`, enroll the gadget once with `./chip-tool-remote.sh forwarder call matter.enroll`: this VM's `chip-tool-remote/storage/` becomes the fabric's CA, and the gadget restarts to use the new fabric (about a minute).
5. `./chip-tool-remote.sh forwarder call matter.nodes` lists the devices with their node IDs and names (labels, see Device Names).

`chip-tool-remote.sh` runs `chip-tool-remote` with the endpoint (through the home-LAN CONNECT proxy) and the secret. Keep `chip-tool-remote/storage/`: it holds the fabric's CA.

`forwarder call` prints the gadget's answer as the last line of its output, one JSON object (`{"ok":true,"payload":...}` or `{"ok":false,"error":...}`): `| tail -1` shows it. Run the calls that change something (`matter.commission`, `matter.remove`, `matter.enroll`, `matter.open_window`, `matter.connect` with `rotate`) once. To filter their output, save it first (`> out.log 2>&1`) and read the file, rather than running them again.

Call `matter.connect` again and rewrite `connect.json` (step 3) after a "does not know this secret" error, or if `chip-tool-remote.sh` can't reach the gadget (no answer, can't connect, the proxy refused the address) while the gadget still answers Link commands: its address on the home network can change (a new DHCP lease). If `client` changed, install that release first (step 2).

## Control and Read

chip-tool's cluster commands work, with the node ID from `matter.nodes`. Every one ends with the node and the endpoint:

```
./chip-tool-remote.sh <cluster> <command> [arguments] <node> <endpoint>
./chip-tool-remote.sh <cluster> read <attribute> <node> <endpoint>
./chip-tool-remote.sh <cluster> write <attribute> <value> <node> <endpoint>
./chip-tool-remote.sh <cluster> subscribe <attribute> <min-interval> <max-interval> <node> <endpoint>
```

Endpoint 0 holds the device-wide clusters (`basicinformation`, `descriptor`, `generaldiagnostics`). The device's functions (a light's `onoff`, `levelcontrol`) are on endpoint 1 and up: `descriptor read parts-list <node> 0` lists them. For example:

```bash
./chip-tool-remote.sh onoff toggle <node> 1
./chip-tool-remote.sh onoff read on-off <node> 1
./chip-tool-remote.sh levelcontrol move-to-level 128 10 0 0 <node> 1
./chip-tool-remote.sh colorcontrol move-to-color-temperature 300 10 0 0 <node> 1
./chip-tool-remote.sh basicinformation read product-name <node> 0
./chip-tool-remote.sh generaldiagnostics read network-interfaces <node> 0   # Type: 1 Wi-Fi, 4 Thread
./chip-tool-remote.sh any read-by-id 0xFFFFFFFF 0xFFFFFFFF <node> 0xFFFF     # everything the device has
./chip-tool-remote.sh descriptor read parts-list <node> 0                    # endpoints
```

- Find the success line: `Status=0x0` for commands, `Data = ...` for reads.
- Some of chip-tool's commands don't work through the forwarder, or break it: see the next section.
- To learn what a device offers, read its descriptor (`server-list` per endpoint), then only the clusters you need. The wildcard read above prints every attribute (hundreds of lines) and is slow on Thread devices.
- To tell whether a device is on Wi-Fi or Thread, read `generaldiagnostics read network-interfaces <node> 0` (`Type`: 1 Wi-Fi, 2 Ethernet, 4 Thread).
- **For many commands or live updates,** keep one `./chip-tool-remote.sh interactive start` running and write commands to its stdin, e.g. `onoff subscribe on-off 1 60 <node> 1 --auto-resubscribe true`; end it by writing `quit` (it doesn't stop at the end of its input). To watch a device, subscribe; don't repeat reads. Each one-shot command is a new process (about 1 s).
- **Sleepy devices** (battery sensors, buttons, locks) sleep and wake on a schedule. The gadget registers as their check-in client when it commissions them, and `forwarder call matter.icd` lists them. A read or command to one can wait or time out until it checks in: call `matter.icd_wait` first. Its subscriptions resume when it checks in.

## What Doesn't Work Through the Forwarder

Not available (chip-tool-remote answers `Unknown cluster or command set`):

- `pairing`: commission with `forwarder call matter.commission`, remove a device with `matter.remove`, share it with another ecosystem with `matter.open_window`.
- `discover`: `forwarder call matter.commissionables` finds devices in pairing mode, and `matter.nodes` lists the forwarder's devices.
- `dcl`, `groupsettings`.

Don't use (they fail, or break the forwarder for every client):

- Group destinations (`0xFFFFFFFFFFFF....`): send to each node instead.
- `--allow-large-payload` and `--commissioner-name`.
- Commands on the device's operational credentials: `operationalcredentials` `remove-fabric`, `add-noc`, `update-noc`, `add-trusted-root-certificate`; `generalcommissioning` `arm-fail-safe`, `commissioning-complete`; or the same with `any command-by-id`. They change the forwarder's own access to the device.
- `icdmanagement register-client` / `unregister-client`: the forwarder is the device's ICD client (`forwarder call matter.icd`).
- An ACL write (`accesscontrol write acl`) without the forwarder's Administer entry: read the ACL first and keep that entry.

## Commission a Device

Get the setup code: the QR text `MT:...` or the 11/21-digit manual code. From a photo of the code (on the device or its box), decode the QR code in a venv: `python3 -m venv /tmp/qr && /tmp/qr/bin/pip install -q opencv-python-headless`, then with `/tmp/qr/bin/python`, `data, _, _ = cv2.QRCodeDetector().detectAndDecode(cv2.imread("photo.jpg"))`. If that fails, ask the user for the manual code printed below the QR code. Use the code only in the command: don't repeat it back or keep it in notes or memory. The label and node ID are enough to find the device later. Which code to use depends on the device:

- **Wi-Fi devices:** the code printed on the device or its box. The gadget adds the device over Bluetooth and gives it the Wi-Fi network the gadget itself is on.
- **Thread devices:** ask the user to add the device in the app of their Thread Border Router first (Apple Home, Google Home, Alexa, SmartThings, Home Assistant). Then they share it from that app ("pair with another app" / "share with other services"), and you use the code the app shows. The device is then already on the home's Thread network; the gadget finds it through the Border Router and needs no Thread credentials.
- **Devices already in another ecosystem** (Wi-Fi or Thread): the same sharing code.
- **Any device the user says is already on the network**, or in pairing mode there: its code. `forwarder call matter.commissionables` lists the devices in pairing mode on the network (`timeout_s`, default 4): `commissioning_mode` 1 is the device's own window (the printed code works), 2 one opened by another app (use the code that app shows).

Then commission:

```bash
./chip-tool-remote.sh forwarder call matter.commission --params '{"code":"...","label":"desk lamp"}'
```

The gadget looks for the device over Bluetooth and on the network at once.

- It takes up to a few minutes.
- On the way, this VM verifies the device's attestation and signs its certificate: the log shows `Forwarder attestation: verified` and `Forwarder signer: signed`.
- Keep the returned `node_id`.

**Advanced: a new Thread device straight from its printed code.** This needs the home's Thread network credentials (the Border Router's active operational dataset) on the gadget. Only offer it when the user can export the dataset themselves, e.g. Home Assistant's OpenThread Border Router or `ot-ctl dataset active -x`.

- Check with `./chip-tool-remote.sh forwarder call thread.status`.
- Store it with `./chip-tool-remote.sh forwarder call thread.set_dataset --params '{"dataset":"..."}'`.
- The dataset holds the network key: never repeat it back or keep it anywhere else.

Other forwarder commands:

- `matter.open_window` (`node_id`): a manual code that lets another ecosystem add the device.
- `matter.remove` (`node_id`): the device leaves this home's fabric only. If it can't be reached because it was factory reset or is gone (`remove_failed`), add `"forget":true`: the forwarder only drops it from its list.
- `matter.icd`: the sleepy devices this gadget is the check-in client of.
- `matter.icd_wait` (`node_id`, optional `timeout_s`, `stay_active_ms`): waits for a sleepy device to check in (and keeps it awake). Wait for one before reading it.

## Device Names

Talk to the user about devices by name. Node IDs are for commands: don't show them, and say "add" and "remove" rather than "commission" or "fabric".

- **Ask for a name before adding a device**, unless the user already gave one: "What would you like to call it?" (e.g. "desk lamp", "hall light"). Pass it as `label`. Don't make one up.
- **The label is the forwarder's.** It is stored on the gadget with the device, `matter.nodes` returns it, and only `chip-tool-remote` through this forwarder sees it; other apps don't. At most 32 bytes (longer ones are refused).
- **A device shared from another app** (Apple Home, Google Home, ...) may already have a name of its own, its `NodeLabel`. After adding one, read it: `./chip-tool-remote.sh basicinformation read node-label <node> 0`. If the user gave no name, add the device without `label` (it's optional), then offer that name, or ask if it's empty. If they gave a different name, mention it: "It's called 'kitchen light' in your other app."
- **To rename:** `./chip-tool-remote.sh forwarder call matter.rename --params '{"node_id":<node>,"label":"..."}'` changes the forwarder's label only (`"label":""` removes it).
- **The device's own name** changes only if the user asks for that: `./chip-tool-remote.sh basicinformation write node-label 'desk lamp' <node> 0`. Quote the name once, for the shell, as shown: chip-tool stores the argument as it gets it, so `'"desk lamp"'` would make the name `"desk lamp"`, quote marks included. Every app the device is shared with reads the same `NodeLabel`, so tell the user it may show there too.

## Verify the Result

- `Status=0x0` means the device accepted the command. Read the state back only when it matters, or keep a subscription; the physical state may still need the user to confirm.
- `Run command failure ... Error 0x0000002F`: chip-tool-remote rejected the arguments before connecting, often a missing endpoint (see the format in Control and Read). Nothing reached the device; fix the command.
- `Forwarder attestation: refused (<code>)`: the device's certificates aren't trusted (e.g. not a certified product). Tell the user; don't retry.
- `not_enrolled`: the gadget has no fabric CA yet: enroll it (Setup, step 4).
- `busy`: another commissioning is running. Wait and retry.
- `not_found`: no device answered the code, over Bluetooth or on the network. Check that the device is in pairing mode and near the gadget, then retry.
- `timeout` on `matter.commission`: the device was found but never accepted the code (often a wrong or mistyped code). Check the code with the user.
- `commissioning_failed` names the stage:
  - stages before PASE: the code, distance, or the device not being in pairing mode;
  - Thread or Wi-Fi network stages: credentials;
  - `FindOperational`: the device joined but wasn't found, so check the border router.

## Limits

- Node IDs are never reused. Removing a device and adding it again gives a new ID.
- This VM's `storage/` holds the fabric's CA. Losing it means the gadget can't add devices until it is reset (which forgets its devices) and enrolled again.
- If the user wants a new secret (`matter.connect` with `"rotate":true`), every open chip-tool-remote connection closes; save the new payload and reconnect.

## Sources

- [chip-tool-remote-forwarder](https://github.com/dhavalgujar/chip-tool-remote-forwarder): the forwarder on the gadget, and chip-tool-remote's releases
- [connectedhomeip chip-tool](https://github.com/project-chip/connectedhomeip/tree/master/examples/chip-tool): the command syntax
- [Matter Application Cluster Specification](https://csa-iot.org/developer-resource/specifications-download-request/)
