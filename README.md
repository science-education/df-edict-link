# df-edict-link

**df-edict-link は、DF-X10000 電子辞書を現行 Windows で利用するための非公式互換ツールです。**
正規にインストールされた PASORAMA 環境において、USB 通信を担当する `USB_PC.dll` と同名の
**独立実装の WinUSB ベース互換 DLL** を利用者環境に配置することで、レガシーなカーネルドライバ
無しで **Windows 10 / 11** での動作を補助します。純正ファイルは同梱・改変しません。

An independently developed, **observation-based WinUSB compatibility DLL for
`USB_PC.dll`**, intended to help a DF-X10000 electronic dictionary interoperate
with a **legitimately installed PASORAMA** environment on modern **Windows 10 / 11**,
without the legacy kernel driver.

---

## ⚠️ 権利・免責 / Legal & disclaimer

- 本リポジトリには **私たちが独自に作成したファイルのみ** を収録しています。
  PASORAMA 本体・純正 `USB_PC.dll`・`PLCommon.dll`・辞書データ・カーネルドライバ・
  マニュアル・ロゴ・アイコンその他のメーカー製ファイルは **含みません**。
- 利用には、**利用者自身が正規に所有する DF-X10000 実機と、正規にインストールされた
  PASORAMA 環境** が必要です。本プロジェクトは、利用者自身が管理する正規インストール環境に、
  USB 通信を担当する `USB_PC.dll` と同名の**互換 DLL を配置**して利用するものです。
  `PASORAMA.exe`・`PLCommon.dll`・辞書データその他の純正ファイルは**配布・改変しません**。
- 本 DLL は、利用者が所有するハードウェアと正規ソフトウェアの**相互運用を目的として、
  観察に基づき独立に実装**したものです。本プロジェクトは、いずれの権利者による公式ソフトウェア・
  後継製品・認定製品・提携製品でもありません（**非公認**）。
- 本プロジェクトは、**辞書データの抽出・複製・再配布、アクセス制限の回避、または実機なしでの
  利用を目的とするものではありません**。
- **無保証です。** 成果物は「現状のまま（AS IS）」提供され、商品性・特定目的適合性・
  権利非侵害性を含む一切の保証をしません。作者は**不具合を修正する義務を負わず**、
  利用・利用不能によって生じたいかなる損害についても**一切責任を負いません**。自己責任でご利用ください。
- ライセンスは **GPL-3.0**（[LICENSE](LICENSE)）。旧機器の相互運用性に関する改良を
  コミュニティへ還元しやすくするために採用しています。
- **PASORAMA、DAYFILER、DF-X10000、SII** その他の製品名・会社名（現在および過去の商標を含む）は、
  互換対象および利用環境を**識別する目的でのみ**記載しています。これらの名称に関する権利は
  それぞれの権利者に帰属します。

> This repository contains **only files independently created by the project
> authors**. It does not include PASORAMA binaries, the genuine `USB_PC.dll`,
> `PLCommon.dll`, dictionary data, kernel drivers, manuals, logos, icons, or other
> vendor-provided files.
>
> Use requires a **lawfully owned DF-X10000 device and a legitimately installed
> PASORAMA** environment. This project provides a same-name compatibility DLL for
> the USB component in the user's own installation; it does **not** distribute or
> modify `PASORAMA.exe`, `PLCommon.dll`, dictionary data, or other genuine files.
> It is **not** intended for extracting, copying, redistributing, or bypassing
> access controls for dictionary data, nor for using PASORAMA without the device.
>
> This is an independently developed, observation-based implementation and is **not
> affiliated with, endorsed by, sponsored by, or certified by** any rights holder.
> PASORAMA, DAYFILER, DF-X10000, SII, and other names (including former trademarks)
> are used only to identify compatible products; all rights belong to their owners.
>
> **Provided AS IS, without warranty of any kind** (including merchantability,
> fitness for a particular purpose, and non-infringement). The authors have no
> obligation to fix anything and accept no liability for any damage arising from use
> or inability to use this project.

