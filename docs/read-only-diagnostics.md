# M1 只讀診斷與 C2 健康資料

這版可以燒進 NUCLEO-G474RE，讀取兩顆 M1 的設定、即時狀態及錯誤。
它不會激磁、不設定目標速度、不改 EEPROM、不重置警報，也不送 NMT 或 SYNC。
它不會替已在運轉的驅動器停車；請先用原廠工具停車並去致能，再接入診斷。

目前已有 firmware、AGX 監看工具與健康資料格式；尚未接到既有 C2 WebSocket/driver health。
本分支不包含 locomotion 的新底盤 driver，也不允許從診斷介面運轉車體。

## 建置與燒錄

Windows 上可使用已安裝 STM32CubeIDE 附帶的 GCC、CMake 與 Ninja：

```powershell
cd D:\stm32\M1CN016RE_Controller
powershell -ExecutionPolicy Bypass -File tools/build_diagnostics.ps1
```

腳本會自動尋找 `C:/ST/STM32CubeIDE*/STM32CubeIDE/plugins` 內的工具。
若自行安裝 GCC，將 `ARM_GCC_BIN` 指到 `arm-none-eabi-gcc` 所在目錄；CMake、Ninja 可放 PATH。

產物位於 `build/diagnostics/`：

- `m1_diagnostics.hex`：以 STM32CubeProgrammer、ST-LINK/SWD 下載；HEX 已含 Flash 位址。
- `m1_diagnostics.elf`：除錯與符號。
- `m1_diagnostics.bin`：純二進位，若使用它，Flash 起始位址為 `0x08000000`。

建置腳本會檢查 ELF 沒有連入 `m1_sdo_write`、RPDO、NMT、SYNC 等寫入/運轉 codec。
實際傳送端另有讀取封包白名單。診斷用 client SDO abort 只結束讀取傳輸，不改驅動器物件。

IAR 使用既有 `EWARM/Project.eww`，已將診斷來源加入 project，stack 調為 4 KiB。
本次實際驗證的是 GCC build；IAR build 仍需在有 IAR 的環境執行。

## 接線與預設值

- USB 接 NUCLEO 的 ST-LINK，使用其 VCP，115200、8N1、無流控。
- VCP 對應 LPUART1 PA2/PA3；USART1 PC4/PC5 不是這版的 console。
- CAN：PB9 TX、PB8 RX，經外接 3.3V 邏輯相容 transceiver 接至 CANH/CANL。
- Classic CAN、標準 11-bit ID、1 Mbps；不支援 extended ID 或 CAN FD 診斷。
- 預設診斷 node 1 與 2。它們是本程式預設值，不代表已確認實機 ID 或左右分配。
- 用原廠工具先確認兩顆 M1 的 ID 不相同、CAN 已啟用且速率一致；診斷程式不替你改設定。
- 檢查幹線兩端終端電阻及信號參考地；不要把 MCU TX/RX 直接接 CANH/CANL。

## 串列終端操作

開機先維持被動，不會自動查詢。會看到 `M1D1 BOOT`、`M1D1 INFO`。
每行命令按 Enter，支援 LF 或 CRLF：

```text
HELP
INFO
NODES 1 2
WATCH ON
PROBE
STATUS
```

| 命令 | 功能 |
| --- | --- |
| `NODES 1 2` | 設定本次診斷目標，不改 M1 的 node ID；會停止既有掃描/監看 |
| `WATCH ON` | 持續讀取兩顆的 error register、statusword、mode display、actual velocity |
| `PROBE` | 一次讀取識別、控制模式、CAN 設定、PDO/觸發方式及 Factor Group |
| `STATUS` | 立即輸出目前健康快照，不以查詢命令刷新舊資料 |
| `WATCH OFF` / `CANCEL` | 停止診斷請求，並結束未完成的 SDO upload |

不支援 `ACTIVATE`、`MOVE`、任意讀寫位址或原始 CAN 發送。
過長、非法字元或 UART overflow 會丟棄整行，不執行殘留尾段。
USER 按鈕只切換 LED，不控制馬達。

讀取超時為 200 ms；單節點失聯後退避 1 秒，避免一直阻擋另一顆。
健康快照每 500 ms 輸出；必要狀態資料超過 1000 ms 不能維持 healthy。
設定為同步/事件回覆的 TPDO 不一定會主動出現，本診斷透過 SDO 讀狀態，不為了取得 TPDO 送運轉 PDO。

## 如何看結果

`M1D1 READ` 包含 node、index、sub、result、值或 abort code。
識別字串用 `text_hex` 輸出，避免控制字元破壞串列資料格式。
部分 firmware 不支援某物件時會顯示 `abort`，不會因此認定馬達一定故障。
`PROBE result=complete` 只表示掃描結束，不表示所有讀取成功。

`M1D1 PROFILE` 的 `mapping=match` 只表示所需的 CiA402 CAN mapping entries 相符。
COB-ID、觸發方式及單位仍需另外核對；`units_verified=0` 不會自動改成 1。
速度回授保留為 `velocity_raw`，未驗證倍率前不標成 rpm 或車速。

`M1D1 HEALTH` 分開描述：

| 欄位 | 意義 |
| --- | --- |
| `link` | online / offline / unknown，只描述收到回覆的狀況 |
| `health` | healthy / impaired / unhealthy / unknown，依新鮮狀態與錯誤資訊判定 |
| `status`、`error`、`mode` | 原始 CiA402 狀態字、錯誤暫存器、模式；NA 表示無有效值 |
| `reason` | drive_fault、drive_warning、mapping_mismatch、no_response、status_unavailable、local_control、not_enabled 等 |
| `drive_ready` | 僅表示最新 CiA402 狀態符合 remote、PV、operation enabled，且未報故障/警告 |
| `motion_allowed` | 此診斷版本固定 0，不是 locomotion 可運轉的授權 |
| `status_age_ms`、`error_age_ms` | 各必要資料的年齡，不能由 heartbeat 或其他查詢延長 |

