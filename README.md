# M5Stack_PPS_Try

M5Stack CoreS3 (SE) で M5Stack PPS モジュールを制御する PlatformIO プロジェクト。

- 画面タッチと Web (SoftAP) から電圧・電流・出力 ON/OFF を操作
- Fan Module v1.1 を PPS 温度で自動制御
- M5GO Battery Bottom3 の RGB LED で温度状態を表示

## セットアップ

1. `src/secrets.example.h` を `src/secrets.h` にコピーし、Wi-Fi の SSID / パスワードを設定する
   （`secrets.h` は git 管理外。無い場合はビルドエラーになる。CI のビルド確認だけは
   `-DALLOW_PLACEHOLDER_SECRETS` を明示してテンプレートの値でコンパイルする）
2. `pio run -t upload` で書き込み

既定では SoftAP `M5S3-PPS` を起動し、`http://192.168.4.1/` から操作できる。

> 注意: Web API には認証がない。信頼できるネットワークでのみ使用すること。
