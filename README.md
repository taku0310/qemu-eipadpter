# qemu-eipadpter

Ubuntu / Linux ホスト上で動作する **EtherNet/IP アプリケーション**一式です。
**Class 1（インプリシット / 周期 I/O）通信**を行う、対になる 2 つのアプリを含みます:

- **`eip_adapter`** — アダプタ（CIP ターゲット機器）。スキャナ/PLC からの接続を受ける側
- **`eip_scanner`** — スキャナ（CIP オリジネータ）。アダプタへ接続して I/O を駆動する側

両者は相互通信でき、本リポジトリ単体で Class 1 通信を完結・検証できます。

Two paired EtherNet/IP applications that run as ordinary userspace programs on an
**Ubuntu/Linux host** and perform **Class 1 (implicit, cyclic) I/O**: `eip_adapter`
(CIP target) and `eip_scanner` (CIP originator). They interoperate with each
other. The well-known ports (TCP/UDP 44818, UDP 2222) are unprivileged —
**no root needed**.

> 履歴上リポジトリ名は `qemu-eipadpter` ですが、本アプリは QEMU を必要とせず、
> ホスト OS（Ubuntu）上で直接動作します。

---

## 機能 / Implemented protocol

| 層 | 内容 |
|----|------|
| Encapsulation (TCP/44818) | `RegisterSession` / `UnRegisterSession` / `ListServices` / `ListIdentity` / `SendRRData` |
| Discovery (UDP/44818) | `ListIdentity` ブロードキャスト応答 |
| CIP Connection Manager | `Forward Open` (0x54) / `Large Forward Open` (0x5B) / `Forward Close` (0x4E) |
| **Class 1 I/O (UDP/2222)** | O→T 受信（消費）, T→O を RPI 周期で生産, シーケンス管理, 接続タイムアウト監視 |
| 接続種別 | Exclusive-Owner / Input-Only / Listen-Only |
| T→O 生産 | point-to-point ユニキャスト / **マルチキャスト（複数消費者で共有）** |
| CIP オブジェクト | Identity (0x01), Assembly (0x04) の Get_Attribute_Single/All |

### データフロー / I/O semantics

```
        Originator (PLC / Scanner)               Adapter (this app, on Ubuntu)
   ┌──────────────────────────┐            ┌──────────────────────────┐
   │  Forward Open  ───────────TCP/44818──▶ │  Class 1 接続を確立        │
   │  O→T data    ─────────────UDP/2222───▶ │  消費 (consume)           │
   │  (出力 / output assembly)              │                          │
   │  T→O data    ◀────────────UDP/2222──── │  生産 (produce, 周期 RPI) │
   │  (入力 / input assembly)               │                          │
   └──────────────────────────┘            └──────────────────────────┘
```

- 既定では受信した O→T を T→O ペイロードへ**ループバック**し、先頭 4 バイトに
  周期ごとに増加するハートビートを格納します（動作確認用。`--no-loopback` で無効）。

---

## 必要環境 / Requirements

- Ubuntu/Linux、C コンパイラ（`build-essential` 等）。ランタイム依存ライブラリなし。

```sh
sudo apt-get install build-essential
```

## ビルド / Build

```sh
make            # -> ./eip_adapter
```

## Ubuntu ホストで実行 / Run on the host

```sh
# 自分のNIC IP を指定して起動（マルチキャスト送出インターフェースにも使われる）
./eip_adapter --ip 192.168.1.50

# 設定ファイルで起動
./eip_adapter --config config/adapter.conf
```

`--ip` を省略すると、最初の TCP 接続時に自身の IP を学習します（単純構成なら省略可）。
同一サブネットのオリジネータから Forward Open すれば Class 1 通信が始まります。

> ファイアウォール（ufw 等）を使っている場合は、**TCP 44818・UDP 44818・UDP 2222** と
> マルチキャストを許可してください。例: `sudo ufw allow 44818 ; sudo ufw allow 2222/udp`

---

## ローカルでの動作確認 / Quick local test

EtherNet/IP の I/O は UDP/2222 を双方向に使うため、1 台のホストで試す場合は
オリジネータ側に別のループバックアドレス（`127.0.0.2`）を割り当ててポート競合を
回避します。

