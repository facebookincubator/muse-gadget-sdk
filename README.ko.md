<!--
Copyright (c) Meta Platforms, Inc. and affiliates.
Copyright (c) 2026 ledienbien-ai (translation and fork-specific text)

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

<p align="center">
  <a href="https://dbrobot.vn/"><img src="doc/image/db-robot-logo.png" width="160" alt="DB_ROBOT"></a>
</p>
<p align="center">
  개발자: <b>ledienbien-ai</b> · 홈페이지: <a href="https://dbrobot.vn/">https://dbrobot.vn/</a>
</p>

# ESP32-S3 기기용 Muse Gadgets

[English](README.md) | [Tiếng Việt](README.vi.md) | [简体中文](README.zh-CN.md) | [日本語](README.ja.md) | **한국어**

> 이 저장소는 [facebookincubator/muse-gadget-sdk](https://github.com/facebookincubator/muse-gadget-sdk)의
> 비공식 커뮤니티 포크입니다. Meta, Waveshare를 비롯한 어떤 보드 제조사와도
> 관련이 없으며, 그 회사들의 승인을 받은 것도 아닙니다. 번역과
> [영어판](README.md)이 다를 경우 영어판이 기준입니다.

<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset=".github/images/muse-gadgets-dark.png">
    <img src=".github/images/muse-gadgets-light.png" width="900" alt="Muse gadget: Waveshare 원형 AMOLED, M5Stack StickS3, Muse Home Link, Raspberry Pi, Seeed reTerminal 전자잉크 디스플레이">
  </picture>
</p>

Muse gadget은 직접 만드는 오픈 소스 기기입니다. 시중에서 파는 ESP32 보드에
프로그램을 올리거나 Raspberry Pi를 설정한 다음, 디스플레이, 버튼, 센서,
액추에이터를 Muse에 연결합니다. 이 포크는 업스트림 ESP32 Device SDK가 아직
지원하지 않는 ESP32-S3 보드를 추가합니다. SDK의 나머지 부분은 업스트림과
같습니다.

해커가 해커를 위해 재미로 만든 프로젝트입니다. 커스텀 펌웨어를 올리면 보드가
벽돌이 되거나 보증이 무효가 될 수 있습니다. 위험은 본인이 감수해야 합니다!

## 이 포크의 보드

모든 보드가 화면 UI 전체를 실행합니다. 움직이는 아바타, 눌러서 말하기, 터치로
하는 설정, Muse가 보내는 이미지 표시입니다.

| 보드 | 화면 | 이름 | 프로필 | 펌웨어 |
|---|---|---|---|---|
| [Waveshare ESP32-S3-Touch-LCD-1.85C](#waveshare-esp32-s3-touch-lcd-185c) | 1.85인치 원형 360×360, 터치 | `s3lcd` | `waveshare-s3-185c` | 펌웨어 온라인 플래시: https://dbrobot.vn/firmware.html |
| [OSTB-3ST](#ostb-3st) | 1.83인치 296×240, 터치 | `ostb` | `ostb-3st` | 펌웨어 온라인 플래시: https://dbrobot.vn/firmware.html |
| [LCDWIKI 2.8inch ESP32-S3 Display](#lcdwiki-28inch-esp32-s3-display) | 2.8인치 320×240, ES3C28P는 터치 지원 | `lcd28` | `lcdwiki-s3-28` | 펌웨어 온라인 플래시: https://dbrobot.vn/firmware.html |
| [Espressif EchoEar](#espressif-echoear) | 1.85인치 원형 360×360, 터치 | `echoear` | `echoear` | 펌웨어 온라인 플래시: https://dbrobot.vn/firmware.html |

이름은 `tools/muse/board.sh`가 보드를 부르는 이름이고, 프로필은 빌드 설정과
빌드 디렉터리의 이름입니다. 업스트림이 지원하는 보드도 그대로 남아 있으며,
[`esp32/devices/README.md`](esp32/devices/README.md)(영어)에 정리되어 있습니다.

보드에서 문제가 생기면 이 저장소의
[Issues](https://github.com/ledienbien-ai/muse-gadget-sdk/issues)에 알려 주세요.

## 베트남어 화면과 음성 응답

위의 보드들은 화면 UI에 두 가지 기능이 추가되었습니다.

<p align="center">
  <img src="doc/image/vietnamese-ui.png" width="720" alt="시뮬레이터로 그린 360×360 베트남어 UI: 준비, 응답을 읽는 중, 스피커를 껐을 때의 응답, 오류, 설정, 언어 페이지, 소리 페이지, 페어링 코드">
</p>

**베트남어 또는 영어 화면.** 보드는 베트남어로 시작합니다. Muse에서 왼쪽으로
밀어 **Settings > Language**(Cài đặt > Ngôn ngữ)를 열고 **English** 또는
**Tiếng Việt**를 고릅니다. Muse가 다시 시작하면서 그 언어로 바뀌고, 선택은
저장됩니다. 터치가 없는 보드에서는 시리얼 콘솔로 `>lang=en` 또는 `>lang=vi`를
보내거나 [`esp32/tools/muse/ble_setup.html`](esp32/tools/muse/ble_setup.html)에서
고릅니다. 베트남어에서는 응답과 말한 내용이 성조 부호와 함께 표시됩니다.
영어에서는 부호 없이 표시됩니다. 영어 글꼴에는 그 글자가 없기 때문입니다.

**응답을 소리 내어 읽기.** Muse는 기기에 텍스트로 응답합니다. 스피커가 켜져
있으면 보드는 각 응답을 Google 번역의 텍스트 음성 변환으로 보내고, 돌아온 음성을
재생하며 자막이 그에 맞춰 넘어갑니다. 응답이 베트남어로 쓰여 있으면 베트남어
목소리로, 그렇지 않으면 화면 언어의 목소리로 읽습니다. 스피커가 꺼져 있으면(메인
화면의 스피커 버튼을 길게 누르거나 Settings > Sound) 응답은 글자로만 표시되고
아무것도 전송되지 않습니다.

> 음성은 Google 번역의 "듣기" 버튼 뒤에 있는 주소에서 가져옵니다. 키는 필요
> 없지만 공개된 서비스가 아닙니다. Google이 언제든 바꾸거나 제한하거나 막을 수
> 있고, 읽어 주는 모든 응답의 텍스트가 Google로 전송됩니다. 응답이 없으면
> 이전처럼 글자로 표시됩니다. `CONFIG_MUSE_TTS_GOOGLE=n`으로 빌드하면 이 기능을
> 뺄 수 있고, 직접 고른 서비스는
> [`muse_tts.c`](esp32/components/muse/muse_tts.c)에 넣으면 됩니다.
> `CONFIG_MUSE_LANG_DEFAULT_VI=n`이면 보드가 영어로 시작합니다.

코드: [`muse_lang.c`](esp32/components/muse/muse_lang.c)(화면 문구),
[`muse_fonts.c`](esp32/components/muse/muse_fonts.c)와
[`fonts/`](esp32/components/muse/fonts)(베트남어 글자),
[`muse_tts.c`](esp32/components/muse/muse_tts.c)와
[`muse_tts_text.c`](esp32/components/muse/muse_tts_text.c)(음성).

## Waveshare ESP32-S3-Touch-LCD-1.85C

<p align="center">
  <img src="doc/image/waveshare-s3-185c-device.jpg" width="300" alt="케이스에 든 Waveshare ESP32-S3-Touch-LCD-1.85C">
  <img src="doc/image/waveshare-s3-185c-muse.jpg" width="300" alt="1.85C에 표시된 Muse 설정 화면">
</p>
<p align="center">
  <img src="doc/image/Wareshare%20Touch%20LCD%201.85C.png" width="720" alt="시뮬레이터로 그린 360 px 원형 화면의 Muse UI: 대기, 페어링, 듣는 중, 생각 중, 오류, 말하는 중">
</p>

| 항목 | 내용 |
|---|---|
| 칩 | ESP32-S3R8, 16 MB 플래시, 8 MB 옥탈 PSRAM, 네이티브 USB |
| 디스플레이 | 1.85인치 원형 360×360 ST77916 LCD(QSPI), PWM 백라이트 |
| 터치 | CST816 |
| 오디오, V1 보드 | PCM5101 DAC와 디지털 I2S 마이크 |
| 오디오, V2 보드("Rev2.0") | ES8311 DAC와 ES7210 ADC, 마이크 2개 |
| 버튼 | BOOT: 누르고 있는 동안 말하기 |
| 배터리 | GPIO8의 분압 회로로 전압 측정 |

펌웨어 하나로 두 오디오 버전을 모두 지원합니다. 부팅할 때 ES8311이 있는지
확인합니다.

알려진 제한: 버튼이 BOOT 하나뿐이어서 화면은 타이머에 따라 꺼지고, 전원 끄기는
Settings의 Power 페이지에 있습니다(보드는 딥 슬립에 들어가며 BOOT를 누르면
깨어납니다. 전원을 완전히 끊는 것은 슬라이드 스위치입니다). 충전 IC의 상태가
어느 핀에도 연결되어 있지 않아서, 컴퓨터에 연결되어 있을 때만 "충전 중"으로
표시됩니다.

코드: [`board_waveshare_s3_185c.c`](esp32/components/muse/boards/board_waveshare_s3_185c.c),
빌드 설정: [`sdkconfig.muse-waveshare-s3-185c`](esp32/devices/sdkconfig.muse-waveshare-s3-185c).
하드웨어 자료: [Waveshare 문서](https://docs.waveshare.com/ESP32-S3-Touch-LCD-1.85C).

## OSTB-3ST

<p align="center">
  <img src="doc/image/ostb-3st-device.jpg" width="560" alt="OSTB-3ST의 앞면과 뒷면">
</p>
<p align="center">
  <img src="doc/image/ostb-3st.png" width="720" alt="시뮬레이터로 그린 296×240 Muse UI: 대기, 페어링, 듣는 중, 생각 중, 오류, 말하는 중">
</p>
<p align="center">
  <img src="doc/image/ostb-3st-settings.png" width="720" alt="시뮬레이터로 그린 296×240 설정 페이지">
</p>

이 보드의 xiaozhi-esp32 펌웨어 소스(`ostb-xiaozhi-3st`)를 바탕으로
포팅했습니다. UI는 296×240 가로 화면에 맞춰 배치됩니다.

| 부품 | 내용 |
|---|---|
| 칩 | ESP32-S3, 플래시 16 MB, 옥탈 PSRAM 8 MB, 네이티브 USB |
| 디스플레이 | 1.83인치 240×296 NV3023 LCD(SPI), 가로로 사용, PWM 백라이트 |
| 터치 | CST816. 인터럽트 선이 없어 폴링으로 읽습니다 |
| 오디오 | ES8311 DAC와 ES7210 ADC |
| 키 | 윗면에 있습니다. **+**(볼륨 +): 누르고 있는 동안 말하기. **−**(볼륨 −): 누르면 화면 끄기, 길게 누르면 전원 끄기. 가운데 키는 쓰지 않습니다 |
| 배터리 | 원래 펌웨어의 ADC 표로 계산한 잔량과 충전 상태 핀 |

알려진 제한: 4G 모뎀과 LED는 쓰지 않습니다. 배터리는 잔량만 표시하고 전압은
표시하지 않습니다. 전원 끄기는 보드의 전원 차단 핀을 구동합니다. USB 전원에서는
보드가 꺼지지 않을 수 있으며, 그때는 + 키를 누를 때까지 화면이 꺼진 상태로
있습니다. 배터리가 가득 찬 뒤에는 컴퓨터에 연결되어 있는 동안에만 USB 전원으로
인식합니다.

코드: [`board_ostb_3st.c`](esp32/components/muse/boards/board_ostb_3st.c),
빌드 설정: [`sdkconfig.muse-ostb-3st`](esp32/devices/sdkconfig.muse-ostb-3st).

## LCDWIKI 2.8inch ESP32-S3 Display

<p align="center">
  <img src="doc/image/lcdwiki-s3-28-device.jpg" width="330" alt="LCDWIKI 2.8inch ESP32-S3 Display의 앞면">
  <img src="doc/image/lcdwiki-s3-28-back.jpg" width="330" alt="뒷면. 커넥터와 버튼 설명 포함">
</p>
<p align="center">
  <img src="doc/image/lcdwiki-s3-28.png" width="720" alt="시뮬레이터로 그린 320×240 Muse UI">
</p>

여러 이름으로 판매되는 2.8인치 디스플레이 보드입니다. LCDWIKI에는 정전식 터치가
있는 **ES3C28P**와 터치가 없는 **ES3N28P** 두 모델이 있습니다. 펌웨어 하나로 두
모델을 모두 지원합니다. 부팅할 때 터치 컨트롤러가 있는지 확인합니다.

| 항목 | 내용 |
|---|---|
| 칩 | ESP32-S3R8, 16 MB 플래시, 8 MB 옥탈 PSRAM, 네이티브 USB |
| 디스플레이 | 2.8인치 240×320 ILI9341V LCD(SPI, 40 MHz), 가로로 사용, PWM 백라이트 |
| 터치 | ES3C28P는 FT6336G. ES3N28P에는 없습니다 |
| 오디오 | ES8311 코덱과 마이크 1개, FM8002E 스피커 앰프 |
| 버튼 | BOOT: 누르고 있는 동안 말하기 |
| 배터리 | 원래 펌웨어의 ADC 범위로 계산한 잔량(GPIO9) |

알려진 제한: 버튼이 BOOT 하나뿐이어서 화면은 타이머에 따라 꺼지고, 전원 끄기는
Settings의 Power 페이지에 있습니다(보드는 딥 슬립에 들어가며 BOOT를 누르면
깨어납니다). ES3N28P는 터치가 없고 버튼도 하나뿐이라 기기에 설정 화면이
없습니다. 눌러서 말하기만 되고, 설정은 Muse 앱에서 합니다. 배터리는 잔량만
표시하고 전압은 표시하지 않으며, 충전 IC의 상태가 어느 핀에도 연결되어 있지
않아서 컴퓨터에 연결되어 있을 때만 "충전 중"으로 표시됩니다. SD 카드 슬롯과
RGB LED는 쓰지 않습니다.

코드: [`board_lcdwiki_s3_28.c`](esp32/components/muse/boards/board_lcdwiki_s3_28.c),
빌드 설정: [`sdkconfig.muse-lcdwiki-s3-28`](esp32/devices/sdkconfig.muse-lcdwiki-s3-28).

## Espressif EchoEar

<p align="center">
  <img src="doc/image/echoear-muse.jpg" width="330" alt="EchoEar 위의 Muse 합성 이미지. 화면에 베트남어 응답: 시뮬레이터 화면을 기기 사진에 합성한 것">
  <img src="doc/image/echoear-device.jpg" width="330" alt="두 번째 합성 이미지. 듣는 중">
</p>
<p align="center">
  <img src="doc/image/echoear-boards-front.webp" width="720" alt="EchoEar의 보드와 기기 앞면(설명 포함): ESP32-S3 모듈, 커넥터, 마이크, 녹색 LED, 화면">
</p>
<p align="center">
  <img src="doc/image/echoear-boards-back.jpg" width="605" alt="보드와 기기 뒷면(설명 포함): 코덱, 앰프, 모션 센서, SD 카드 슬롯, BOOT와 RST 버튼, 자석 커넥터, 전원 스위치">
</p>
<p align="center">
  <img src="doc/image/echoear.png" width="720" alt="시뮬레이터로 그린 360×360 Muse UI">
</p>

Espressif의 고양이 모양 음성 개발 키트로, ESP-VoCat이라는 이름으로도 판매됩니다.
보드에는 v1.0과 v1.2 두 가지 버전이 있고 몇몇 핀의 위치가 다릅니다. 하나의
펌웨어가 둘 다 지원합니다. 부팅할 때 버전을 구분하고 로그에 남깁니다.

| 부품 | 세부 사항 |
|---|---|
| 칩 | v1.2는 ESP32-S3-WROOM-1-N16R16VA(16 MB 플래시), v1.0은 ESP32-S3-WROOM-2-N32R16V(32 MB 플래시). 16 MB 옥탈 PSRAM, 네이티브 USB |
| 디스플레이 | 1.85인치 원형 360×360 ST77916 LCD(QSPI), PWM 백라이트 |
| 터치 | CST816S |
| 오디오 | ES8311 코덱, NS4150B 스피커 앰프, ES7210과 마이크 2개(1개 사용) |
| 버튼 | BOOT(뒷면, 자석 커넥터 옆)와 케이스 아래의 터치 패드(v1.2는 두 개, v1.0은 한 개): 어느 것이든 누르고 있는 동안 말하기 |
| 배터리 | BQ27220 잔량 게이지: 잔량, 전압, 충전 상태 |

알려진 제한: 펌웨어가 읽는 버튼은 BOOT 하나뿐이어서 화면은 타이머에 따라 꺼지고,
Settings의 Power 페이지에서 전원을 끄면 보드는 딥 슬립에 들어갈 뿐이며 BOOT를
누르면 깨어납니다. 실제로 전원을 끊는 것은 보드의 전원 키입니다. 터치 패드는
BOOT처럼 말할 때와 화면을 깨울 때 쓸 수 있지만, Muse 앱과의 페어링 확인과 딥
슬립 해제는 하지 못합니다. 둘 다 BOOT를 눌러야 합니다. 20초 동안 계속 눌린 터치
패드는 터치가 아니라 덮인 것으로 보고 뗀 것으로 처리합니다. 모션 센서, SD 카드
슬롯, 녹색 LED는 사용하지 않습니다. 잔량
게이지는 배터리로 동작하므로 배터리가 없으면 잔량이 표시되지 않습니다. 컴퓨터가
아닌 충전기에 연결하면 배터리로 전류가 흘러 들어가는 동안에만 "충전 중"으로
표시됩니다.

코드: [`board_echoear.c`](esp32/components/muse/boards/board_echoear.c),
빌드 설정: [`sdkconfig.muse-echoear`](esp32/devices/sdkconfig.muse-echoear).

## 빌드와 플래시

절차는 모든 보드에서 같습니다. 이름과 프로필은 [위의 표](#이-포크의-보드)에서
확인하세요.

1. [SDK 토큰](https://gadgets.muse.ai/settings/sdk-tokens)을 받고
   [Gadget SDK 약관](https://gadgets.muse.ai/sdk-terms)을 읽습니다. 모든
   gadget은 페어링하려면 토큰이 필요합니다.
2. ESP-IDF **v6.0.1**을 설치합니다([`esp32/README.md`](esp32/README.md) 참고).
3. `esp32/`에서 빌드합니다. macOS 또는 Linux에서는 보드의 이름을 씁니다.

   ```sh
   tools/muse/board.sh build ostb
   ```

   Windows에서는 ESP-IDF PowerShell에서 보드의 프로필을 씁니다.

   ```powershell
   $P = "ostb-3st"
   idf.py -B build-muse-$P -DIDF_TARGET=esp32s3 `
     "-DSDKCONFIG=build-muse-$P/sdkconfig" `
     "-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;devices/sdkconfig.muse;devices/sdkconfig.muse-$P" build
   ```

   토큰은 `idf.py -B build-muse-$P menuconfig`
   (ESP32 Device SDK > Muse Gadgets SDK token)에서 설정한 뒤 다시 빌드합니다.
4. USB-C로 플래시합니다. `PORT`는 보드의 포트로 바꾸세요.

   ```powershell
   idf.py -B build-muse-$P -p PORT flash
   ```

   연결되지 않으면 보드를 다운로드 모드로 만든 뒤 다시 시도합니다. **BOOT**를
   누른 채 **RESET**을 눌렀다 떼고, **BOOT**를 뗍니다. RESET 버튼이 없는
   보드에서는 BOOT를 누른 채 케이블을 꽂습니다.

   웹 플래시 도구를 쓰려면 파일 하나로 합쳐서 주소 `0x0`에 씁니다.

   ```powershell
   idf.py -B build-muse-$P merge-bin -o muse-gadget-$P-merged.bin
   ```

   본인의 SDK 토큰으로 빌드한 펌웨어에는 그 토큰이 들어 있으니, 파일을
   공개하기 전에 신중히 생각하세요.
5. Muse 앱에서 **Settings > Devices > Developer mode**를 켜고,
   `MuseGadget-XXXXXX`라는 이름의 기기를 추가한 다음, 요청이 나오면 보드의
   말하기 버튼을 누릅니다(1.85C, LCDWIKI 보드, EchoEar는 BOOT, OSTB-3ST는 + 키).

## 다른 기기 추가하기

화면, 스피커, 마이크가 있는 보드라면 드라이버 파일 하나와 등록 몇 줄이면
됩니다. 이 포크의 각 보드는 다음으로 이루어져 있습니다.

1. `esp32/components/muse/boards/board_<id>.c`가 `muse_board_t`
   (`muse_board.h`)를 채웁니다. 디스플레이, 터치, 오디오, 버튼, 배터리,
   전원입니다. 자기 보드와 가장 비슷한 보드에서 시작하세요.
2. `esp32/devices/sdkconfig.muse-<profile>`에 빌드 설정을 둡니다. 칩, 플래시,
   PSRAM입니다.
3. `esp32/components/muse/Kconfig`, `CMakeLists.txt`, `idf_component.yml`에
   보드와, 필요한 드라이버 컴포넌트를 등록합니다.
4. `esp32/tools/muse/board.sh`, `ports.py`, `avatar.py`에 짧은 이름을
   추가합니다.
5. 문서에도 추가합니다. 각 README의 표에 한 줄과 절 하나,
   [`esp32/devices/README.md`](esp32/devices/README.md)의 줄들, `doc/image/`의
   그림, 그리고 [`CREDITS.md`](CREDITS.md)에 참고한 출처와 그 라이선스입니다.

UI는 원형 화면과 직사각형 화면에 맞춰 스스로 배치됩니다. 위의 그림은
`esp32/simulator`의 시뮬레이터에서 `src/sim_board.c`의 화면 크기를 보드의
크기로 바꿔 그린 것입니다. 보드를 손에 넣기 전에 새 크기에서 어떻게 보이는지
확인할 수 있습니다. 보드 정보를 모으는 일부터 결과를 확인하는 일까지의 전체
절차는 [`esp32/devices/AGENTS.md`](esp32/devices/AGENTS.md)(영어)에 있습니다.

## SDK의 나머지 부분

| | |
|---|---|
| [**ESP32 Device SDK**](esp32) | ESP32 보드를 Muse에 연결합니다. 화면을 달아 이미지를 보여 주거나, 오디오 입출력을 추가하거나, 다른 센서를 연결할 수 있습니다. |
| [**Linux Device SDK**](linux) | 남는 Raspberry Pi나 Linux 컴퓨터를 Muse gadget으로 만듭니다. 직접 만든 명령을 넣어 시스템 관리 잡무나 Home Assistant 설정을 Muse에게 맡길 수 있습니다. |

ESP32와 Linux gadget은 iOS와 Android의 Muse 앱에서 Settings > Devices를 통해
페어링합니다. 먼저 거기서 Developer mode를 켠 다음, 이름이 "MuseGadget"으로
시작하는 기기를 찾으세요. 각 디렉터리에는 시작 방법을 담은 `README.md`와 코딩
에이전트를 위한 `AGENTS.md`가 있습니다(둘 다 영어).

## 커뮤니티

업스트림 프로젝트의 커뮤니티는 [Discord](https://discord.gg/3bhjCkZdd6)에
있습니다. 이 포크에서 추가한 보드에 관한 내용은 이 저장소에 이슈를 등록해
주세요.

## 라이선스와 크레딧

- Muse Gadget SDK의 저작권은 Meta Platforms, Inc. 및 그 계열사에 있으며,
  Apache License, Version 2.0으로 제공됩니다. [`LICENSE`](LICENSE)를
  참고하세요.
- 이 포크의 변경 사항은 ledienbien-ai(2026)에게 저작권이 있으며, 같은
  라이선스로 제공됩니다.
- 1.85C 포팅은 Waveshare의 문서와
  [예제 코드](https://github.com/waveshareteam/ESP32-S3-Touch-LCD-1.85C)
  (Apache-2.0), 그리고 [xiaozhi-esp32](https://github.com/78/xiaozhi-esp32)의
  해당 보드 코드(MIT)를 참고했습니다.
- OSTB-3ST 포팅은 핀, 패널 초기화 표, 배터리 표를 이 보드의 xiaozhi-esp32
  펌웨어 소스에서 가져왔습니다. 그 파일 자체에는 라이선스 표기가 없습니다.
  xiaozhi-esp32는 MIT입니다.
- LCDWIKI 2.8inch 포팅은 [LCDWIKI의 페이지](https://www.lcdwiki.com/2.8inch_ESP32-S3_Display), 이 보드의 xiaozhi-esp32
  파일, 그리고 [esphome-es3c28p-light-panel](https://github.com/jvduuren/esphome-es3c28p-light-panel)에 공개된 실측 결과를
  참고했습니다. 코드는 복사하지 않았습니다.
- 업스트림 파일 중 두 개는 자체 라이선스를 유지합니다: `minimp3.h`(CC0-1.0)와
  `pixel_font.c`(BSD-2-Clause). 빌드할 때 내려받는 컴포넌트에는 각자의
  라이선스가 적용됩니다.
- DB_ROBOT 시작 로고는 ledienbien-ai의 것이며, 이 포크의 다른 변경 사항과 같은
  라이선스로 자유롭게 쓸 수 있습니다.
- Apache License는 [Jollybot 아바타](esp32/avatar)에 적용되지 않으며, Meta,
  Muse, Waveshare의 이름과 상표에도 적용되지 않습니다.

전체 목록은 [`CREDITS.md`](CREDITS.md)(영어)에 있습니다. 각 출처와 그
라이선스, 이 포크에서 추가하거나 변경한 파일, 이후 버전에서 지켜야 할 사항을
담고 있습니다.