通訊在線而未致能可以是 healthy + drive_ready=0；未致能本身不是 fault。
斷線、CAN bus-off、fault、warning、mapping 不符、必要資料缺失會分別回報，不把它們一律當作硬體損壞。
Boot-up、bus unavailable、RX 資料遺失會使快取健康證據失效。
實體 STO/抱閘不能僅憑 digital inputs raw bits 推定；它們的接點功能仍需核對。

## AGX 即時監看

```bash
python3 -m pip install pyserial
python3 tools/m1_monitor.py --port /dev/ttyACM0 --nodes 1 2 --raw
```

Windows 可用 `python tools/m1_monitor.py --port COM7 --nodes 1 2 --raw`，換成實際 COM 號。
程式使用帶唯一 ID 的 `REQ`，依序確認 `INFO`、`NODES`、`WATCH ON`、`PROBE`；
不只依據 serial 寫入成功判定配置完成。stdout 是每 500 ms 一筆 JSON，raw 資料送 stderr。
不要讓終端機、監看工具和未來 driver 同時占用這個 serial port。

JSON schema 為 `m1-diagnostics-health/v1`，含兩顆 node 明細、aggregate health、
`evidence_level`、`driver_status`、`drive_ready` 與固定 false 的 `motion_allowed`。
若任一顆 unhealthy，aggregate 為 Unhealthy；任一必要資料未知時不會把整車顯示為正常。
serial 沒有新進度超過 1.5 秒、重複序號、MCU reboot、USB 斷線，都不能保留舊的正常狀態。
ACK 逾時/拒絕會附上 `command_error`，掃描結果在 `probe_result`；不能用持續收到 HEALTH 來掩蓋命令未確認。

## 命令執行確認

```text
REQ check-1 PING
M1D1 ACK id=check-1 cmd=PING status=complete reason=ok

REQ check-2 PROBE
M1D1 ACK id=check-2 cmd=PROBE status=accepted reason=pending
... READ / PROFILE ...
M1D1 ACK id=check-2 cmd=PROBE status=complete reason=ok
```

一般診斷操作有終端 ACK；PROBE 先 accepted，再回 complete/failed/cancelled。
部分讀取失敗時 reason=partial，PDO 不符時 reason=profile_mismatch；沒有成功讀取則 failed/no_valid_reads。
未帶 REQ 的終端命令也會回 ACK，但 id=none，不適合上位機交易配對。
這些回覆不等於物理動作完成：診斷版本收到 ACTIVATE/MOVE/停止/抱閘等運動操作會明確拒絕。
HOLD/STOP_ID/ESTOP_ID 的負面 ACK 會保留舊 token，供 newarchitecture 立即辨識不相容。
詳細規則與原有上位機契約見 [ACK 核對說明](m1-stm32-ack-contract.md)。

## C2 接法

此 repo 目前輸出健康資料，**尚未修改 C2 UI 或 runtime 的 WebSocket**。
既有 codebase 使用 `HealthReport` / `HealthEvidenceLevel`：

| diagnostic aggregate | codebase evidence | 只有 normal/error 兩態的 UI |
| --- | --- | --- |
| healthy | Healthy | normal |
| impaired | Impaired | error，顯示 warning 的原因 |
| unhealthy | Unhealthy | error，顯示 fault/失聯等原因 |
| unknown / stale | Unknown | error，顯示 unknown/stale；可新增灰色 unknown 狀態 |

未來 M1 driver 的 `report_health()` 應從單一 serial reader 的新鮮資料產生左右各一份 report，
帶上 component identity、last progress 與 stale_after；不要另開 monitor 搶 serial port，
也不要只使用「driver thread 有在跑」作為 M1 正常證據。

`normal` 只表示診斷條件下未發現驅動器故障，不保證可運轉、停止距離、STO 或抱閘正確。
locomotion 的啟動授權仍需完整幾何/單位/PDO/安全設定驗證及運轉狀態機。

## 台架驗收

1. 先不接 CAN，上電可收到 console；`STATUS` 為 unknown，`WATCH ON` 後超時回報失聯。
2. 接一顆已停車/去致能的 M1，指定正確 ID；該顆能讀出身份及狀態，另一顆維持失聯。
3. 接兩顆不同 ID 的 M1；確認身份、控制模式與 PDO/單位，保留 `--raw` 的完整紀錄。
4. 由原廠可控方式產生/解除 warning 或 fault，確認 status/error 及 JSON 隨之更新。
5. 拔 USB、斷 CAN、關閉其中一顆、reset STM32，確認 UI 資料來源不會停留 normal。
6. 這一階段不測車體運動。硬體通訊與 UART 實際接線、IAR 及故障注入尚未實測。

## 軟體回歸

```bash
cmake -S tests -B build/tests -DCMAKE_BUILD_TYPE=Debug
cmake --build build/tests
ctest --test-dir build/tests --output-on-failure
python3 -m unittest discover -s tests -p test_monitor.py -v
```

測試包括只讀傳送限制、SDO abort/timeout、字串分段與 toggle、過長輸入、錯誤/警告、
EMCY、PDO 不符、heartbeat 不得維持舊狀態、MCU reboot、CAN unavailable 及 host 端過期。
