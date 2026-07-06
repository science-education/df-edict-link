# df-edict-link

**DF-X10000 電子辞書の PC 連携ソフト「PASORAMA」を Windows 10 / 11 で動かすための、独立実装の WinUSB 版 `USB_PC.dll`（差し替えDLL）とセットアップ一式。**

An independent, clean-room **WinUSB replacement for `USB_PC.dll`** that lets the
**PASORAMA** companion software for the **DF-X10000** electronic dictionary run on
modern **Windows 10 / 11**, without the legacy kernel driver.

---

## ⚠️ 権利・免責 / Legal & disclaimer

- 本リポジトリには **私たちが独自に作成したものだけ** を収録しています。
  **PASORAMA 本体・純正 `USB_PC.dll`・`PLCommon.dll`・辞書データ・カーネルドライバ等、
  メーカーの著作物は一切含みません。** それらは各権利者に帰属します。
- 利用には、**あなた自身が正規に所有する DF-X10000 実機と、正規にインストールされた
  PASORAMA** が必要です。本プロジェクトはその純正ファイルに関して、
  **`USB_PC.dll` だけを、あなたが所有するインストール環境上で差し替えて**使います。
- 本 DLL は、**あなたが所有するハードウェアと純正ソフトを相互運用させる目的**での
  観察に基づく独立実装です。メーカーとは無関係・非公認です。
- **無保証です。** 本プロジェクトの成果物は「現状のまま（AS IS）」提供され、
  商品性・特定目的適合性を含む一切の保証をしません。作者は**不具合を修正する義務を負わず**、
  本プロジェクトの利用・不利用によって生じたいかなる損害（デバイスやソフトウェアの
  故障・データ喪失等を含む）についても**一切責任を負いません**。自己責任でご利用ください。
- ライセンスは **GPL-3.0**（[LICENSE](LICENSE)）。製品名・会社名は各社の商標（過去においての商標を含む）です。

> This project ships **only our own original work**. It contains **no PASORAMA
> binaries, no genuine `USB_PC.dll`, no `PLCommon.dll`, no dictionary data, and no
> vendor driver.** You must own a genuine DF-X10000 and a legitimately installed
> copy of PASORAMA; this replaces only `USB_PC.dll` on *your* installation for the
> purpose of interoperating your own hardware with your own software.
>
> **Provided AS IS, without warranty of any kind.** The authors have no obligation
> to fix anything and accept no liability for any damage (including device or
> software malfunction or data loss) arising from use. Trademarks — including
> former trademarks — belong to their respective owners.

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
| 項目内リンク（相互参照）ジャンプ | ✅ 動作（履歴書き込みを成功応答で受理） |
| 検索の高速化（無駄待ち撤去・ブロックキャッシュ） | ✅ 実装済み |
| 履歴の**永続化** | ⛔ 未対応（書き込み経路の wire 仕様が未解析。navigation 自体は落ちない） |

> The on-device **write / history-persist** path is intentionally acknowledged but
> not persisted: the genuine write opcode has not been captured, so we return
> success without forging an unverified frame. Cross-reference navigation works;
> the previous crash on clicking an in-entry link is fixed.

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

WinUSB 上で純正 DLL の関数テーブルを再現するために解析した、
USB バルク転送のフレーミング・オペコード・エクスポート表の対応などを
[docs/PROTOCOL.md](docs/PROTOCOL.md) にまとめています（純正バイナリや辞書データは含みません）。
