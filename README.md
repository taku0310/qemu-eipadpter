# qemu-eipadpter

QEMU 上で動作する **EtherNet/IP アダプタ（CIP ターゲット機器）** アプリケーションです。
オリジネータ（PLC / スキャナ）からの **Class 1（インプリシット / 周期 I/O）通信**を
受け付け、リアルタイムにデータを交換します。

An EtherNet/IP adapter (CIP target) that runs as a normal Linux userspace
program — designed to boot inside a QEMU guest — and exchanges **Class 1
(implicit, cyclic I/O)** data with an originator (PLC / scanner).

---

## 機能 / Implemented protocol

| 層 | 内容 |
|----|------|
| Encapsulation (TCP/44818) | `RegisterSession` / `UnRegisterSession` / `ListServices` / `ListIdentity` / `SendRRData` |
| Discovery (UDP/44818) | `ListIdentity` ブロードキャスト応答 |
| CIP Connection Manager | `Forward Open` (0x54) / `Large Forward Open` (0x5B) / `Forward Close` (0x4E) |
| **Class 1 I/O (UDP/2222)** | O→T 受信（消費）, T→O 周期送信（生産）, シーケンス番号, 接続タイムアウト監視 |
| CIP オブジェクト | Identity (0x01), Assembly (0x04) の Get_Attribute_Single/All |

### データフロー / I/O semantics

```
        Originator (PLC/Scanner)                 Adapter (this app)
   ┌──────────────────────────┐            ┌──────────────────────────┐
   │  Forward Open  ───────────TCP/44818──▶ │  Class 1 接続を確立        │
   │                                        │                          │
   │  O→T data    ─────────────UDP/2222───▶ │  消費 (consume)           │
   │  (出力 / output assembly)              │                          │
   │                                        │                          │
   │  T→O data    ◀────────────UDP/2222──── │  生産 (produce, 周期 RPI) │
   │  (入力 / input assembly)               │                          │
   └──────────────────────────┘            └──────────────────────────┘
```

- **O→T (output)**: オリジネータが送る出力データをアダプタが消費します。
- **T→O (input)**: アダプタが入力データを RPI 周期で生産し送信します。
- 既定では受信した O→T データを T→O ペイロードに**ループバック**し、
  先頭 4 バイトには周期ごとに増加するハートビートカウンタを格納します
  （動作確認しやすくするためのデモ動作。`--no-loopback` で無効化可能）。

---

## ビルド / Build

```sh
make            # 通常ビルド -> ./eip_adapter
make static     # 静的リンク（QEMU initramfs 用に推奨）
```

必要なのは C コンパイラのみ（依存ライブラリなし）。

---

## ローカルでの動作確認 / Quick local test

EtherNet/IP の I/O は UDP/2222 を双方向に使うため、1 台のホストで試す場合は
オリジネータ側に別のループバックアドレス（`127.0.0.2`）を割り当てて
ポート競合を回避します。

```sh
# 端末1: アダプタ起動
./eip_adapter --ip 127.0.0.1

# 端末2: 付属のオリジネータ（スキャナ）で Class 1 通信
python3 tools/eip_originator.py --adapter 127.0.0.1 --local 127.0.0.2 \
        --seconds 3 --rpi-ms 50
```

期待される出力例:

```
RegisterSession OK, handle=0x00010000
ForwardOpen OK: O->T id=0x12340001  T->O id=0x20000001
Exchanging Class 1 I/O for 3.0s (RPI 50 ms)...
Received 60 T->O packets
Loopback OK: YES (embedded O->T counter=59, ours=60)
ForwardClose sent. Done.
```

`tools/eip_originator.py` は最小構成のオリジネータ実装で、RegisterSession →
Forward Open → UDP I/O 周期交換 → Forward Close を行い、T→O の受信と
ループバック内容を検証します。

---

## QEMU 上で動かす / Running under QEMU

### 1. initramfs を作成

静的バイナリを `init` として組み込んだ initramfs を生成します
（`busybox` と `cpio` が必要）。

```sh
# DHCP で IP を取得（既定）
sh scripts/make-initramfs.sh

# 固定 IP の場合
IP_MODE=static STATIC_IP=192.168.1.50/24 GATEWAY=192.168.1.1 \
    sh scripts/make-initramfs.sh
```

