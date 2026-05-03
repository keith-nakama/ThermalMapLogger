# ThermalMapLogger

ESP32をWi-Fiアクセスポイントとして動作させ、AMG8833赤外線サーマルセンサーから取得した8×8の温度データをスマホブラウザでリアルタイム確認しながら、SDカードにCSV形式で記録するスタンドアロンシステムです。外部ルーター不要で現場でも単独動作します。

---

## システム構成

```
[スマホ / PC ブラウザ]
        |
        | Wi-Fi (APモード)
        |
[ESP32 WROVER] ── I2C ── [AMG8833 赤外線センサー]
        |
       SPI
        |
  [SDカード]
```

外部ルーター不要。ESP32単体でアクセスポイントを立ち上げてスタンドアロン動作します。

---

## 必要なハードウェア

| パーツ | 備考 |
|--------|------|
| ESP32 WROVER | デュアルコア 240MHz |
| AMG8833 | 8×8 赤外線サーマルセンサー |
| microSDカード | FAT32フォーマット |

### ピン接続

| AMG8833 | ESP32 | 備考 |
|---------|-------|------|
| VCC | 3.3V | 専用電源 |
| GND | GND | — |
| SDA | GPIO21 | I2C |
| SCL | GPIO22 | I2C |
| AD0 | GND | I2Cアドレス 0x68 (3.3Vで0x69) |

| SDカード | ESP32 | 備考 |
|---------|-------|------|
| CMD | GPIO15 | SD_MMC |
| CLK | GPIO14 | SD_MMC |
| D0 | GPIO2 | SD_MMC 1bitモード |
| GND | GND | — |

---

## 必要なライブラリ

Arduino IDEのライブラリマネージャーからインストールしてください。

