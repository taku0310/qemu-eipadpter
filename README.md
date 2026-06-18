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

## 構成 / Layout

```
src/eip.h           エンキャプスュレーション / CIP 定数, エンディアン補助
src/device.h        機器アイデンティティ / アセンブリ定義
src/eip_adapter.c   本体（TCP/UDP サーバ + Class 1 I/O エンジン）
tools/eip_originator.py   テスト用オリジネータ（スキャナ）
scripts/make-initramfs.sh QEMU 用 initramfs 生成
scripts/run-qemu.sh       QEMU 起動
Makefile
```

---

## 制限 / Notes & limitations

- 単一の I/O 接続を主用途とし、最大 8 接続まで保持します。
- T→O は point-to-point ユニキャスト送信です（マルチキャスト生産は未対応）。
- 接続パスのアセンブリ番号は解析・ログ出力しますが、I/O 自体は単一の
  入出力イメージにマッピングします。
- 教育 / 検証用途を想定した軽量実装であり、ODVA 認証取得品ではありません。