→ `build/initramfs.cpio.gz` が生成されます。

### 2. QEMU で起動

```sh
# user モード（ホストからの簡易確認用、設定不要）
KERNEL=/boot/vmlinuz-$(uname -r) MODE=user sh scripts/run-qemu.sh

# tap モード（実機 PLC との Class 1 通信に推奨）
sudo TAP=tap0 MODE=tap KERNEL=/path/to/bzImage sh scripts/run-qemu.sh
```

- **user モード**: ホストの `tcp/44818` と `udp/2222` をゲストへフォワードします。
  手軽ですが、NAT のためブロードキャスト探索（ListIdentity）は通りません。
- **tap モード**: ゲストが L2 で直接ネットワークに参加するため、双方向 UDP・
  ブロードキャスト・マルチキャストがすべて機能します。実際の PLC / スキャナと
  接続する場合はこちらを使用してください。

> `KERNEL` には QEMU で起動可能な Linux カーネル（bzImage）を指定してください。
> ホストの `/boot/vmlinuz-$(uname -r)` をそのまま使えることが多いです。

ゲスト内ではブート時に eth0 を立ち上げ、自身の IP を `--ip` に渡して
`eip_adapter` を起動します。あとは同一ネットワーク上のオリジネータから
Forward Open すれば Class 1 通信が始まります。

### 3. 同一 br0 でソフト PLC と Class 1 通信する

ソフト PLC（CODESYS Control / OpenPLC 等）の VM と、本アダプタの VM を
**同じブリッジ `br0`** に載せると、同一 L2 セグメントになり双方向 UDP・
マルチキャストがそのまま機能します。

```
[ ソフトPLC VM ] --tap0--\
                          br0 (host 192.168.77.1/24)
[ アダプタ  VM ] --tap1--/
```

**(1) ブリッジと tap を作成**（ホスト、root 必要）:

```sh
sudo sh scripts/setup-bridge.sh up          # br0 + tap0 tap1 を作成
#   soft PLC = 192.168.77.20 , adapter = 192.168.77.10 を推奨
```

**(2) アダプタ VM を tap1 に接続して起動**:

```sh
IP_MODE=static STATIC_IP=192.168.77.10/24 GATEWAY=192.168.77.1 \
    sh scripts/make-initramfs.sh
TAP=tap1 MAC=52:54:00:00:00:11 MODE=tap KERNEL=/path/to/bzImage \
    sh scripts/run-qemu.sh
```

**(3) ソフト PLC VM を tap0 に接続して起動**（各 PLC のイメージを利用）。
QEMU 例（MAC は VM ごとに必ず変える）:

```sh
qemu-system-x86_64 -m 1G -hda softplc.qcow2 \
    -netdev tap,id=n0,ifname=tap0,script=no,downscript=no \
    -device e1000,netdev=n0,mac=52:54:00:00:00:20
```

ゲスト内でソフト PLC に `192.168.77.20/24` を割り当て、EtherNet/IP スキャナで
アダプタ `192.168.77.10` へ Class 1 接続を構成します（アセンブリ番号・サイズを
一致させ、必要なら生成した EDS を登録）。

> **動作確認のコツ**: 実機 PLC を用意する前に、ホスト（br0 = 192.168.77.1）から
> 付属オリジネータで疎通確認できます:
> ```sh
> python3 tools/eip_originator.py --adapter 192.168.77.10 --local 192.168.77.1
> ```
> 後片付けは `sudo sh scripts/setup-bridge.sh down`。

> 補足: 同一ブリッジに載せる VM は **MAC を必ず個別**にしてください
> （`run-qemu.sh` は `MAC=` で指定、QEMU の e1000 既定 MAC は全 VM 同一のため衝突します）。

---

## パラメータの可変化 / Configurable parameters

### どのパラメータを誰が決めるか

EtherNet/IP では、パラメータによって**決定する側**が異なります。

