# PROTOCOL — interoperability notes

WinUSB 上で純正 `USB_PC.dll` の役割を再現するために観察・整理した、
**デバイスとの USB バルク通信仕様** と **DLL の公開インターフェイス契約** の解説です。

This document describes, in our own words, the USB wire behavior and the exported
DLL interface needed to interoperate PASORAMA with the device over WinUSB. It
contains **no vendor code, no disassembly, and no dictionary data** — only the
externally observable protocol facts required for a reimplementation.

> 本資料は、相互運用性のために観察された通信仕様を**独自の記述として整理**したものです。
> 純正バイナリ、その逆アセンブル結果、辞書データ、暗号化・認証・アクセス制限の回避手順は
> **含みません**。辞書データの抽出・複製・再配布や、実機なしでの利用を目的とするものでは
> ありません。
>
> This is an independent write-up of observed behavior for interoperability only. It
> includes no vendor binaries, no disassembly, no dictionary data, and no methods for
> bypassing encryption, authentication, or access controls, and is not for extracting
> or redistributing dictionary content.

---

## 1. トランスポート / Transport

- デバイスのベンダーインターフェイス（`MI_01`）に WinUSB をバインドして使用。
- Bulk OUT エンドポイント `0x03`、Bulk IN エンドポイント `0x83`。
- 送受信は「24 バイト固定ヘッダのコマンド」→（必要ならペイロード本体）→
  「データ本体（IN）」→「12 バイトの ACK」で 1 トランザクション。

### コマンドヘッダ（24 バイト, OUT）

| offset | size | 意味 |
|---|---|---|
| 0 | 1 | `0xAB`（コマンド開始マーカ） |
| 1 | 1 | シーケンス番号（1 ずつ増加、ACK と突き合わせ） |
| 2 | 1 | `0x80` |
| 3 | 1 | `0x10` |
| 4 | 4 | length（LE。オペコード依存の長さ/オフセット等） |
| 8 | 4 | opcode（LE） |
| 12 | 4 | length のコピー（4..7 と同値） |
| 16 | 8 | tail（オペコード固有の引数 8 バイト） |

### ACK（12 バイト, IN）

- 先頭が `0xAC`、続いてシーケンス番号。
- offset 4 の 4 バイト（LE）が「ACK 値」で、オペコードにより
  **オープン時のリモートハンドル**、**シーク後の絶対位置**、**実転送長** などを表す。
- データを伴うオペコードでは、ACK の前に 1 個以上の IN パケットでデータ本体が届く。
  受信ループは `0xAC` の 12 バイトが来るまでデータを連結する。

---

## 2. オペコード / Opcodes

観察された主なオペコード（`opcode` フィールド、LE 32bit）:

| 用途 | opcode | 備考 |
|---|---|---|
| open | `0x000C8037` | tail に固定フィールド、ペイロードに UTF-16LE のパス。ACK 値＝リモートハンドル。 |
| read | `0x000D0037` | length に要求バイト数。tail 先頭 4 バイトにリモートハンドル。データ本体＋ACK（実長）。 |
| seek | `0x00138037` | length にオフセット、tail にハンドルと origin(0/1/2)。ACK 値＝新しい絶対位置。 |
| sector read | `(handle<<16) | 0x0034` | ランダムブロック読み（後述）。※本実装では直接送らず read 経路で代替。 |

> パス系ペイロードは UTF-16LE で、末尾 NUL を含む。open のペイロード長は
> ヘッダの length に入る。

---

## 3. 公開インターフェイス（エクスポート表） / Exported interface

純正 `USB_PC.dll` は 2 つのシンボルをエクスポートします。

- `DllExportTable` — 関数ポインタ配列（DATA エクスポート）。呼び出し側はこの配列の
  各インデックスを、ファイル API 相当の操作として呼ぶ。
- `InitialDll` — 初期化用エントリ（本実装では実処理なし）。

呼び出し側が使う主なインデックスと、本実装での対応:

