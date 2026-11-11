# M5Stack_PPS_Try

M5Stack CoreS3 (SE) で M5Stack PPS モジュールを制御するファームウェア（Arduino）。

- 画面タッチと Web (SoftAP / STA) から電圧・電流・出力 ON/OFF を操作
- Fan Module v1.1 を PPS 温度で自動制御
- M5GO Battery Bottom3 の RGB LED で温度状態を表示
- PPS 温度が 60℃ に達すると出力を OFF にして固定（50℃ 未満に下がるまで ON 不可。自動では再開しない）
- STA 接続中は、AP につないだ機器を STA 側へ NAT で中継

## ビルド環境

Arduino 公式の [Arduino CLI](https://arduino.github.io/arduino-cli/) と、Espressif 公式の
Arduino-ESP32 コアを使う。コア・ライブラリの版は `src/sketch.yaml`（プロファイル `cores3`）で固定しており、
初回ビルド時に自動でダウンロードされる（ユーザーのスケッチブックのライブラリは使わない）。

| 項目 | 版 |
|---|---|
| Arduino-ESP32 コア (`esp32:esp32`) | 3.3.11 |
| M5Unified / M5GFX | 0.2.28 / 0.2.35 |
| Adafruit NeoPixel | 1.15.5 |

## セットアップ

1. Arduino CLI をインストールする
2. `src/secrets.example.h` を `src/secrets.h` にコピーし、Wi-Fi の SSID / パスワードを設定する
   （`secrets.h` は git 管理外。無い場合はビルドエラーになる。CI のビルド確認だけは
   `-DALLOW_PLACEHOLDER_SECRETS` を明示してテンプレートの値でコンパイルする）
3. ビルドと書き込み（Windows / PowerShell）:

   ```powershell
   .\scripts\build.ps1 -Upload -Port COM6
   ```

   ほかの OS では次のコマンドで同じことができる:

   ```sh
   arduino-cli compile --profile cores3 src
   arduino-cli upload  --profile cores3 -p <port> src
   ```

初回はツールチェーン（約 400MB）のダウンロードがあり、既定のタイムアウトで失敗することがある。
その場合は環境変数 `ARDUINO_NETWORK_CONNECTION_TIMEOUT=3600s` を設定する（`build.ps1` は自動で設定する）。

既定では SoftAP `M5S3-PPS` を起動し、`http://192.168.4.1/` から操作できる。

> 注意: Web API には認証がない。信頼できるネットワークでのみ使用すること。