| パラメータ | 決定者 | アダプタ側の扱い |
|---|---|---|
| IP アドレス | アダプタ | `--bind` / `--ip` で設定 |
| アセンブリ インスタンス ID | アダプタ | `--out-inst` / `--in-inst` / `--cfg-inst` |
| 機器アイデンティティ | アダプタ | `--vendor-id` 他で設定 |
| **RPI**（更新周期） | **オリジネータ** | Forward Open で受信。`--rpi-min-ms`/`--rpi-max-ms` で範囲検証・拒否可 |
| **タイムアウト** | **オリジネータ**（RPI×倍率） | 受信値を使用。`--timeout-ms` で上書き可 |
| **通信バイト数** | **オリジネータ**（接続パラメータ） | 受信値を使用。`--out-size`/`--in-size` で期待値検証・拒否可 |

RPI・タイムアウト・バイト数は本来オリジネータ（PLC/スキャナ）が Forward Open で
指定する値です。アダプタは既定では**それをそのまま受け入れます**（＝自動的に可変）。
`--out-size` などを指定すると、期待値と一致しない Forward Open を**正しく拒否**します
（実機アダプタとして適切な挙動。拒否時は CIP 拡張ステータスを返します）。

| 拒否理由 | 拡張ステータス |
|---|---|
| O→T サイズ不一致 | `0x0127` |
| T→O サイズ不一致 | `0x0128` |
| RPI が範囲外 | `0x0111` |
| 接続数の上限 | `0x0113` |

### オリジネータ側（テストツール）の可変パラメータ

`tools/eip_originator.py` 側では RPI・サイズを引数で指定します:
`--rpi-ms` / `--ot-size` / `--to-size` / `--out-inst` 等。

### コマンドラインオプション / Options

```
eip_adapter [options]

ネットワーク / アドレス:
  --ip A.B.C.D        ListIdentity で広告する自身の IP
  --bind A.B.C.D      ソケットを束ねるアドレス (既定 0.0.0.0)

アセンブリ インスタンス:
  --out-inst N        出力 (O→T) アセンブリインスタンス (既定 150)
  --in-inst  N        入力 (T→O) アセンブリインスタンス (既定 100)
  --cfg-inst N        設定アセンブリインスタンス       (既定 151)

Class 1 接続ポリシー (Forward Open を検証):
  --out-size N        O→T (消費) サイズを N バイトに固定・検証
  --in-size  N        T→O (生産) サイズを N バイトに固定・検証
  --rpi-min-ms N      RPI が N ms 未満の Forward Open を拒否
  --rpi-max-ms N      RPI が N ms 超の  Forward Open を拒否
  --timeout-ms N      O→T 無通信タイムアウトを上書き (既定: RPI×倍率)

リアルタイム形式 / 動作:
  --no-ot-run-idle    O→T データに 32bit run/idle ヘッダが無い場合に指定
  --to-run-idle       T→O データに 32bit run/idle ヘッダを付加する
  --no-loopback       O→T を T→O ペイロードへループバックしない

アイデンティティ (数値は 0x.. の 16進可):
  --vendor-id N       ベンダ ID       (既定 0x1234)
  --device-type N     デバイスタイプ  (既定 12)
  --product-code N    プロダクトコード(既定 0x0065)
  --serial N          シリアル番号    (既定 0x00C0FFEE)
  --rev MAJOR.MINOR   リビジョン      (既定 1.1)
  --product-name STR  製品名          (既定 "QEMU EIP Adapter")

その他:
  --quiet             ログを抑制
  --help              ヘルプ
```

設定例:

```sh
# 出力16B/入力8B、RPI 10〜100ms に制限、固定IPにバインド、機器情報を変更
./eip_adapter --bind 192.168.1.50 --ip 192.168.1.50 \
    --out-size 16 --in-size 8 --rpi-min-ms 10 --rpi-max-ms 100 \
    --out-inst 150 --in-inst 100 --cfg-inst 151 \
    --vendor-id 0xABCD --product-name "MyAdapter" --serial 0x11223344
```

### run/idle ヘッダについて

Class 1 の Connected Data には先頭に 16bit のシーケンスカウントが付き、
その後に **32bit run/idle ヘッダ**（bit0 = Run）が続く場合があります。
出力（O→T）接続ではこのヘッダを付けるオリジネータが多いため、既定で
`--ot-run-idle` 相当（有効）にしています。相手がヘッダ無し（modeless）の
場合は `--no-ot-run-idle` を指定してください。