| index | 役割 | 本実装 |
|---|---|---|
| 0 | init | 接続確立（open + ハンドシェイク） |
| 2 | openA（ANSI パス） | パスを UTF-16LE 化して open、仮想ハンドルを返す |
| 3 | openW（wide パス） | 同上 |
| 4 | read(handle, buf, len) | read 経路で分割読み |
| 5 | write(handle, buf, len) | **成功応答のみ**（履歴書き込みを受理。永続化はしない・下記参照） |
| 6 | seek(handle, off, origin) | seek |
| 8 | close(handle) | 仮想ハンドル解放 |
| 9 | sector read（ランダムブロック） | 後述の式でオフセット算出し read 経路で取得 |
| 20 | version 取得 | バージョン文字列を返す |
| 21 | connect | 接続確認 |
| 22 | disconnect | クローズ |
| 23 | セッション/生存確認 | 実際に USB パイプ健全性を検査（下記） |

> インデックスは「仮想ハンドル（本 DLL が採番）」でファイルを識別し、内部で
> デバイス側の「リモートハンドル」（open の ACK 値）に対応づけます。

### 補助関数テーブル

呼び出し側は別途、補助関数ポインタ表（open/read/seek/size 等の別ラッパ）も参照します。
本実装は必要なもの（openW / read / seek / file-size）を提供し、他は失敗を返します。

---

## 4. index 9（ランダムブロック読み）/ Random block read

検索や項目表示では、**すでに open 済みの索引ファイルに対する固定サイズの
ランダムブロック読み** が多数発行されます。呼び出しは 4 引数では足りず、
スタック上に複数引数が積まれます（cdecl）。観察された対応:

- `handle = a1 & 0xffff` … open で返した（呼び出し側が保持する）ハンドル。
- `block = (a2 << 8) | (a5 & 0xff)` … `a2` が上位、`a5` が下位バイト。
  **`a2` はバイトにマスクしない**（上位バイトが有効。マスクすると大きなオフセットで破綻）。
  `a0` は `a2 >> 8` の冗長コピーで、追加情報を持たない。
- `offset = block * 4096`、`nbytes = a4 * 512`（`a4` は通常 8 ＝ 4096 バイト）。

本実装ではこのランダムブロック読みを、実績のある read（`0x000D0037`）経路に
必要なら seek を挟んで置き換えます（sector-read オペコードはセッション固有の
ドライバ内部値を要し、外部から正確に再構成できないため）。

### ブロックキャッシュ

同一 `(リモートハンドル, offset)` のブロックが繰り返し読まれるため、
固定サイズ（4096B）ブロックを LRU でキャッシュします。読み取り専用辞書のため
安全ですが、**ハンドルの同一性が変わりうる契機**（close / 再オープン /
新規オープンでのハンドル番号再利用 / 切断）では該当ブロックを無効化します。

---

## 5. write と履歴 / write and history

項目内の相互参照リンクを辿ると、呼び出し側はデバイス上の**履歴ファイル**
（`.../KJ46Hist.bin` 等）を open し、小さなレコードを **index 5（write）** で書き込みます。

デバイスへの書き込みオペコードの wire 仕様は未採取のため、本実装は
**未検証のフレームを捏造せず、write を「全バイト書けた」という成功応答で受理**します。
これによりリンク遷移は正常に続行しますが、**履歴は永続化されません**。

> 以前は write が失敗（-1）を返していたため、呼び出し側がリトライののち全ハンドルを
> 破棄し、リンククリックでクラッシュしていました。成功応答を返すことで解消しています。
> 履歴を実際に保存したい場合は、リンククリック時の write を実機採取して
> オペコード/tail を解析する必要があります。

---

## 6. 生存確認とセッション維持 / Keepalive

辞書ロード後、通信が静かになると呼び出し側は定期的に「生存確認」を発行します。
本実装はこの契機で **実際に USB パイプの健全性を検査**し、転送失敗を検知したら
再接続（クローズ→再オープン→ハンドシェイク再送）を行います。これを行わないと、
パイプが黙って落ちた場合に、呼び出し側が「生存」と誤認して死んだパイプへ要求を
送り続け、最終的にクラッシュします。

また、通信を絶やさないための軽量なキープアライブ要求を、別スレッドから
一定間隔で送出します。
