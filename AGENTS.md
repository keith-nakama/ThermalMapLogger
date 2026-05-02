# Repository Guidelines

## Language
- ユーザーへの説明は必ず日本語で行ってください
- コード・クラス名・関数名は英語のまま維持してください
- コメントは既存スタイルに合わせてください

## Security & Configuration Tips
権限昇格の確認は、毎回コマンド全体に近い長い承認ではなく、再利用しやすい `prefix_rule` を優先してください。実運用の認証情報や本番 DB の内容はコミットしないでください。

## Project Structure
- `ThermalMapLogger.ino` が本体です。ESP32 WROVER を Wi-Fi AP として動作させ、AMG8833 の 8x8 温度データを Web UI に配信しつつ、SD_MMC 経由で CSV に記録します。
- Web UI は `ThermalMapLogger.ino` 内の `INDEX_HTML` に HTML/CSS/JavaScript として埋め込まれています。UI 変更時も外部ファイル追加ではなく、既存の単一スケッチ構成を優先してください。
- `README.md` はセットアップ、配線、HTTP エンドポイント、バージョン履歴の利用者向け説明です。挙動やピン、エンドポイント、バージョンを変えた場合は必ず同期してください。
- `docs/specification.md` は UML 仕様資料、`docs/images/` は実機写真と回路図です。仕様資料は古いバージョン情報を含む可能性があるため、実装確認では `ThermalMapLogger.ino` を優先してください。

## Build & Verification
- Arduino IDE でボードを `ESP32 Wrover Module` に設定し、ライブラリマネージャーから `Adafruit AMG88xx Library` と依存の `Adafruit BusIO` を導入してビルドします。
- スケッチは `WiFi.h`、`WebServer.h`、`Wire.h`、`Adafruit_AMG88xx.h`、`FS.h`、`SD_MMC.h`、`time.h`、`sys/time.h` に依存します。ESP32 Arduino core が前提です。
- 実機確認では、シリアルモニタ 115200bps、AMG8833 の 0x68/0x69 自動検出、SD_MMC 1bit モード初期化、`ESP32-Thermal-Monitor` への接続、`http://192.168.4.1` の Web UI 表示を確認してください。
- 手動確認の主要観点は `/sync` 後に `Start Logging` が成功すること、`/data` が 64 要素 JSON を返すこと、CSV が `YYYYMMDD_HHMMSS.csv` で作成されること、`/list` `/download` `/delete` が SD カード上の `.csv` に対して期待通り動くことです。
- ハードウェアが必要なため、自動テストだけで完了扱いにしないでください。実機未確認の場合は、最終報告と Issue コメントに未確認範囲を明記してください。

## Coding Guidelines
- 既存の区切りコメント、関数単位の説明、Issue 番号に紐づく補足コメントのスタイルに合わせてください。
- サンプリング周期は `loop()` の 500ms 間隔が基準です。ブラウザ接続状態に依存せず SD 記録を継続する設計を崩さないでください。
- 時刻は `/sync` と `millis()` 差分で管理しています。1970年ファイル名の再発を避けるため、ロギング開始前の `g_timeSynced` チェックを維持してください。
- SD 書き込みは `logFile` を保持し、`flush()` と定期的な close/open で確定する方針です。毎サンプルで open/close する実装へ戻さないでください。
- `/delete` などファイル操作系エンドポイントでは `.csv` 限定、ルート直下限定、パストラバーサル対策を維持してください。
- Wi-Fi SSID やパスワードを変更する場合も、実運用の認証情報は入れず、README の説明と整合させてください。

## Commit & Pull Request Guidelines
履歴では `Build 044: ...` のようにビルド番号付きで要点を書く形式が使われています。コミットメッセージは `Build NNN: 変更内容` を基本にしてください。PR には目的、影響範囲、手動確認内容、UI 変更がある場合のスクリーンショットを含め、設定値や秘密情報は絶対に含めないでください。

Git タグは必ず `<アプリ名>-<yymmdd>.<build番号>` 形式にしてください。例: `ThermalMapLogger-260425.046`。日付はタグ作成時点ではなく、そのビルドに対応する日付を使い、ビルド番号は画面内やコメントに記載した `Build NNN` と一致させます。

## Issue Workflow
Issue 対応は GitHub 上の Issue を基点に進めてください。「対応して」と指示を受けたら、ローカルファイルで下書きを作らず、`gh issue edit` などの GitHub CLI で Issue 本文そのものを清書します。本文は必ず以下の形式に揃え、タイトルは1行で内容を要約し、動詞で終わる文にしてください。
`## タイトル` 
`## 概要` 
`## 再現手順や背景` 
`## 期待する結果` 
`## スクリーンショットやエラー出力` 
`## 備考関連IssueやPRへのリンク` 

コードやドキュメントを変更した後は、必ず Issue コメントで変更内容と実機確認依頼を出し、ユーザー確認前に commit / close / push をしないでください。

Issue 解決時はクローズだけで終わらせず、Issue コメントに調査検証の具体的な過程、失敗や再試行の経緯、コードの具体的な変更箇所、Before / After、主要ロジック修正を残したうえで、確認OK後にコミット、プッシュへ進めます。ソースコードやドキュメントを変更して Issue を解決する場合は、`gh issue close` などで Issue を単独 close せず、`Fix #123` や `Closes #123` のように commit メッセージへ記載して close させてください。GitHub CLI で本文やコメントを送る際は、文字化け防止のため必ず BOM なし UTF-8 ファイルを介してください。

権限昇格の確認を求める際は、毎回コマンド全体に近い長い承認ではなく、再利用しやすい `prefix_rule` を優先してください。特に `git add` `git commit -m` `git push origin` `gh issue comment` のような、同種作業に再利用できる短い prefix で承認を出すことを基本にします。