---

## デフォルトの機器情報 / Default identity

| 項目 | 値 |
|------|----|
| Vendor ID | `0x1234` |
| Device Type | `12` (Communications Adapter) |
| Product Code | `0x0065` |
| Revision | `1.1` |
| Serial | `0x00C0FFEE` |
| Product Name | `QEMU EIP Adapter` |
| 出力アセンブリ (O→T) | inst 150, 32 bytes |
| 入力アセンブリ (T→O) | inst 100, 32 bytes |
| 設定アセンブリ | inst 151 |

既定値は `src/device.h` で、起動時はコマンドライン引数で上書きできます
（`--vendor-id` 等）。I/O サイズは既定では Forward Open の
Network Connection Parameters から動的に決まり、`--out-size`/`--in-size`
を指定した場合はその値に固定・検証されます。

> QEMU の initramfs では `ADAPTER_ARGS` 環境変数でこれらの引数を渡せます。例:
> `ADAPTER_ARGS="--out-size 16 --rpi-min-ms 10 --product-name MyAdapter" sh scripts/make-initramfs.sh`

---

## 接続種別 / Connection types

Class 1 のアプリケーション接続には 3 種類があり、Forward Open の接続パラメータ
（O→T / T→O の connection type とサイズ）から自動判定します。

| 種別 | O→T（出力） | T→O（入力） | 説明 |
|---|---|---|---|
| **Exclusive-Owner** | データ (P2P) | 生産 | 出力を所有。1 台のみ。再 Open は同一可 |
| **Input-Only** | ハートビート (Null/0B) | 生産 | 入力のみ受信。出力は持たない。複数可 |
| **Listen-Only** | ハートビート (Null/0B) | 生産 (Multicast) | 既存の producer が生産する入力を傍受。producer 必須 |

判定ロジック:
- O→T が Null または 0 バイト → 非所有（ハートビート）→ T→O が Multicast なら
  **Listen-Only**、それ以外は **Input-Only**
- O→T が実データ → **Exclusive-Owner**

セマンティクス:
- **Exclusive-Owner** は 1 つだけ。2 つ目は `0x0106`（所有権競合）で拒否
- **Listen-Only** は共有マルチキャスト生産が無いと `0x0119` で拒否。
  全 producer が閉じると Listen-Only も自動切断
- 出力イメージは Exclusive-Owner の O→T のみが更新し、入力イメージは全 T→O 消費者へ
  共通に生産されます（複数消費者モデル）

受理する種別は設定で制限できます（拒否時は `0x0103`）:

```sh
./eip_adapter --no-input-only --no-listen-only     # Exclusive-Owner のみ受理
```

設定ファイルでは `allow_exclusive` / `allow_input_only` / `allow_listen_only`。

### マルチキャスト T→O / Multicast production

T→O 接続種別（connection type）が **Multicast** の場合、アダプタは 1 本の
マルチキャストストリームを生産し、Exclusive-Owner と複数の Listen-Only が
これを共有します。

- アダプタが CIP のアルゴリズムでグループアドレス（既定 `239.192.x.x`）を割当て、
  Forward Open 応答の **T→O ソケットアドレス項目（CPF 0x8001）** でオリジネータに通知
- 共有する全消費者は同一の T→O 接続 ID・同一グループを受信（単一ストリーム）
- 最初の producer（Exclusive-Owner）が生産を確立し、Listen-Only はこれに join
- 全 producer が閉じると生産停止、Listen-Only も切断
- Listen-Only は **マルチキャスト生産が存在する**ことが前提（producer が P2P の場合は
  傍受できず `0x0119`）

テストツールでの各種別の指定:

```sh
# ユニキャスト
python3 tools/eip_originator.py --conn-type exclusive  --local 127.0.0.2
python3 tools/eip_originator.py --conn-type input-only  --local 127.0.0.2 --serial 0x02 --ot-cid 0x12340002

# マルチキャスト共有: owner を --to-multicast で起動し、Listen-Only を別IPで並行起動
python3 tools/eip_originator.py --conn-type exclusive   --to-multicast --local 127.0.0.2 --serial 0x01 --ot-cid 0x12340001 &
python3 tools/eip_originator.py --conn-type listen-only  --local 127.0.0.3 --serial 0x03 --ot-cid 0x12340003
```