---

## なぜ必要か / Why

PASORAMA.exe は、辞書デバイスとの通信を純正 `USB_PC.dll` に任せ、その DLL が
カーネルドライバ `PASORAMA.sys` 経由で USB をやり取りします。この古いドライバは
Windows 10 / 11（64bit・ドライバ署名強制）では素直に動きません。

本プロジェクトの `USB_PC.dll` は、同じ公開関数テーブルを提供しつつ、内部を
**Microsoft 標準の WinUSB** に差し替えます。これにより **レガシーな署名付きドライバ無しで**
PASORAMA.exe をそのまま最新 Windows で動かせます（`PASORAMA.exe` や `PLCommon.dll` は
純正のまま利用）。

---

## 動作状況 / Status

| 機能 | 状態 |
|---|---|
| 接続・辞書ロード | ✅ 動作 |
| 検索（英字・漢字） | ✅ 動作 |
| 項目内リンク（相互参照）ジャンプ | ✅ 動作 |
| 音声再生・複数辞書（一括）検索・設定 | ✅ 動作 |
| 図版（挿絵）の表示 | ⚠️ 概ね動作（一部の図が表示されない既知の制限あり。下記） |
| 検索の高速化（無駄待ち撤去・4096B読み） | ✅ 実装済み |
| ブロックキャッシュ | ✅ 既定**有効**（ファイルパスをキーにし取り違え不能＝正しく高速。`USBPC_NOCACHE=1` で無効化可） |

### 既知の制限 / Known limitations

- **一部の図版が表示されない**ことがあります。原因は、フォント/グリフ系ファイルの
  ある範囲の読み出しがデバイス上でゼロを返すため（`index9`/op34 の絶対オフセット再構成が
  一部ブロックで実データに届かない）で、ブロックキャッシュとは無関係（キャッシュ ON/OFF で挙動不変）です。
  検索・本文・多くの図は表示されます。正確な修正には図描画時の op34 の実機採取（グランドトゥルース）が必要です。

  Some figures may not render. Certain reads of the glyph/font files return zeros on
  the device (our `index9`/op34 absolute-offset reconstruction does not reach the real
  data for some blocks); this is unrelated to the block cache. Search, entry text, and
  most figures work. A proper fix needs ground-truth capture of op34 during figure rendering.
| ユーザーデータ（履歴・単語帳・メモ）の**永続化** | ⛔ 未対応（下記参照。操作自体は落ちない） |

> ユーザーデータの書き込み（履歴・単語帳・メモ）は、デバイス側 write の wire 仕様が
> 未解析のため、**未対応の書き込み要求には安全側の互換応答を返します**（検証されていない
> 書き込みフレームは生成しません）。このため相互参照リンクの遷移や各機能は正常に動作しますが、
> これらのユーザーデータは保存されません。以前あった「項目内リンクのクリックでクラッシュ」は解消済みです。
>
> User-data writes (history / wordbook / memo) are acknowledged with a safe
> compatibility response rather than persisted: the device-side write frame has not
> been analyzed, so no unverified write frames are generated. Navigation and
> features work; this user data is not saved. The earlier crash on clicking an
> in-entry link is fixed.

---

## 必要環境 / Requirements

- Windows 10 または 11（x64）
- DF-X10000 実機 ＋ 正規インストール済み PASORAMA
- ビルド用: Visual Studio Build Tools（**x86 / 32bit C++ ツールチェーン**）＋ Windows SDK
  （`PASORAMA.exe` は 32bit なので DLL も x86 が必須）
