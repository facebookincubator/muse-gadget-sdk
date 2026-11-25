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

# M5Stack Stack-Chan (CoreS3 Edition)

Board support for the **M5Stack Stack-Chan**: combining the **M5Stack CoreS3** unit (ESP32-S3, 16 MB flash, 8 MB quad PSRAM, 320x240 ILI9342C touch display, dual ES7210 microphones, AW88298 amplifier, AXP2101 PMIC, AW9523B IO expander) with the **Stack-Chan motorized base** (2x Feetech SCS0009 serial bus servos on UART and 12x WS2812C RGB LEDs via the PY32L020 base controller at I2C address `0x6F`).

---

## Recursos Incluídos / Supported Features

- **Pareamento Bluetooth LE e Wi-Fi:** Provisionamento direto pelo aplicativo móvel Muse (com *Allow community devices* ativo).
- **Display e Touch:** Interface gráfica com avatar animado e legendas em tempo real no LCD ILI9342C de 2,0 polegadas (320x240) com touch capacitivo FT6336U.
- **Áudio Bidirecional:** Captação pelos microfones duplos ES7210 e reprodução de voz no alto-falante amplificado pelo chip I2S AW88298.
- **Gerenciamento de Energia:** Ativação automática do barramento de 5V da base via expansor AW9523B e gerenciamento de bateria pelo PMIC AXP2101.
- **Movimento e Expressões Físicas dos Servos (Feetech SCS0009):**
  - `idle`: Centralização suave (Yaw 500, Pitch 580) e respiração sutil periódica.
  - `listening`: Postura atenta com inclinação para frente e LEDs em azul suave.
  - `thinking`: Inclinação curiosa de cabeça (tilt/nod) e animação em magenta/roxo.
  - `speaking`: Micro-movimentos expressivos de cabeça (assentimento e leve oscilação) sincronizados com o nível de voz em tempo real (`muse_state_level()`).
  - Totalmente seguro e não-bloqueante (nunca trava o loop principal de áudio/rede se a base for desconectada).
- **LEDs RGB na Base (12x WS2812C):** Iluminação dinâmica de status controlada pelo coprocessador PY32L020 da base.

---

## Como Configurar o SDK Token Sem Recompilar Nada

Você **não precisa instalar o ESP-IDF nem recompilar o código** para usar o Stack-Chan. O firmware aceita a configuração do token via porta serial (USB) e armazena na memória não-volátil (NVS) do ESP32.

### Passo 1: Obtenha seu Token do Muse
Acesse [gadgets.muse.ai/settings/sdk-tokens](https://gadgets.muse.ai/settings/sdk-tokens) e gere seu SDK Token (começa com `mgst_` e tem 48 caracteres).

### Passo 2: Grave o Firmware Pré-compilado (se ainda não gravou)
Instale apenas o `esptool`:
```bash
pip install esptool
```
Coloque o CoreS3 em **modo bootloader** (segure o botão lateral RST/G0 por 2 a 3 segundos até o LED acender em verde, depois solte). Em seguida, grave os binários:
```bash
esptool.py --chip esp32s3 -p <PORTA_SERIAL> -b 460800 write_flash \
  0x00000 bootloader.bin \
  0x10000 partition-table.bin \
  0x1d000 ota_data_initial.bin \
  0x20000 muse-gadget.bin
```

### Passo 3: Envie o Token pela Serial Console
Com o dispositivo conectado ao computador e rodando o firmware, você pode enviar o comando de configuração usando um script Python simples, um monitor serial (como Arduino IDE, miniterm ou PuTTY) ou o Web Serial.

#### Opção A: Script Python Automático (Recomendado)
Crie um script `set_token.py` ou execute via terminal:

```python
import serial
import time
import sys

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/cu.usbmodem2101" # Ajuste para a sua porta
TOKEN = sys.argv[2] if len(sys.argv) > 2 else "mgst_SEU_TOKEN_AQUI"

print(f"Conectando a {PORT}...")
ser = serial.Serial(PORT, 115200, timeout=1)
time.sleep(1)

# Envia o comando de configuracao
cmd = f">sdk_token={TOKEN}\n".encode()
print(f"Enviando token: {TOKEN[:8]}...")
ser.write(cmd)
ser.flush()

# Aguarda confirmacao
time.sleep(1)
resp = ser.read_all().decode("utf-8", errors="replace")
print("Resposta do Stack-Chan:", resp.strip())

if "@sdk_token ok" in resp or "stored the SDK token" in resp:
    print(" Sucesso! O Stack-Chan salvou o token na NVS e reiniciou.")
else:
    print(" Verifique se o dispositivo esta rodando e se o token esta correto.")

ser.close()
```

Execute com:
```bash
python set_token.py /dev/cu.usbmodem2101 mgst_SEU_TOKEN_AQUI
```

#### Opção B: Monitor Serial (Arduino IDE / miniterm / Screen)
1. Abra a porta serial do CoreS3 a **115200 baud**.
2. Digite ou cole a linha exatamente neste formato (com o caractere `>` no início):
   ```text
   >sdk_token=mgst_SEU_TOKEN_AQUI
   ```
3. Pressione **Enter**.
4. A placa responderá `@sdk_token ok` e reiniciará automaticamente com o novo token salvo.

---

## Compilação a Partir do Código Fonte (Desenvolvedores)

Se você quiser modificar o código ou recompilar você mesmo:

### 1. Requisitos
- **ESP-IDF v6.0.3** com suporte ao alvo `esp32s3`.
- Python 3.10 ou 3.11.

### 2. Compilação
No diretório `esp32/`:
```bash
tools/muse/board.sh build stackchan
```

### 3. Gravação
```bash
tools/muse/board.sh flash stackchan /dev/cu.usbmodem2101
```
*(Se o esptool não reiniciar o CoreS3 automaticamente, dê um clique rápido no botão lateral RST/Power ou reconecte o cabo USB-C).*