> 注: マルチキャスト送信 TTL は既定 1（ローカルセグメント）です。ローカル単一ホストで
> 試す場合、テストツールは `--mcast-if`（既定 127.0.0.1）でループバック上の
> グループに join します。

---

## 設定ファイル / Configuration file

すべての設定を INI 形式のファイルにまとめられます（`config/adapter.conf` がサンプル）。
`key = value` 形式、`#` または `;` 以降はコメント、`[section]` 行は無視されます。

```sh
./eip_adapter --config config/adapter.conf
```

**優先順位**: 既定値 < 設定ファイル < コマンドライン引数。
つまり設定ファイルで基本値を書き、必要な項目だけ CLI で上書きできます。

```sh
# ファイルの product_name を CLI で上書き
./eip_adapter --config config/adapter.conf --product-name "Line-A Adapter"
```

設定キーは CLI のロング名のハイフンをアンダースコアにしたものです
（`out-size` → `out_size` など）。指定可能なキーは `config/adapter.conf` を参照。

QEMU の initramfs に組み込む場合:

```sh
CONFIG_FILE=config/adapter.conf sh scripts/make-initramfs.sh
# -> /etc/eip-adapter.conf として埋め込まれ、--config 付きで起動します
```

---

## EDS ファイル出力 / EDS generation

現在の設定（アイデンティティ・アセンブリ・サイズ・RPI）から **EDS（Electronic
Data Sheet）** を生成できます。スキャナ／構成ツール（RSNetWorx, EZ-EDS 等）への
登録に使用します。

```sh
# 設定ファイルから生成
./eip_adapter --config config/adapter.conf --write-eds MyAdapter.eds

# Makefile ターゲット（CONFIG / EDS で変更可）
make eds                                   # -> QEMU_EIP_Adapter.eds
make eds CONFIG=config/adapter.conf EDS=out.eds
```

生成される EDS には `[File]` `[Device]` `[Device Classification]` `[Params]`
`[Assembly]` `[Connection Manager]` セクションが含まれ、受理する各接続種別
（Exclusive-Owner / Input-Only / Listen-Only）の Class 1 接続定義（O→T / T→O の
サイズ・RPI・接続パス）が記述されます（`--no-*` で除外した種別は出力されません）。

> 注: EDS の細かなフィールド（Connection の trigger/transport ビットなど）は
> 一般的な Exclusive Owner テンプレートに基づいています。製品化の際は EZ-EDS 等の
> 公式ツールで検証・調整してください。

---

## 構成 / Layout

```
src/eip.h           エンキャプスュレーション / CIP 定数, エンディアン補助
src/device.h        機器アイデンティティ / アセンブリ定義
src/eip_adapter.c   本体（TCP/UDP サーバ + Class 1 I/O + 設定 / EDS 生成）
config/adapter.conf       設定ファイルのサンプル
tools/eip_originator.py   テスト用オリジネータ（スキャナ）
scripts/make-initramfs.sh QEMU 用 initramfs 生成
scripts/run-qemu.sh       QEMU 起動（tap/user, MAC 指定可）
scripts/setup-bridge.sh   br0 + tap 作成（ソフト PLC と同一L2に載せる）
Makefile
```

---

## 制限 / Notes & limitations

- Exclusive-Owner / Input-Only / Listen-Only に対応し、最大 8 接続まで同時保持します。
- T→O は point-to-point ユニキャストと **multicast 生産**の両方に対応します
  （マルチキャストは 1 ストリームを複数消費者で共有）。マルチキャスト生産は1グループ
  （単一入力アセンブリ）を想定しています。
- デバイスは単一の入出力イメージを持ち、出力は Exclusive-Owner のみが更新、入力は
  全 T→O 消費者へ共通に生産します。接続パスのアセンブリ番号は解析・ログ出力します。
- 教育 / 検証用途を想定した軽量実装であり、ODVA 認証取得品ではありません。
