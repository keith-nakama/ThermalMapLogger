/**
 * ============================================================
 *  06_ThermalMapLogger.ino
 *  Thermal Mapper v13.4 / Build 142
 * ============================================================
 *
 *  【概要】
 *  ESP32がWiFiアクセスポイントとして動作し、ブラウザから
 *  AMG8833サーマルカメラのリアルタイム映像を確認しながら
 *  SDカードにCSVログを記録するスタンドアロンシステム。
 *
 *  【システム構成】
 *
 *    [スマホ/PC] ─── WiFi (AP mode) ─── [ESP32 WROVER]
 *                                              │
 *                                    ┌─────────┴──────────┐
 *                                 [AMG8833]           [SDカード]
 *                              8x8サーマルセンサー    CSVログ保存
 *
 *  【動作フロー】
 *    1. ESP32がWiFiアクセスポイントを起動 (SSID: ESP32-Thermal-Monitor)
 *    2. ブラウザで http://192.168.4.1 にアクセス
 *    3. 0.5秒ごとにAMG8833から64画素を取得してヒートマップ表示
 *    4. "Start Logging"ボタンでSDカードへのCSV記録を開始/停止
 *    5. 記録したCSVファイルはブラウザからダウンロード/削除可能
 *
 *  【ハードウェア接続】
 *    AMG8833 VCC  → ESP32 3.3V
 *    AMG8833 GND  → ESP32 GND
 *    AMG8833 SDA  → ESP32 GPIO21
 *    AMG8833 SCL  → ESP32 GPIO22
 *    AMG8833 AD0  → GND (0x68) または 3.3V (0x69)
 *    SDカード     → SD_MMC (GPIO2/4/12/13/14/15)
 *
 *  【必要ライブラリ】
 *    - Adafruit AMG88xx Library
 *    - Adafruit BusIO (依存)
 *
 *  【CSVフォーマット】
 *    datetime, 11, 12, 13, ... 88   ← ヘッダー行 (行列番号)
 *    2025/01/01 12:00:00.05, 22.50, 23.00, ...  ← データ行
 * ============================================================
 */

// ============================================================
//  インクルード
// ============================================================
#include <Arduino.h>
#ifndef BitOrder
typedef uint8_t BitOrder; // ESP32コア3.xでのAdafruit_BusIOビルドエラー対策
#endif
#include <WiFi.h>           // WiFi アクセスポイント機能
#include <WebServer.h>      // HTTP サーバー機能
#include <Wire.h>           // I2C 通信 (AMG8833との接続)
#include <Adafruit_AMG88xx.h> // AMG8833 サーマルセンサードライバ
#include "FS.h"             // ファイルシステム基底クラス
#include "SD_MMC.h"         // SDカード (MMCバス) アクセス
#include "SD.h"             // Wokwi/予備用 SPI SDカードアクセス
#include "SPI.h"            // SPI SDカード接続
#include <time.h>           // time(), localtime_r() などの時刻関数
#include <sys/time.h>       // gettimeofday(), settimeofday() (マイクロ秒精度)

// ============================================================
//  WiFi アクセスポイント設定
//  ESP32自身がAPとして動作するため、外部WiFiルーターは不要。
//  接続後は http://192.168.4.1 でWebUIにアクセスする。
// ============================================================
const char *ssid     = "ESP32-Thermal-Monitor"; // APのSSID
const char *password = "88888888";              // APのパスワード(8文字以上)
#ifdef WOKWI_SIMULATION
const char *wokwiSsid = "Wokwi-GUEST";          // Wokwi仮想Wi-Fi。パスワードなし、ch 6
#endif

const char *APP_NAME    = "Thermal Mapper";
const char *APP_VERSION = "v13.4";
const char *APP_BUILD   = "Build 142";

// ============================================================
//  AMG8833 センサーオブジェクト
//  I2Cアドレスはボードの AD0 ピンで切替: GND=0x68 / 3.3V=0x69
//  setup()内で0x68→0x69の順に自動検出する。
// ============================================================
Adafruit_AMG88xx amg;
uint8_t       amg_addr        = 0x68;  // 実際に接続されていたアドレスを保持(デバッグ用)
bool          sdAvailable     = false; // SDカードが使用可能かどうか
fs::FS       *storageFs       = &SD_MMC; // 通常はSD_MMC、WokwiではSPI SDにフォールバック
float         latestPixels[64] = {};   // loop()で更新される最新の温度データ
unsigned long lastSampleTime   = 0;    // 最後にサンプリングした時刻(ms)
// settimeofday()がこのESP32ビルドで機能しないため、millis()差分で時刻を自前管理する
bool          g_timeSynced    = false; // /sync を受信済みかどうか
long long     g_syncEpochMs   = 0;    // /sync 受信時のUnixタイムスタンプ(ms)
unsigned long g_syncMillis    = 0;    // /sync 受信時のmillis()