```sh
# 端末1: アダプタ起動
./eip_adapter --ip 127.0.0.1

# 端末2: 付属オリジネータ（スキャナ）で Class 1 通信
python3 tools/eip_originator.py --adapter 127.0.0.1 --local 127.0.0.2 \
        --seconds 3 --rpi-ms 50
```

期待される出力例:

```
RegisterSession OK, handle=0x00010000
ForwardOpen OK [exclusive]: O->T id=0x12340001  T->O id=0x20000001
Exchanging Class 1 I/O for 3.0s (RPI 50 ms)...
Received 60 T->O packets
Loopback OK: YES (embedded O->T counter=59, ours=60)
ForwardClose sent. Done.
```

`tools/eip_originator.py` は最小構成のオリジネータで、RegisterSession →
Forward Open → UDP I/O 周期交換 → Forward Close を行い、ループバックを検証します。

---

## スキャナ（オリジネータ）アプリ / Scanner application

本リポジトリには、アダプタと対になる **C 製の EtherNet/IP スキャナ
`eip_scanner`**（オリジネータ）も含まれます。Class 1 接続を開いて周期 I/O を
実行し、本アダプタと相互通信できます。

```sh
make                               # eip_adapter と eip_scanner を両方ビルド
./eip_scanner --target 192.168.1.50            # 既定: exclusive, 32/32B, RPI 50ms
```

主なオプション（`./eip_scanner --help` で全一覧）:

```
  --target IP        接続先アダプタ IP                        [必須]
  --local IP         送信元 IP（同一ホスト検証用）
  --conn-type T      exclusive | input-only | listen-only      [exclusive]
  --to-multicast     T→O をマルチキャストで要求
  --ot-size / --to-size   O→T / T→O バイト数                  [32 / 32]
  --rpi-ms N         要求 RPI（ミリ秒）                        [50]
  --out-inst/--in-inst/--cfg-inst   アセンブリ番号            [150/100/151]
  --no-ot-run-idle / --to-run-idle  run/idle ヘッダ
  --serial/--ot-cid  接続識別（複数スキャナ並行時に個別指定）
  --seconds N        実行時間（0 = 無限）                      [0]
```

### スキャナ ↔ アダプタ 相互通信（同一ホストでの確認）

UDP/2222 を双方向に使うため、同一ホストではスキャナに別 IP（`127.0.0.2`）を割当てます。

```sh
# 端末1: アダプタ
./eip_adapter --ip 127.0.0.1

# 端末2: スキャナ（exclusive, ユニキャスト）
./eip_scanner --target 127.0.0.1 --local 127.0.0.2 --rpi-ms 50

# マルチキャスト共有: owner と listen-only を別 IP で並行起動
./eip_scanner --target 127.0.0.1 --local 127.0.0.2 --to-multicast \
    --serial 0x01 --ot-cid 0x12340001 &
./eip_scanner --target 127.0.0.1 --local 127.0.0.3 --conn-type listen-only \
    --serial 0x03 --ot-cid 0x12340003
```

スキャナは毎秒ステータス（送受信数・受信した入力データ先頭）を出力します:

```
Forward Open OK [exclusive]: O->T id=0x12340001  T->O id=0x20000001
status: O->T sent=20  T->O recv=21  last input[0:4]=21
```

別ホストの実機どうしでも、同一サブネットにあれば `--target` を相手 IP にするだけで
通信できます（`--local` は省略可）。

---

## サービスとして常駐 / Install as a systemd service

```sh
sudo make install
sudo systemctl daemon-reload
sudo systemctl enable --now eip-adapter
sudo systemctl status eip-adapter
journalctl -u eip-adapter -f          # ログ
```

- バイナリ → `/usr/local/bin/eip_adapter`, `/usr/local/bin/eip_scanner`
- 設定 → `/etc/eip-adapter/adapter.conf`（既存があれば上書きしません）
- ユニット → `/lib/systemd/system/eip-adapter.service`

設定を変えたら `sudo systemctl restart eip-adapter`。アンインストールは
`sudo make uninstall`（設定ファイルは残します）。

> サービスは `DynamicUser=yes`（非 root の動的ユーザ）で動きます。ポートは
> いずれも 1024 超のため特権は不要です。`/etc/eip-adapter/adapter.conf` の
> `ip = <このホストのIP>` を設定しておくのが確実です。