- USB: Microsoft **WinUSB**（後述の INF もしくは [Zadig](https://zadig.akeo.ie/) で割り当て）

---

## セットアップ手順 / Setup

詳細は [docs/INSTALL.md](docs/INSTALL.md)。概要：

1. **PASORAMA を正規の手段でインストール**
   メーカー正規の配布物・手順で PASORAMA をインストールしておきます
   （`PASORAMA.exe` と純正 `USB_PC.dll` を含むフォルダが用意された状態）。本プロジェクトは
   この正規インストール環境の `USB_PC.dll` だけを差し替えます。
2. **デバイスに WinUSB を割り当てる**
   デバイスのベンダーインターフェイス（`VID_0619&PID_0704&MI_01`）に WinUSB をバインド。
   最も簡単なのは Zadig。もしくは [driver/winusb_pasorama.inf](driver/winusb_pasorama.inf) を利用。
3. **DLL をビルド**
   ```bat
   build.bat
   ```
   → `build\USB_PC.dll`（x86）が生成されます。
4. **差し替えインストール**（管理者 PowerShell）
   ```powershell
   .\scripts\install_dll.ps1
   ```
   純正 `USB_PC.dll` を一度だけ `USB_PC.dll.orig_backup` に退避し、置き換えます。
   （PASORAMA を終了してから実行）
5. **PASORAMA.exe を通常起動**。接続・検索できれば成功。

元に戻すには、退避した `USB_PC.dll.orig_backup` を `USB_PC.dll` に戻すだけです。

---

## リポジトリ構成 / Layout

```
df-edict-link/
├─ src/
│  ├─ usb_pc_stub.c        # 差し替え USB_PC.dll の本体（唯一の実装ソース）
│  └─ usb_pc_stub.def      # エクスポート定義（DllExportTable / InitialDll）
├─ driver/
│  ├─ winusb_pasorama.inf  # 自作 WinUSB インストール INF
│  └─ winusb_pasorama.cdf  # カタログ生成用定義
├─ scripts/
│  ├─ install_dll.ps1      # 純正退避＋差し替え（管理者・パラメータ化）
│  └─ run_pasorama_verbose.bat  # 詳細トレース付き起動（デバッグ用）
├─ docs/
│  ├─ INSTALL.md           # セットアップ詳細
│  └─ PROTOCOL.md          # 相互運用のためのプロトコル/インターフェイス解説
├─ build.bat               # x86 でビルド
├─ LICENSE                 # GPL-3.0
└─ .gitignore
```

## デバッグ / Debugging

`USBPC_VERBOSE=1` を設定して起動すると、全 API 呼び出し・USB 転送を
`%TEMP%\usbpc_stub_log.txt` に記録します（`scripts\run_pasorama_verbose.bat` が簡便）。
既定では静かに動作します。

---

## 開発の技術的背景 / Protocol notes

WinUSB 上で純正 DLL の関数テーブルを再現するために観察・整理した、
USB バルク転送のフレーミング・オペコード・エクスポート表の対応などを
[docs/PROTOCOL.md](docs/PROTOCOL.md) にまとめています（純正バイナリの逆アセンブル、
辞書データ、アクセス制限の回避手順は含みません）。

---

## 対象範囲 / Supported scope

本プロジェクトは、**正規所有の DF-X10000 実機と正規インストール済み PASORAMA 環境の相互運用**
のみを目的とします。辞書データの抽出・複製・再配布、アクセス制限の回避、または実機なしでの
利用を目的とするものではありません。

> This project is intended only for interoperability between a lawfully owned
> DF-X10000 device and a legitimately installed PASORAMA environment. It is not
> intended for extracting, copying, redistributing, or bypassing access controls for
> dictionary data, nor for using PASORAMA without the corresponding device.

## 権利者の方へ / Notice to rights holders

権利者の方で、本リポジトリの内容についてご懸念がある場合は、GitHub の **Issue**
（または今後掲載する連絡先）までご連絡ください。内容を確認のうえ、必要に応じて修正・削除等の
対応を行います。

> If you are a rights holder and believe this repository contains material that
> should be revised or removed, please contact us via GitHub **Issues**. We will
> review the concern and take appropriate action where necessary.