// ============================================================
//  HTTP サーバー (ポート80)
//  以下のエンドポイントを提供する:
//    /        → WebUI (HTML)
//    /data    → 64画素JSONデータ取得 + SDへの記録
//    /sync    → ブラウザからESP32へ時刻同期
//    /toggle  → ログ記録の開始/停止
//    /list    → SDカード内CSVファイル一覧
//    /download→ CSVファイルのダウンロード
//    /delete  → CSVファイルの削除
//    /version → アプリ名・バージョン・ビルド番号
// ============================================================
WebServer server(80);

// ============================================================
//  ログ管理変数
//  currentLogFile: 現在書き込み中のCSVファイルパス (例: /20250101_120000.csv)
//  isLogging     : ログ記録中かどうかのフラグ
// ============================================================
String currentLogFile = "";  // 空文字 = ファイル未作成
bool isLogging = false;      // false = 停止中、true = 記録中
int flushCount = 0;          // FAT確定用の書き込みカウンタ (Issue #12)
File logFile;                // ログファイルを開きっぱなしで保持 (Issue #1対応)
const int SPI_SD_CS_PIN = 5;  // Wokwi microSD(SPI)用CS。実機SD_MMC成功時は使用しない。

// ============================================================
//  Web UI (HTML/CSS/JavaScript)
//  PROGMEM修飾子でフラッシュメモリに格納 (RAMを節約)
//  R"=====(...)====="はC++生文字列リテラル(エスケープ不要)
//
//  【JavaScript の動作】
//    initApp()   : ページ読み込み時に時刻同期→ファイル一覧取得→0.5秒ごとの
//                  センサー更新を開始
//    updateData(): /data にアクセスして64画素取得→8x8グリッドに描画
//    getHeatColor(): 温度値をRGB色に変換 (0°C=青系、80°C以上=赤系)
//    toggleLogging(): /toggle を叩いてロギングON/OFFを切り替え
//    updateList(): /list からCSVファイル一覧を取得して表示
//    deleteFile(): /delete を叩いてファイル削除後に一覧を更新
// ============================================================
const char INDEX_HTML[] PROGMEM = R"=====(
<!DOCTYPE html><html><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>Thermal Mapper v13.4 - Build 142</title>
<style>
  body { font-family: sans-serif; text-align: center; background: #121212; color: #eee; margin:0; padding:8px; }
  h3 { color: #4db6ac; margin: 4px 0 8px; }
  .app-meta { display: flex; justify-content: center; align-items: center; gap: 8px; color: #aaa; font-size: 12px; margin-bottom: 6px; }
  /* 8x8グリッド: 横幅と高さの小さい方に合わせ、Wokwi/スマホ画面内に収める */
  #grid { display: grid; grid-template-columns: repeat(8, 1fr); width: min(92vw, 56vh, 450px); height: min(92vw, 56vh, 450px); margin: 6px auto; gap: 1px; background: #333; }
  .cell { display: flex; align-items: center; justify-content: center; font-size: 10px; font-weight: bold; text-shadow: 1px 1px 2px #000; }
  .controls { margin: 10px 0 6px; }
  button { padding: 12px 24px; font-size: 16px; border: none; border-radius: 8px; cursor: pointer; font-weight: bold; }
  #logBtn { background: #00897b; color: white; }
  #status { color: #888; min-height: 1.2em; }
  #fileList { margin-top: 10px; text-align: left; max-width: 480px; margin-left: auto; margin-right: auto; background: #222; padding: 10px; border-radius: 8px; max-height: 22vh; overflow-y: auto; }
  #fileList h4 { margin: 0 0 6px; }
  .file-item { display: flex; justify-content: space-between; align-items: center; gap: 8px; padding: 7px 0; border-bottom: 1px solid #333; }
  .file-item a { overflow-wrap: anywhere; }
  a { color: #8bc34a; text-decoration: none; font-family: monospace; }
  .del-btn { background: #444; color: #ff5252; padding: 5px 10px; border: none; border-radius: 4px; font-size: 12px; }
  #aboutBtn { background: #333; color: #ddd; padding: 4px 8px; border-radius: 4px; font-size: 12px; }
  #aboutDialog { border: 1px solid #444; border-radius: 8px; background: #1b1b1b; color: #eee; padding: 14px; min-width: min(84vw, 320px); }
  #aboutDialog::backdrop { background: rgba(0, 0, 0, 0.55); }
  #aboutDialog h4 { margin: 0 0 10px; color: #4db6ac; }
  #aboutDialog dl { display: grid; grid-template-columns: auto 1fr; gap: 6px 12px; text-align: left; margin: 0 0 12px; }
  #aboutDialog dt { color: #aaa; }
  #aboutDialog dd { margin: 0; font-family: monospace; }
  #aboutDialog button { background: #00897b; color: white; padding: 8px 14px; border-radius: 6px; font-size: 14px; }
</style>
</head>
<body onload="initApp()">
  <h3>Thermal Mapper [Grid: 11-88] 142</h3>
  <div class="app-meta"><span>v13.4 / Build 142</span><button id="aboutBtn" onclick="showAbout()">About</button></div>
  <div id="grid"></div>
  <div class="controls"><button id="logBtn" onclick="toggleLogging()">Start Logging</button></div>
  <div id="status">Syncing Time...</div>
  <div id="fileList"><h4>Saved Logs</h4><div id="listContent"></div></div>
  <dialog id="aboutDialog">
    <h4>Thermal Mapper</h4>
    <dl>
      <dt>Version</dt><dd>v13.4</dd>
      <dt>Build</dt><dd>Build 142</dd>
      <dt>HTTP</dt><dd>/version</dd>
    </dl>
    <button onclick="closeAbout()">Close</button>
  </dialog>
<script>
  /**
   * 温度値をヒートマップ色(RGB)に変換する
   * @param {number} t - 温度(°C)
   * @returns {string} CSS rgb()文字列
   *
   * 色のマッピング例:
   *   0°C以下  → 青
   *   40°C     → 中間色
   *   80°C以上 → 赤
   */
  function getHeatColor(t) {
    const ratio = Math.max(0, Math.min(1, t / 80));
    const r = Math.round(ratio * 255);
    const g = Math.round(80 - Math.abs(ratio - 0.5) * 80);
    const b = Math.round((1 - ratio) * 255);
    return `rgb(${r}, ${g}, ${b})`;
  }

  /**
   * アプリ初期化 (ページ読み込み時に自動呼び出し)
   * 1. ESP32に現在のブラウザ時刻(UNIXミリ秒)を送信して時刻同期
   * 2. SDカードのファイル一覧を取得
   * 3. 0.5秒ごとのセンサー更新タイマーを開始
   */
  async function initApp() {
    // ブラウザのUNIXタイムをクエリパラメータで送信 → /sync ハンドラで受信
    try {
      await fetch(`/sync?now=${Date.now()}&t=${Date.now()}`);
      document.getElementById('status').innerText = "JST Time Synced";
    } catch(e) {
      document.getElementById('status').innerText = "Time sync failed. Reload the page.";
    }
    updateList();
    setInterval(updateData, 500); // 500ms = 2fps でセンサーデータを更新
  }

  /**
   * センサーデータを取得して8x8グリッドを更新
   * /data エンドポイントから64要素のJSON配列を受け取り
   * 各セルの背景色と温度値テキストを更新する
   */
  async function updateData() {
    try {
      const res = await fetch('/data?t=' + Date.now()); // キャッシュ防止のため t= を付与
      const pixels = await res.json(); // [22.50, 23.00, ...] 64要素の配列
      const grid = document.getElementById('grid');
      grid.innerHTML = ''; // 前フレームをクリア
      pixels.forEach(t => {
        const c = document.createElement('div');
        c.className = 'cell';
        c.style.backgroundColor = getHeatColor(t); // 温度→色変換
        c.innerText = t.toFixed(1);                // 小数1桁で表示
        grid.appendChild(c);
      });
    } catch(e) {} // 通信エラー時は静かにスキップ(表示を維持)
  }

  /**
   * ロギングのON/OFFを切り替える
   * /toggle を叩くと ESP32側でisLogging フラグが反転し
   * "Recording" または "Stopped" が返ってくる
   */
  async function toggleLogging() {
    const btn = document.getElementById('logBtn');
    if (btn.disabled) return;
    btn.disabled = true; // 二重送信防止
    try {
      const res = await fetch('/toggle?t=' + Date.now());
      const state = await res.text(); // "Recording" or "Stopped"
      // ボタンのラベルと色をロギング状態に合わせて切り替え
      btn.innerText = state.includes("Recording") ? "Stop Logging" : "Start Logging";
      btn.style.background = state.includes("Recording") ? "#555" : "#00897b";
      updateList(); // 新しいファイルが作成されたので一覧を更新
    } catch(e) {
      alert("Failed to toggle logging");
    } finally {
      btn.disabled = false;
    }
  }

  /**
   * SDカード内のCSVファイル一覧を取得して表示
   * ファイルは新しい順(reverse)で並べる
   * 各ファイルにダウンロードリンクと削除ボタンを付与
   */
  async function updateList() {
    try {
      const res = await fetch('/list?t=' + Date.now());
      if (!res.ok) throw new Error();
      const files = await res.json(); // ["20250101_120000.csv", ...] の配列
      const cont = document.getElementById('listContent');
      cont.innerHTML = '';
      files.reverse().forEach(f => { // 新しいファイルを先頭に
        cont.innerHTML += `<div class="file-item"><a href="/download?file=${f}" download>${f}</a><button class="del-btn" onclick="deleteFile(this, '${f}')">Delete</button></div>`;
      });
    } catch(e) {
      document.getElementById('listContent').innerHTML = '<div style="color:#ff5252; font-size:12px;">SD not available</div>';
    }
  }

  /**
   * 指定ファイルを削除する
   * confirm()で確認後、/delete を叩いてファイルを消去
   * @param {HTMLElement} btn - 押されたボタン要素
   * @param {string} f - 削除するファイル名
   */
  async function deleteFile(btn, f) {
    if (btn.disabled) return;
    if(confirm(`Delete ${f}?`)) {
      btn.disabled = true; // 二重送信防止
      try {
        await fetch(`/delete?file=${f}&t=` + Date.now());
        updateList(); // 削除後に一覧を再取得
      } catch(e) {
        alert("Failed to delete file");
        btn.disabled = false;
      }
    }
  }

  function showAbout() {
    document.getElementById('aboutDialog').showModal();
  }

  function closeAbout() {
    document.getElementById('aboutDialog').close();
  }
</script></body></html>
)=====";

// ============================================================
//  SDカードへの記録処理
//
//  【CSVフォーマット (1行あたり)】
//  2025/01/01 12:00:00.05, 22.50, 23.00, 24.25, ... (64値)
//                 ↑センチ秒(1/100秒)
//
//  isLogging=false または logFileが未オープンのときは何もしない。
//  /data エンドポイントへのアクセスごとに呼び出される。
//
//  【Issue #1対応】
//  毎回open/closeするのではなく、handleToggle()でopenしたlogFileを
//  保持しっぱなしにして書き込みだけ行い、flush()でSDに確定させる。
//  これによりFATテーブルの書き換え回数を大幅に削減し、
//  SDカードの寿命と書き込み速度を改善する。
// ============================================================
// millis()差分で現在時刻を計算する。settimeofday()に依存しない。
void getSyncedTimeval(struct timeval* tv) {
  long long elapsed_ms = (long long)(millis() - g_syncMillis);
  long long current_ms = g_syncEpochMs + elapsed_ms;
  tv->tv_sec  = (time_t)(current_ms / 1000);
  tv->tv_usec = (suseconds_t)((current_ms % 1000) * 1000);
}

void saveToSD(float* pixels) {
  // ロギング無効またはファイルが開いていなければ即リターン
  if (!isLogging || !logFile) return;

  // millis()差分で現在時刻を取得 (settimeofday()に依存しない)
  struct timeval tv;
  getSyncedTimeval(&tv);       // tv.tv_sec = 秒, tv.tv_usec = マイクロ秒

  // UNIXタイムをローカル時刻(JST)に変換
  struct tm ti;
  localtime_r(&tv.tv_sec, &ti);

  // 日時文字列を組み立て (例: "2025/01/01 12:00:00.05")
  // centisec = マイクロ秒 / 10000 → 0〜99のセンチ秒
  char timeStr[64];
  int centisec = (int)(tv.tv_usec / 10000);
  snprintf(timeStr, sizeof(timeStr), "%04d/%02d/%02d %02d:%02d:%02d.%02d",
           ti.tm_year + 1900,  // tm_year は1900年からの経過年数
           ti.tm_mon + 1,      // tm_mon は0始まり(0=1月)
           ti.tm_mday,
           ti.tm_hour, ti.tm_min, ti.tm_sec,
           centisec);

  // "日時,画素0,画素1,...,画素63" の形式で書き込み
  logFile.print(timeStr);
  for(int i = 0; i < 64; i++) {
    logFile.print(",");
    logFile.print(pixels[i], 2); // 小数2桁(例: 22.50)
  }
  logFile.println(); // 行末の改行
  logFile.flush();   // closeせずにSDへ書き込みを確定 (open状態を維持)

  // 1分に1回だけclose→openしてFATテーブルを確定させる
  // 0.5秒ごとに呼ばれるので 120回 = 約60秒
  // 万が一の電源断時にも最大1分分のデータ損失で済むよう保護する
  if (++flushCount >= 120) {
    flushCount = 0;
    logFile.close();
    logFile = storageFs->open(currentLogFile.c_str(), FILE_APPEND);
    if (!logFile) {
      // 再openに失敗した場合はロギングを安全停止
      isLogging = false;
      Serial.println("SaveToSD Error: Failed to reopen log file. Logging stopped.");
    }
  }
}

// ============================================================
//  HTTPハンドラ: /data
//
//  latestPixels(loop()で更新)をJSON配列として返す。
//  センサー読取・SD保存はloop()が担うため、ここでは返すだけ。
//
//  レスポンス例: [22.50, 23.00, 24.25, ... ]  (64要素)
// ============================================================
void handleData() {
  // JSON配列を文字列として組み立て
  String json = "[";
  for(int i = 0; i < 64; i++){
    json += String(latestPixels[i], 2); // 小数2桁
    if(i < 63) json += ",";
  }
  json += "]";

  server.send(200, "application/json", json);
}

// ============================================================
//  HTTPハンドラ: /sync
//
//  ブラウザからUNIXタイムスタンプ(ミリ秒)を受け取り、
//  ESP32のシステム時刻を設定する。
//  NTPが使えないスタンドアロン環境でも時刻合わせができる。
//
//  クエリパラメータ:
//    now = UNIXタイムスタンプ (ミリ秒, JavaScriptのDate.now())
// ============================================================
void handleSync() {
  if (server.hasArg("now")) {
    long long now_ms = atoll(server.arg("now").c_str()); // ミリ秒をlong longで受け取る

    // timeval構造体に変換: tv_sec=秒, tv_usec=マイクロ秒
    struct timeval tv = {
      (time_t)(now_ms / 1000),              // 秒部分
      (suseconds_t)((now_ms % 1000) * 1000) // ミリ秒の余りをマイクロ秒に変換
    };
    settimeofday(&tv, NULL); // ESP32のシステム時刻を更新 (効かない場合は下記で補完)
    g_syncEpochMs = now_ms;  // millis()ベース時刻管理用に保存
    g_syncMillis  = millis();
    g_timeSynced  = true;
    server.send(200, "text/plain", "OK");
  } else {
    server.send(400, "text/plain", "Bad Request: missing 'now' parameter");
  }
}

// ============================================================
//  HTTPハンドラ: /toggle
//
//  ロギングのON/OFFを切り替える。
//  
//  OFF→ON時: タイムスタンプをファイル名にしたCSVを新規作成し
//            UTF-8 BOMとヘッダー行を書き込む。
//            【Issue #1対応】ファイルはcloseせずlogFileに保持する。
//            【Issue #4対応】open失敗時はisLoggingをfalseに戻す。
//  ON→OFF時: logFileをcloseしてisLoggingをfalseにする。
//
//  レスポンス: "Recording" または "Stopped"
//
//  CSVヘッダー例: datetime,11,12,13,...,88
//    行番号(1〜8) + 列番号(1〜8) の2桁数字で各画素を表す
//    例: 11=1行1列(左上), 18=1行8列(右上), 88=8行8列(右下)
// ============================================================
void handleToggle() {
  if (!sdAvailable) {
    server.send(503, "text/plain", "SD not available");
    return;
  }

  isLogging = !isLogging; // フラグを反転

  if(isLogging) {
    // 時刻未同期チェック: /sync が届いていない場合は1970年になるため拒否
    if (!g_timeSynced) {
      isLogging = false;
      server.send(503, "text/plain", "Time not synced");
      return;
    }

    // ロギング開始: 現在時刻でCSVファイルを作成 (millis()差分で取得)
    struct timeval tv;
    getSyncedTimeval(&tv);
    struct tm ti;
    localtime_r(&tv.tv_sec, &ti);

    // ファイル名を "YYYYMMDD_HHMMSS.csv" 形式で生成 (例: /20250101_120000.csv)
    char buf[32];
    strftime(buf, sizeof(buf), "/%Y%m%d_%H%M%S.csv", &ti);
    currentLogFile = String(buf);
    flushCount = 0; // カウンタリセット (Issue #12)

    // 新規ファイルを書き込みモードで作成し、グローバル変数logFileに保持
    logFile = storageFs->open(currentLogFile.c_str(), FILE_WRITE);
    if(logFile) {
      // UTF-8 BOM (0xEF 0xBB 0xBF) を先頭に書き込む
      // ExcelでCSVを開いたとき日本語が文字化けしないために必要
      const uint8_t bom[] = {0xEF, 0xBB, 0xBF};
      logFile.write(bom, sizeof(bom));

      // ヘッダー行を書き込む: "datetime,11,12,...,88"
      logFile.print("datetime");
      for(int i = 0; i < 64; i++) {
        int row = (i / 8) + 1; // 画素インデックスから行番号を計算 (1〜8)
        int col = (i % 8) + 1; // 画素インデックスから列番号を計算 (1〜8)
        logFile.printf(",%d%d", row, col); // 例: 11, 12, ..., 18, 21, ..., 88
      }
      logFile.println(); // ヘッダー行末の改行
      // closeしない → logFileを保持してsaveToSD()で使い続ける (Issue #1)
    } else {
      // ファイルオープン失敗 → フラグを元に戻してサイレント障害を防ぐ (Issue #4)
      isLogging = false;
      currentLogFile = "";
      Serial.println("Toggle Error: Failed to open log file.");
    }
  } else {
    // ロギング停止: ここで初めてファイルをclose
    if(logFile) {
      logFile.close();
    }
    currentLogFile = "";
  }

  // ロギング状態を文字列で返す
  server.send(200, "text/plain", isLogging ? "Recording" : "Stopped");
}

// ============================================================
//  HTTPハンドラ: /list
//
//  SDカードのルートディレクトリを走査し、
//  .csv 拡張子を持つファイルの名前一覧をJSON配列で返す。
//
//  レスポンス例: ["20250101_120000.csv","20250101_130000.csv"]
// ============================================================
void handleList() {
  if (!sdAvailable) {
    server.send(503, "text/plain", "SD not available");
    return;
  }
  String json = "[";
  File root = storageFs->open("/");      // ルートディレクトリを開く
  File file = root.openNextFile();        // 最初のファイルを取得
  while(file) {
    // ディレクトリを除外し、.csv ファイルのみを対象にする
    if(!file.isDirectory() && String(file.name()).endsWith(".csv")) {
      if(json != "[") json += ","; // 2件目以降はカンマで区切る
      json += "\"" + String(file.name()) + "\"";
    }
    file.close();                    // ファイルをクローズ
    file = root.openNextFile();      // 次のファイルへ
  }
  root.close();                      // ディレクトリをクローズ
  json += "]";
  server.send(200, "application/json", json);
}

// ============================================================
//  HTTPハンドラ: /download
//
//  指定されたCSVファイルをブラウザにストリーム送信する。
//  Content-Dispositionヘッダーで添付ファイルとして扱わせ、
//  ブラウザに「名前を付けて保存」ダイアログを出させる。
//
//  クエリパラメータ:
//    file = ダウンロードするファイルパス (例: /20250101_120000.csv)
// ============================================================
void handleDownload() {
  if (!sdAvailable) {
    server.send(503, "text/plain", "SD not available");
    return;
  }
  String path = server.arg("file");
  if(!path.startsWith("/")) path = "/" + path; // 先頭スラッシュを保証

  // ロギング中のファイルダウンロードを防止 (Issue #13)
  if (isLogging && path.equalsIgnoreCase(currentLogFile)) {
    server.send(409, "text/plain", "Conflict: file is currently being recorded. Stop logging first.");
    return;
  }

  if(storageFs->exists(path.c_str())) {
    File file = storageFs->open(path.c_str(), FILE_READ);
    // Content-Disposition: attachment でダウンロードとして扱う
    server.sendHeader("Content-Disposition", "attachment; filename=\"" + path.substring(1) + "\"");
    server.streamFile(file, "application/octet-stream"); // バイナリストリームで送信
    file.close();
  } else {
    server.send(404, "text/plain", "Not Found");
  }
}

// ============================================================
//  HTTPハンドラ: /delete
//
//  指定されたCSVファイルをSDカードから削除する。
//
//  クエリパラメータ:
//    file = 削除するファイルパス (例: /20250101_120000.csv)
// ============================================================
void handleDelete() {
  if (!sdAvailable) {
    server.send(503, "text/plain", "SD not available");
    return;
  }
  String path = server.arg("file");

  // 先頭スラッシュを保証
  if (!path.startsWith("/")) path = "/" + path;

  // .csv拡張子のみ許可
  if (!path.endsWith(".csv")) {
    server.send(400, "text/plain", "Bad Request: only .csv files can be deleted");
    return;
  }

  // パストラバーサル対策: ルートディレクトリ直下のファイルのみ許可
  // 2つ目の'/'が存在する場合はサブディレクトリ指定とみなす
  if (path.indexOf('/', 1) != -1) {
    server.send(400, "text/plain", "Bad Request: only files in root directory can be deleted");
    return;
  }

  // ロギング中のファイル削除を防止 (Issue #9)
  Serial.printf("Delete Request: [%s], Current: [%s], Logging: %s -> ", path.c_str(), currentLogFile.c_str(), isLogging ? "Yes" : "No");
  if (isLogging && path.equalsIgnoreCase(currentLogFile)) {
    Serial.println("BLOCKED");
    server.send(409, "text/plain", "Conflict: file is currently being recorded");
    return;
  }

  if (storageFs->remove(path.c_str())) {
    Serial.println("DELETED");
    server.send(200, "text/plain", "Deleted");
  } else {
    Serial.println("FAILED (Not Found?)");
    server.send(200, "text/plain", "Deleted"); // 存在しない場合も成功扱いにする既存仕様を維持
  }
}

// ============================================================
//  アプリ情報の返却
//  Web UIの表示と同じバージョン/ビルド番号をJSONで返す。
// ============================================================
void handleVersion() {
  String json = "{";
  json += "\"name\":\"" + String(APP_NAME) + "\",";
  json += "\"version\":\"" + String(APP_VERSION) + "\",";
  json += "\"build\":\"" + String(APP_BUILD) + "\"";
  json += "}";
  server.send(200, "application/json", json);
}

#ifdef WOKWI_SIMULATION
float int12ToFloat(uint16_t val) {
  val &= 0x0fff;
  return (val & 0x0800) ? -1.0f * (float)(val & 0x07ff) : (float)val;
}

bool readWokwiPixels(float *pixels) {
  uint8_t raw[128];
  const uint8_t chunkSize = 16;

  for (uint8_t offset = 0; offset < sizeof(raw); offset += chunkSize) {
    Wire.beginTransmission(amg_addr);
    Wire.write(0x80 + offset);
    if (Wire.endTransmission(false) != 0) {
      return false;
    }

    uint8_t received = Wire.requestFrom(amg_addr, chunkSize);
    if (received != chunkSize) {
      return false;
    }

    for (uint8_t i = 0; i < chunkSize; i++) {
      raw[offset + i] = Wire.read();
    }
  }

  for (uint8_t i = 0; i < 64; i++) {
    uint8_t pos = i << 1;
    uint16_t recast = ((uint16_t)raw[pos + 1] << 8) | raw[pos];
    pixels[i] = int12ToFloat(recast) * 0.25f;
  }
  return true;
}
#endif

// ============================================================
//  セットアップ (起動時に1度だけ実行)
// ============================================================
void setup() {
  Serial.begin(115200); // シリアルモニタ: 115200bps
  Serial.println("ThermalMapLogger booting...");

  // I2C初期化: SDA=GPIO21, SCL=GPIO22 (ESP32デフォルトピン)
  Wire.begin(21, 22);

  // タイムゾーンをJST (UTC+9) に設定
  // POSIX TZ文字列: "JST-9" = JSTタイムゾーン、UTCより9時間進める
  setenv("TZ", "JST-9", 1);
  tzset(); // TZ環境変数をシステムに反映

  // -------------------------------------------------------
  //  AMG8833 接続試行 (0x68 → 0x69 の順に自動検出)
  //  AD0ピンがGNDなら0x68、3.3Vなら0x69
  //  両方失敗した場合、3秒待って最大3回リトライ。
  //  3回失敗したらESP.restart()で再起動する。
  // -------------------------------------------------------
  {
    const int AMG_MAX_RETRY = 3;
    bool amgFound = false;
    for (int attempt = 1; attempt <= AMG_MAX_RETRY; attempt++) {
      Serial.printf("AMG8833: Attempt %d/%d - Connecting to 0x68...\n", attempt, AMG_MAX_RETRY);
      if (amg.begin(0x68)) {
        amg_addr = 0x68;
        Serial.println("AMG8833: Connected at 0x68!");
        amgFound = true;
        break;
      }
      Serial.println("AMG8833: Failed at 0x68. Trying 0x69...");
      if (amg.begin(0x69)) {
        amg_addr = 0x69;
        Serial.println("AMG8833: Connected at 0x69!");
        amgFound = true;
        break;
      }
      Serial.printf("AMG8833: Failed at 0x69. Sensor not found! (attempt %d/%d)\n", attempt, AMG_MAX_RETRY);
      if (attempt < AMG_MAX_RETRY) {
        Serial.println("AMG8833: Waiting 3s before retry...");
        delay(3000);
      }
    }
    if (!amgFound) {
      Serial.println("AMG8833: All attempts failed. Restarting...");
      ESP.restart();
    }
  }

  // -------------------------------------------------------
  //  SDカード初期化 (SD_MMC: 1bitモード)
  //  第2引数 true = 1bitモード (通常の4bitモードより配線が少ない)
  //  マウントポイント: /sdcard
  //  失敗した場合、3回までリトライ。それでも失敗ならSD無しモードで続行。
  // -------------------------------------------------------
  {
    const int SD_MAX_RETRY = 3;
    for (int attempt = 1; attempt <= SD_MAX_RETRY; attempt++) {
      Serial.printf("SD: Initializing... (attempt %d/%d)\n", attempt, SD_MAX_RETRY);
      if (SD_MMC.begin("/sdcard", true)) {
        storageFs = &SD_MMC;
        sdAvailable = true;
        Serial.println("SD: Initialized successfully.");
        break;
      }
      Serial.printf("SD: Failed. (attempt %d/%d)\n", attempt, SD_MAX_RETRY);
      if (attempt < SD_MAX_RETRY) {
        delay(1000);
      }
    }
    if (!sdAvailable) {
      Serial.println("SD_MMC not available. Trying SPI SD fallback...");
      SPI.begin(18, 19, 23, SPI_SD_CS_PIN);
      if (SD.begin(SPI_SD_CS_PIN)) {
        storageFs = &SD;
        sdAvailable = true;
        Serial.println("SPI SD: Initialized successfully.");
      } else {
        Serial.println("SD not available. Logging disabled.");
      }
    }
  }

  // -------------------------------------------------------
  //  WiFi アクセスポイント起動
  //  ESP32がAPとして動作し、外部ルーター不要でスタンドアロン動作
  //  接続後のESP32のIPアドレス: 192.168.4.1 (AP固定)
  // -------------------------------------------------------
#ifdef WOKWI_SIMULATION
  // Wokwiのnet.forwardはSTA接続が前提のため、Wokwi用ビルドだけ仮想Wi-Fiへ接続する。
  WiFi.mode(WIFI_STA);
  WiFi.begin(wokwiSsid, "", 6);
  Serial.print("Wokwi WiFi: Connecting to Wokwi-GUEST");
  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 10000) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Wokwi WiFi: Connected. IP address: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("Wokwi WiFi: Connection timed out. localhost forwarding may be unavailable.");
  }
#else
  WiFi.softAP(ssid, password);
#endif

  // -------------------------------------------------------
  //  HTTPルーティング登録
  //  各URLパスとハンドラ関数を紐付ける
  // -------------------------------------------------------
  server.on("/",        [](){ server.send(200, "text/html", INDEX_HTML); }); // WebUI
  server.on("/data",    handleData);     // センサーデータ取得 (0.5秒ごとに呼ばれる)
  server.on("/sync",    handleSync);     // 時刻同期 (起動時1回)
  server.on("/toggle",  handleToggle);   // ロギングON/OFF
  server.on("/list",    handleList);     // ファイル一覧
  server.on("/download",handleDownload); // ファイルダウンロード
  server.on("/delete",  handleDelete);   // ファイル削除
  server.on("/version", handleVersion);  // バージョン確認

  server.begin();
#ifdef WOKWI_SIMULATION
  Serial.println("HTTP server started. Connect to: http://localhost:8180");
#else
  Serial.println("HTTP server started. Connect to: http://192.168.4.1");
#endif
}

// ============================================================
//  メインループ
//  500msごとにセンサー読取とSD保存を実行する。
//  ブラウザの接続状態に関わらずSD記録が継続される。
//  /dataエンドポイントはlatestPixelsを返すだけ。
// ============================================================
void loop() {
  unsigned long now = millis();
  if (now - lastSampleTime >= 500) {
    lastSampleTime = now;
#ifdef WOKWI_SIMULATION
    if (!readWokwiPixels(latestPixels)) {
      amg.readPixels(latestPixels); // Wokwi直接読みが失敗した場合のみライブラリ経由へ戻す
    }
#else
    amg.readPixels(latestPixels); // センサーから64画素を取得してグローバルに保持
#endif
    saveToSD(latestPixels);       // ロギング中ならSDに記録
  }
  server.handleClient(); // クライアントからのHTTPリクエストを処理
}