---

## 接続種別 / Connection types

Class 1 のアプリケーション接続は 3 種類あり、Forward Open の接続パラメータから
自動判定します。

| 種別 | O→T（出力） | T→O（入力） | 説明 |
|---|---|---|---|
| **Exclusive-Owner** | データ (P2P) | 生産 | 出力を所有。1 台のみ |
| **Input-Only** | ハートビート (Null/0B) | 生産 | 入力のみ。出力なし。複数可 |
| **Listen-Only** | ハートビート (Null/0B) | 生産 (Multicast) | 共有マルチキャストを傍受。producer 必須 |

- **Exclusive-Owner** は 1 つだけ。2 つ目は `0x0106`（所有権競合）で拒否
- **Listen-Only** は共有マルチキャスト生産が無いと `0x0119` で拒否。全 producer が
  閉じると自動切断
- 出力イメージは Exclusive-Owner のみが更新、入力イメージは全 T→O 消費者へ共通生産

受理する種別は制限できます（拒否時 `0x0103`）:

```sh
./eip_adapter --no-input-only --no-listen-only      # Exclusive-Owner のみ
```

設定ファイルでは `allow_exclusive` / `allow_input_only` / `allow_listen_only`。

### マルチキャスト T→O / Multicast production

T→O が Multicast の場合、アダプタは 1 本のマルチキャストストリームを生産し、
Exclusive-Owner と複数の Listen-Only が共有します。

- CIP のアルゴリズムでグループ（既定 `239.192.x.x`）を割当て、Forward Open 応答の
  **T→O ソケットアドレス項目（CPF 0x8001）** でオリジネータへ通知
- 全消費者は同一 T→O 接続 ID・同一グループを受信（単一ストリーム）
- マルチキャスト送出は `--ip` で指定したインターフェースから（TTL=1）

各種別のテスト指定:

```sh
python3 tools/eip_originator.py --conn-type exclusive  --local 127.0.0.2
python3 tools/eip_originator.py --conn-type input-only  --local 127.0.0.2 --serial 0x02 --ot-cid 0x12340002
# マルチキャスト共有（owner を --to-multicast、Listen-Only を別IPで並行起動）
python3 tools/eip_originator.py --conn-type exclusive   --to-multicast --local 127.0.0.2 --serial 0x01 --ot-cid 0x12340001 &
python3 tools/eip_originator.py --conn-type listen-only  --local 127.0.0.3 --serial 0x03 --ot-cid 0x12340003
```

---

## パラメータ / Configurable parameters

EtherNet/IP では、パラメータごとに**決定する側**が異なります。

| パラメータ | 決定者 | アダプタ側の扱い |
|---|---|---|
| IP アドレス | アダプタ | `--bind` / `--ip` |
| アセンブリ インスタンス | アダプタ | `--out-inst` / `--in-inst` / `--cfg-inst` |
| 機器アイデンティティ | アダプタ | `--vendor-id` 他 |
| **RPI** | **オリジネータ**（Forward Open） | 受入れ。`--rpi-min-ms`/`--rpi-max-ms` で範囲検証 |
| **タイムアウト** | **オリジネータ**（RPI×倍率） | 受入れ。`--timeout-ms` で上書き |
| **通信バイト数** | **オリジネータ**（接続パラメータ） | 受入れ。`--out-size`/`--in-size` で固定・検証 |

RPI・タイムアウト・バイト数は本来オリジネータが指定する値で、既定では**そのまま
受け入れます**。`--out-size` 等を指定すると不一致を**拒否**します（CIP 拡張ステータス）。

| 拒否理由 | 拡張ステータス |
|---|---|
| O→T サイズ不一致 | `0x0127` |
| T→O サイズ不一致 | `0x0128` |
| RPI 範囲外 | `0x0111` |
| 所有権競合 | `0x0106` |
| Listen-Only に生産が無い | `0x0119` |
| 種別が許可されていない | `0x0103` |

### 設定ファイル / Configuration file

INI 形式（`key = value`、`#`/`;` コメント、`[section]` 無視）。優先順位は
**既定値 < 設定ファイル < コマンドライン**。キーは CLI ロング名の `-` を `_` に
したもの（例 `out-size` → `out_size`）。`config/adapter.conf` を参照。

