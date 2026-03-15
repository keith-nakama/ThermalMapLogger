# UML仕様書 Thermal Mapper v13.1

**対象コード:** `06_ThermalMapLogger.ino`  
**作成者:** なかま合同会社 データロガー開発  
**作成日:** 2026年3月5日

---

## 第1章 システム概要・ハードウェア構成

本システムはESP32 WROVERをWi-Fiアクセスポイントとして動作させ、AMG8833から取得した温度データをスマホブラウザにリアルタイム配信しながら、SDカードにCSVとして記録するシステムです。外部ルーター不要でフォーミュラカーのコース・ピット上でも単独動作します。

```mermaid
flowchart LR
    subgraph Client["クライアント"]
        Phone["スマホ ブラウザ"]
    end
    subgraph Core["メインシステム"]
        ESP32["ESP32 WROVER"]
    end
    subgraph Hardware["外部デバイス"]
        AMG["AMG8833 センサー"]
        SD["SDカード"]
    end
    
    Phone <-->|"Wi-Fi AP"| ESP32
    ESP32 <-->|"I2C"| AMG
    ESP32 <-->|"SPI"| SD
```

---

## 第2章 ユースケース図

ユーザー（スマホブラウザ）とESP32（システム）の2アクターを定義し、システムが提供する機能とアクターの関係を示します。

```mermaid
flowchart LR
    User["スマホ ブラウザ"]
    Sys["ESP32 システム"]

    UC1["UC01 WebUI表示"]
    UC2["UC02 時刻同期"]
    UC3["UC03 リアルタイム表示"]
    UC4["UC04 記録開始"]
    UC5["UC05 記録停止"]
    UC6["UC06 ファイル一覧取得"]
    UC7["UC07 CSVダウンロード"]
    UC11["UC11 ファイル削除"]

    UC8["UC08 センサー読取"]
    UC9["UC09 JSON配信"]
    UC10["UC10 CSV追記"]

    %% ユーザーとシステムの両方に関わるユースケース
    User --- UC1 --- Sys
    User --- UC2 --- Sys
    User --- UC3 --- Sys
    User --- UC4 --- Sys
    User --- UC5 --- Sys
    User --- UC6 --- Sys
    User --- UC7 --- Sys
    User --- UC11 --- Sys

    %% システム内部で完結するユースケース
    Sys --- UC8
    Sys --- UC9
    Sys --- UC10
```

---

## 第3章 クラス図

主要なコンポーネントと役割を定義します。

```mermaid
classDiagram
    class AppState {
        -bool isLogging
        -String currentLogFile
        -int amg_addr
    }
    
    class SensorModule {
        -Adafruit_AMG88xx amg
        +begin() bool
        +readPixels() float
    }
    
    class StorageModule {
        -File logFile
        +saveToSD() void
        +closeLog() void 
    }
    
    class NetworkModule {
        -WebServer server
        +handleData() void
        +sendJSON() void
    }
    
    AppState --> SensorModule : uses
    AppState --> StorageModule : uses
    AppState --> NetworkModule : manages
    NetworkModule --> SensorModule : calls
    NetworkModule --> StorageModule : calls
```

> **【クラス図における課題事項】**
> * **AppState:** toggle失敗時フラグ未処理
> * **SensorModule:** 初期化失敗時無限ループ停止の改善
> * **StorageModule:** SDカードのopenとcloseの毎回実行
> * **NetworkModule:** send前SD書込の順序問題

---

## 第4章 シーケンス図

### 4-A: 起動フェーズ
```mermaid
sequenceDiagram
    participant Sys
    participant AMG
    participant SD

    Sys->>AMG: アドレス0x68で接続試行
    AMG-->>Sys: 失敗を返却
    Sys->>AMG: アドレス0x69で接続試行
    AMG-->>Sys: 成功を返却
    Note right of Sys: 課題 両方失敗時は無限ループ停止
    Sys->>SD: SDカード初期化
```

### 4-B: リアルタイム表示フェーズ
```mermaid
sequenceDiagram
    participant Browser
    participant Server
    participant Sensor
    participant SD

    Browser->>Server: データ要求
    Server->>Sensor: 温度読取
    Sensor-->>Server: 温度データ
    Note over Server, SD: 改善提案 SD保存を先に実行
    Server->>SD: SDカードへ保存
    Server->>Browser: JSON応答
```

---

## 第5章 アクティビティ図 — handleData

```mermaid
flowchart TD
    Start["開始"] --> Read["温度読取"]
    Read --> Check["記録中か判定"]
    Check -->|Yes| Save["課題 SD保存"]
    Check -->|No| GenJSON["課題 JSON文字列生成"]
    Save --> GenJSON
    GenJSON --> Send["JSON配信"]
    Send --> End["終了"]
```

---

## 第6章 コンポーネント図

```mermaid
flowchart TB
    UI["ブラウザ UI"] --- Web["WebServer Lib"]
    Web --- App["Application"]
    App --- SDLib["SD Lib"]
    App --- AMGLib["AMG88xx Lib"]
    SDLib --- SDCard["SD Card"]
    AMGLib --- AMG["AMG8833"]
```

---

## 第7章 HTTP APIエンドポイント仕様

> **【重要】** deleteのパス検証なし・toggleの失敗時未処理・syncのelseなし・listのroot_close漏れは今すぐ修正が必要です。

| エンドポイント | メソッド | 処理内容 |
| :--- | :--- | :--- |
| `/` | GET | WebUIを返す |
| `/data` | GET | 温度データをJSONで返す（SD記録も実行） |
| `/sync` | GET | ブラウザ時刻を受け取りシステム時刻を更新 |
| `/toggle` | GET | ロギングの開始や停止を切り替える |
| `/list` | GET | SD内のファイル一覧をJSONで返す |
| `/download` | GET | 指定されたCSVファイルをダウンロード |
| `/delete` | GET | 指定されたファイルを削除 |

---
