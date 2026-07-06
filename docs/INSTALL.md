# INSTALL — DF-X10000 / PASORAMA on Windows 10 / 11

このガイドは、あなたが正規に所有する DF-X10000 と PASORAMA インストール環境で、
本リポジトリの WinUSB 版 `USB_PC.dll` に差し替える手順です。純正ファイルは同梱していません。

> All steps operate on **your own** device and **your own** legitimately installed
> PASORAMA. Nothing here distributes vendor files.

---

## 0. 前提 / Prerequisites

- Windows 10 or 11, x64
- DF-X10000 実機、USB ケーブル
- 正規インストール済み PASORAMA（`PASORAMA.exe` と純正 `USB_PC.dll` を含むフォルダ。
  既定例: `C:\Program Files\PASORAMA\df-x10000\`）
- Visual Studio Build Tools（**x86 C++**）＋ Windows SDK
- （任意）[Zadig](https://zadig.akeo.ie/) — WinUSB 割り当てが最も簡単

---

## 1. デバイスに WinUSB を割り当てる / Bind WinUSB

PASORAMA が使うベンダーインターフェイスは **`USB\VID_0619&PID_0704&MI_01`** です。
ここに Microsoft の WinUSB をバインドします。方法は 2 通り。

### 方法A: Zadig（推奨・簡単）

1. デバイスを接続。
2. Zadig を起動 → `Options > List All Devices`。
3. リストから **PASORAMA の複合デバイスのインターフェイス 1（MI_01）** を選択
   （USB ID が `0619 0704`、Interface 1 のもの）。
4. ドライバに **WinUSB** を選び、`Replace Driver` / `Install Driver`。

> ⚠️ **注意（重要）**: 対象が **DF-X10000 のベンダーインターフェイス（`0619 0704` の
> Interface 1 = MI_01）** であることを必ず確認してから割り当ててください。**誤ったデバイスや
> インターフェイスに WinUSB を割り当てると、他の機器や機能が正常に動作しなくなる可能性があります。**
> 万一誤って当てた場合は、デバイスマネージャで該当デバイスのドライバを「元に戻す」／
> アンインストールして再スキャンすれば復旧できます。自己責任で操作してください。

### 方法B: 付属 INF（pnputil）

`driver/winusb_pasorama.inf` は同一の GUID/デバイス条件を記述した自作 INF です。
自己署名でカタログを作るか、テスト署名モードで導入します（上級者向け）。

```powershell
# 管理者 PowerShell、driver フォルダで
pnputil /add-driver .\winusb_pasorama.inf /install
```

未署名 INF はドライバ署名強制下では拒否されます。その場合は方法A（Zadig）を使うか、
テスト署名（`bcdedit /set testsigning on`）＋自己署名カタログを用意してください。

### 確認

デバイスマネージャで対象インターフェイスのドライバが **WinUSB** になっていること。
本 DLL はインターフェイス GUID `{6EC98D31-3F8B-4C1F-9E6C-3F6F5B0A1A11}` を探すので、
INF を使う場合はこの GUID が登録されている必要があります（付属 INF は登録済み）。

---

## 2. DLL をビルド / Build

リポジトリのルートで:

```bat
build.bat
```

- `vcvarsall.bat` の場所が違う場合は `build.bat` の `VCVARS` を編集。
- 成功すると `build\USB_PC.dll`（**x86**）が生成されます。
- `dumpbin /exports` に `DllExportTable` と `InitialDll` が出れば OK。

---

## 3. 差し替えインストール / Install

**PASORAMA を終了**してから、**管理者 PowerShell** で:

```powershell
.\scripts\install_dll.ps1
# 別フォルダにインストールしている場合:
.\scripts\install_dll.ps1 -PasoramaDir "D:\Path\To\PASORAMA\df-x10000"
```

スクリプトは:
1. 純正 `USB_PC.dll` を **一度だけ** `USB_PC.dll.orig_backup` に退避（既存の退避は上書きしない）
2. ビルドした `USB_PC.dll` を配置
3. SHA-256 を表示

手動で行う場合も同じです（純正を退避 → 置換）。Program Files への書き込みは昇格が必要。

---

## 4. 起動 / Run

`PASORAMA.exe` を通常どおり起動。接続 → 辞書ロード → 検索 → 項目内リンク遷移が
できれば成功です。

デバッグしたいときは:

```bat
scripts\run_pasorama_verbose.bat
```

`USBPC_VERBOSE=1` で全トレースが `%TEMP%\usbpc_stub_log.txt` に残ります。

---

## 5. 元に戻す / Revert

```powershell
# 管理者 PowerShell、PASORAMA 終了後
$dir = "C:\Program Files\PASORAMA\df-x10000"
Copy-Item "$dir\USB_PC.dll.orig_backup" "$dir\USB_PC.dll" -Force
```

WinUSB 割り当ても戻したい場合はデバイスマネージャでドライバを元に戻すか、
`pnputil /delete-driver ... /uninstall` を使います。

---

## トラブルシューティング / Troubleshooting

| 症状 | 対処 |
|---|---|
| PASORAMA が「USB が見つからない/切断」 | WinUSB が **MI_01** に当たっているか確認。ケーブル・再接続。 |
| ビルドが `vcvarsall.bat not found` | `build.bat` の `VCVARS` を実際の VS パスに修正。**x86** ツールチェーン必須。 |
| `install_dll.ps1` が権限エラー | **管理者** PowerShell で実行。PASORAMA を終了してから。 |
| 起動直後に落ちる/無反応 | `USBPC_VERBOSE=1` でログを取り、`%TEMP%\usbpc_stub_log.txt` の末尾を確認。 |
| DLL がロックされて置換できない | PASORAMA / 関連プロセスを完全終了してから再実行。 |