```sh
./eip_adapter --config config/adapter.conf --product-name "Line-A Adapter"
```

### EDS ファイル出力 / EDS generation

現在の設定から **EDS（Electronic Data Sheet）** を生成します。

```sh
./eip_adapter --config config/adapter.conf --write-eds MyAdapter.eds
make eds                                   # -> Linux_EIP_Adapter.eds
```

`[File] [Device] [Device Classification] [Params] [Assembly] [Connection Manager]`
を含み、受理する接続種別（Exclusive-Owner / Input-Only / Listen-Only）の Class 1
接続定義が出力されます。

> EDS の細かなフィールドは一般的なテンプレートに基づきます。製品化時は EZ-EDS 等で
> 検証してください。

### コマンドラインオプション / Options（抜粋）

```
ネットワーク:    --ip A.B.C.D / --bind A.B.C.D
アセンブリ:      --out-inst N / --in-inst N / --cfg-inst N
接続ポリシー:    --out-size N / --in-size N / --rpi-min-ms N / --rpi-max-ms N / --timeout-ms N
接続種別:        --no-exclusive / --no-input-only / --no-listen-only
リアルタイム:    --no-ot-run-idle / --to-run-idle / --no-loopback
アイデンティティ: --vendor-id / --vendor-name / --device-type / --product-code / --serial / --rev / --product-name
設定/EDS:        --config FILE / --write-eds FILE / --rpi-ms N
その他:          --quiet / --help
```

完全な一覧は `./eip_adapter --help`。

### run/idle ヘッダ

出力（O→T）接続では 32bit run/idle ヘッダを付けるオリジネータが多いため既定で
有効です。相手がヘッダ無し（modeless）なら `--no-ot-run-idle` を指定してください。

---

## デフォルトの機器情報 / Default identity

| 項目 | 値 |
|------|----|
| Vendor ID | `0x1234` |
| Device Type | `12` (Communications Adapter) |
| Product Code | `0x0065` |
| Revision | `1.1` |
| Serial | `0x00C0FFEE` |
| Product Name | `Linux EIP Adapter` |
| 出力アセンブリ (O→T) | inst 150, 32 bytes |
| 入力アセンブリ (T→O) | inst 100, 32 bytes |
| 設定アセンブリ | inst 151 |

既定は `src/device.h`、起動時は CLI / 設定ファイルで上書きできます。

---

## 構成 / Layout

```
src/eip.h                 エンキャプスュレーション / CIP 定数, エンディアン補助
src/device.h              機器アイデンティティ / アセンブリ定義
src/eip_adapter.c         アダプタ本体（TCP/UDP サーバ + Class 1 I/O + 設定 / EDS 生成）
src/eip_scanner.c         スキャナ本体（オリジネータ、Class 1 I/O）
config/adapter.conf       設定ファイルのサンプル
packaging/eip-adapter.service  systemd ユニット
tools/eip_originator.py   テスト用オリジネータ（スキャナ）
Makefile                  build / install / eds
```

---

## 制限 / Notes & limitations

- Exclusive-Owner / Input-Only / Listen-Only に対応し、最大 8 接続まで同時保持します。
- T→O は point-to-point ユニキャストとマルチキャスト生産の両方に対応（マルチキャストは
  1 ストリームを複数消費者で共有、単一入力アセンブリ想定）。
- デバイスは単一の入出力イメージを持ち、出力は Exclusive-Owner のみが更新、入力は
  全 T→O 消費者へ共通生産します。
- 教育 / 検証用途の軽量実装であり、ODVA 認証取得品ではありません。

### セキュリティ / Security

- ネットワーク入力のパース（Encapsulation/CPF/CIP, Class 1 I/O）は受信実長に対して
  境界チェックを行い、領域外読み取りを防ぎます（ASan/UBSan + 異常入力ファジングで確認）。
- アイドルな TCP セッションは `--tcp-timeout-ms`（既定 0 = 無効）で切断できます。
  TCP には `SO_KEEPALIVE` も設定します。
- EtherNet/IP プロトコル自体には認証・暗号がありません。**信頼された隔離ネットワーク**で
  運用してください（ファイアウォール / セグメント分離を推奨）。
