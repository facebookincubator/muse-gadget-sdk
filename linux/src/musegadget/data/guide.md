<!--
Copyright (c) Meta Platforms, Inc. and affiliates.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
-->

# Linux gadget

This device is a Linux computer running the Muse Gadget SDK's Linux client,
`musegadget`. It pairs with the Muse app over Bluetooth and keeps an encrypted
Link session with Muse over the computer's own network connection. Muse uses
it only through the commands it lists; each command's description says what it
does and returns.

## Using it

- Commands run as the account the owner chose at install, not as root, with
  no terminal.
- Find out what the machine is before acting on it, e.g. `uname -a` and
  `cat /etc/os-release`.
- Nothing can answer a prompt: use non-interactive flags (`apt-get -y`), and
  start long work in the background with its output in a file to read later.
- The account has only its own permissions; `sudo` works only if the owner
  set it up.
- Ask the user before changes that are hard to undo, such as installing or
  removing software, editing system files or rebooting.

## Limits

- Up to four commands run at once.
- Commands work only while the service is running and connected. Restarting
  the service ends any command still running.
- Bluetooth setup is open only while `musegadget pair` runs on the device.