- [Adafruit AMG88xx Library](https://github.com/adafruit/Adafruit_AMG88xx)
- Adafruit BusIO（上記の依存ライブラリ・自動インストール）

---

## セットアップ手順

**1. ライブラリをインストール**

Arduino IDE → ツール → ライブラリを管理 → `Adafruit AMG88xx` を検索してインストール。

**2. Wi-Fi設定を変更（任意）**

`ThermalMapLogger.ino` の冒頭にあるSSIDとパスワードを変更する場合は以下を編集してください。

```cpp
const char *ssid     = "ESP32-Thermal-Monitor"; // 任意のSSID
const char *password = "88888888";              // 8文字以上
```

**3. ESP32に書き込む**

Arduino IDEでボードを `ESP32 Wrover Module` に設定して書き込みます。

**4. 接続する**

スマホまたはPCのWi-Fi設定から `ESP32-Thermal-Monitor` に接続し、ブラウザで以下にアクセスします。

```
http://192.168.4.1
```

---

## Wokwiシミュレーション

リポジトリ直下の `diagram.json` と `libraries.txt` を使うと、Wokwi 上で ESP32、AMG8833相当のI2Cカスタムチップ、microSDカードを含む構成を開けます。

### Web版Wokwi

1. https://wokwi.com/ で新規 ESP32 プロジェクトを作成、または GitHub 連携でこのリポジトリを開きます
2. `diagram.json`、`libraries.txt`、`amg8833-sim.chip.json`、`amg8833-sim.chip.c` をプロジェクトに含めます
3. シリアルモニタで `HTTP server started. Connect to: http://192.168.4.1` が出ることを確認します

### VS Code版Wokwi

VS Code では `Wokwi Embedded Simulator` 拡張を使います。拡張 ID は `wokwi.wokwi-vscode` です。

1. VS Code の拡張機能で `Wokwi Embedded Simulator` をインストールします
2. コマンドパレットから `Wokwi: Request a new License` を実行してライセンスを有効化します
3. Arduino CLI などで `WOKWI_SIMULATION` を有効にした `build-wokwi/ThermalMapLogger.ino.merged.bin` と `build-wokwi/ThermalMapLogger.ino.elf` を生成します
4. `diagram.json` を開いた状態で、画面内の緑色の再生ボタンではなく、コマンドパレットから `Wokwi: Start Simulator` を実行します
5. `Wokwi Terminal` で `ThermalMapLogger booting...`、`Wokwi WiFi: Connected.`、`HTTP server started. Connect to: http://localhost:8180` が出ることを確認します
6. PC のブラウザで `http://localhost:8180` を開きます

Arduino CLI で生成する場合は、以下をリポジトリ直下で実行します。

```powershell
arduino-cli compile --clean --fqbn esp32:esp32:esp32 --build-property compiler.cpp.extra_flags=-DWOKWI_SIMULATION --output-dir build-wokwi .
```

AMG8833相当のカスタムチップを更新した場合は、Wokwi CLI で WASM を再生成します。

```powershell
wokwi-cli chip compile amg8833-sim.chip.c -o amg8833-sim.chip.wasm
```

このリポジトリでは VS Code 版 Wokwi 用に `wokwi.toml` を追加し、`firmware` に `build-wokwi/ThermalMapLogger.ino.merged.bin`、`elf` に `build-wokwi/ThermalMapLogger.ino.elf` を指定しています。`build-wokwi/` は生成物のため Git 管理外です。ESP32 の Web UI は Wokwi 起動後に `http://localhost:8180` へ転送されます。シリアル出力は VS Code の `PORT` 設定ではなく、Wokwi 拡張が作成する `Wokwi Terminal` に表示されます。
Wokwi の `net.forward` は ESP32 が `Wokwi-GUEST` へ STA 接続した後に有効になります。実機ビルドでは SoftAP の `ESP32-Thermal-Monitor` を使い、Wokwi 用ビルドでは `WOKWI_SIMULATION` により `Wokwi-GUEST` へ接続します。
`Wokwi Terminal` に何も出ない場合は、`diagram.json` の `connections` に `esp:TX` から `$serialMonitor:RX`、`esp:RX` から `$serialMonitor:TX` への接続があることを確認してください。

実機では SD_MMC を優先します。Wokwi の microSD は SPI 接続のため、SD_MMC 初期化に失敗した場合だけ GPIO18/19/23/5 の SPI SD にフォールバックします。
ブラウザから ESP32 内の HTTP サーバーへ接続する確認には、Wokwi IoT Gateway が必要になる場合があります。

---

## 使い方

### リアルタイム表示

接続後、自動的に8×8のヒートマップが0.5秒ごとに更新されます。温度が低いほど青、高いほど赤で表示されます。

### バージョン確認

画面上部のアプリタイトル右側に `134`、その下に `v13.4 / Build 134` が表示されます。`About` ボタンでも同じバージョンとビルド番号を確認できます。

ブラウザまたは `curl` で以下にアクセスすると、JSONでも確認できます。

```
http://192.168.4.1/version
```

Wokwi の VS Code 版で `net.forward` を使っている場合は、以下で確認します。

```
http://localhost:8180/version
```

### CSV記録

1. **「Start Logging」** ボタンを押すと記録開始
2. SDカードに `YYYYMMDD_HHMMSS.csv` 形式でファイルが作成されます
3. **「Stop Logging」** ボタンで記録停止
4. 画面下部の **「Saved Logs」** 一覧からダウンロード・削除が可能です

### 時刻同期

ページ読み込み時にブラウザの時刻をESP32に自動送信します。NTPサーバー不要でJST時刻が設定されます。

---

## CSVフォーマット

```
datetime, 11, 12, 13, 14, 15, 16, 17, 18, 21, ... , 88
2025/01/01 12:00:00.05, 22.50, 23.00, ...
```

| 列名 | 説明 |
|------|------|
| datetime | 計測日時（JST / センチ秒精度） |
| 11〜88 | 画素番号（行番号+列番号）。11=左上、88=右下 |

---

## HTTPエンドポイント

| エンドポイント | メソッド | 説明 |
|--------------|---------|------|
| `/` | GET | Web UI を返す |
| `/data` | GET | 温度データをJSON配列で返す |
| `/sync` | GET | ブラウザ時刻をESP32に同期 |
| `/toggle` | GET | ロギングの開始・停止を切り替え |
| `/list` | GET | SDカード内のCSVファイル一覧を返す |
| `/download` | GET | 指定CSVファイルをダウンロード |
| `/delete` | GET | 指定CSVファイルを削除 |
| `/version` | GET | アプリ名・バージョン・ビルド番号をJSONで返す |

---

## バージョン履歴

| バージョン | 内容 |
|-----------|------|
| v13.4 | バージョン確認表示・About表示・/versionエンドポイントを追加 |
| v13.3 | バグ修正（Issue #8対応） |
| v13.2 | バグ修正・安定性改善（Issue #1〜#6 全対応） |
| v13.1 | 初版リリース |

### v13.4 変更詳細

- 画面上部に `v13.4 / Build 134` を常時表示
- `About` ボタンでバージョン・ビルド番号・確認用エンドポイントを表示
- `/version` エンドポイントで `name`、`version`、`build` をJSON返却

### v13.3 変更詳細

- ブラウザ切断時もSD記録を継続（loop()ベースのサンプリングに変更）（[#8](https://github.com/keith-nakama/ThermalMapLogger/issues/8)）
- 1970年ファイル名問題を修正（millis()差分方式で時刻管理）（[#8](https://github.com/keith-nakama/ThermalMapLogger/issues/8)）
- 時刻未同期時のロギング開始を拒否（503エラーを返す）（[#8](https://github.com/keith-nakama/ThermalMapLogger/issues/8)）
- ブラウザ側の時刻同期失敗時にエラーメッセージを表示（[#8](https://github.com/keith-nakama/ThermalMapLogger/issues/8)）

### v13.2 変更詳細

- SDカード書込最適化・毎回open/closeを廃止しflush方式に変更（[#1](https://github.com/keith-nakama/ThermalMapLogger/issues/1)）
- 電源断対策として1分に1回close→openでFATテーブルを確定（[#1](https://github.com/keith-nakama/ThermalMapLogger/issues/1)）
- データ配信前にSD書込を実行（レスポンス改善）（[#2](https://github.com/keith-nakama/ThermalMapLogger/issues/2)）
- /deleteエンドポイントのセキュリティ強化（.csvのみ許可）（[#3](https://github.com/keith-nakama/ThermalMapLogger/issues/3)）
- toggle失敗時にisLogging=falseへ安全復帰（[#4](https://github.com/keith-nakama/ThermalMapLogger/issues/4)）
- センサー/SD初期化失敗時のリトライ処理を追加（3回失敗でESP.restart()）（[#5](https://github.com/keith-nakama/ThermalMapLogger/issues/5)）
- /listエンドポイントのディレクトリクローズ漏れを修正（[#6](https://github.com/keith-nakama/ThermalMapLogger/issues/6)）

---

## ライセンス

Copyright (c) 2026 なかま合同会社
