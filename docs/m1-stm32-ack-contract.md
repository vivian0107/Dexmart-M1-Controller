# M1 STM32 回覆與 newarchitecture 核對

狀態：只讀診斷 ACK 已實作；運轉命令仍不支援。尚未連到 C2 WebSocket 或實機驗證。

## 既有上位機實際會確認什麼

| 操作 | newarchitecture 現況 | M1 診斷版本回覆 |
| --- | --- | --- |
| ACTIVATE | `UgvSerialBridge::activate_and_wait_ready()` 等待指定 ready 字串；原本失敗會送 RESTART | 明確拒絕，不回 ready；bridge 立即回 Unsupported，不自動 RESTART |
| HOLD / STOP_ID / ESTOP_ID | `confirmed_stop()` 要求相同 command/token 的 `[STOP_ACK] ... APPLIED`；CAN_FAILED 則失敗 | 回相同 token 的 read_only 拒絕；bridge 立即回停止未確認，不重試假成功 |
| RESTART | 啟動失敗復原流程會等待舊版啟動提示 | 回 read_only 拒絕；bridge 不把它解讀為重啟完成 |
| 位置初始化 | `position_exchange()` 等待 STOP_ID、BRAKE、ENCODER、POSITION 等指定格式；四輪 node 0A..0D、TPDO4 | 明確拒絕；UGV02 API 立即失敗，不進入參數 recovery 或運轉 |
| MOVE | 舊 bridge 只確認 UART 寫入，沒有逐筆馬達 ACK | 明確拒絕；不得以舊 MOVE 的寫入成功當作 M1 已接受速度 |
| motor/encoder health | 舊 API 有自己的 serial reader 與事件解析 | 使用 M1D1 HEALTH，需獨立的 M1 driver 解析，不能當作四輪 encoder 資料 |

`stop_ack_enabled=false` 是既有舊韌體的非確認模式，不是 M1 相容開關。
它不能把診斷韌體變成可運轉控制器，也不能作為物理停止成功的證據。

參考程式：

- `src/drivers/robot/kuang_shin/ugv01/include/drivers/robot/kuang_shin/ugv01/serial/UgvSerialBridge.hpp`
- `src/drivers/robot/kuang_shin/ugv01/include/drivers/robot/kuang_shin/ugv02/api/KuangShinUgv02Api.hpp`

## M1 診斷 request / ACK

格式：`REQ <id> <command>`。ID 為 1–32 字元，只接受英數字、`_`、`-`、`.`，不使用保留字 `none`。
上位機每次連線/重啟產生新 session nonce，每筆命令使用不同 ID。

```text
REQ agx-1 INFO
M1D1 INFO firmware=m1-diag-2 ... read_only=1 motion_allowed=0 ack=1
M1D1 ACK id=agx-1 cmd=INFO status=complete reason=ok

REQ agx-2 PROBE
M1D1 ACK id=agx-2 cmd=PROBE status=accepted reason=pending
... READ / PROFILE / HEALTH ...
M1D1 ACK id=agx-2 cmd=PROBE status=complete reason=ok

REQ agx-3 ACTIVATE
M1D1 ACK id=agx-3 cmd=ACTIVATE status=rejected reason=read_only
```

ACK 的 `cmd` 是命令第一個字，例如 WATCH ON / WATCH OFF 都是 `cmd=WATCH`，由唯一 ID 配對完整原命令。

| status | 意義 |
| --- | --- |
| accepted | 非同步工作已排入，尚未完成；目前僅 PROBE 使用 |
| complete | 這筆診斷操作完成；不代表 M1 健康、可運轉或輪子靜止 |
| failed | CAN 無法使用、沒有有效讀取等失敗 |
| rejected | 不支援、唯讀限制、參數錯誤或 busy；沒有執行該操作 |
| cancelled | 原 PROBE 已由 CANCEL / WATCH OFF / NODES 中止 |

PROBE 最終 reason：`ok` 表示讀取無失敗；`partial` 表示部分讀取失敗/節點跳過；
`profile_mismatch` 表示讀回的 PDO entries 不符；`failed/no_valid_reads` 表示沒有任何成功讀取。
即使 reason=ok，仍需看 PROFILE、COB-ID、觸發方式與單位，不能直接開放 locomotion。

PING、INFO、STATUS、HELP、NODES、WATCH、CANCEL 都有終端 ACK。
命令不帶 REQ 仍可在終端操作，ACK 的 id=none；上位機不應以 none 作為交易識別。
HOLD/STOP_ID/ESTOP_ID 的舊格式特別保留原 token 到負面 ACK，讓舊 bridge 能立即辨識不相容。
M1 不會輸出假的 `[STOP_ACK] ... APPLIED`、ready 或四輪位置資料。

## 上位機驗證規則

`tools/m1_monitor.py` 已改成 INFO -> NODES -> WATCH ON -> PROBE 的逐筆確認流程：

1. 只有 ID 與 command 都相符的 ACK 才能完成該筆請求。
2. 一般 ACK 最多等 1.5 秒；PROBE 收到第一次 accepted 後最多等 30 秒。
3. 健康資料、READ log、舊 ACK、重複 accepted 不延長等待期限。
4. 遲到、錯 ID、錯 command、未先 accepted 就 complete 的 PROBE 不算成功。
5. 拒絕、失敗、取消、逾時會回報 command_error，不維持正常狀態。
6. MCU 重啟後清除快取及舊 session，重新握手。配置確認不等於馬達回授的新鮮度確認。

JSON 另帶 `command_error` 與 `probe_result`；driver_status=normal 仍不等於 motion_allowed。
只讀版本 `motion_allowed` 固定 false。

## 驗證範圍

- firmware：ID 配對、busy/cancel、部分讀取失敗、PDO 不符、完全失聯、運動命令拒絕。
- AGX：逐筆 ACK、錯/舊 ID、超時、重複 accepted、取消/失敗及健康資料不能掩蓋 ACK 失敗。
- C emitter -> Python：實際 ACK 字串可完成對應的診斷 session。
- newarchitecture：ACTIVATE 拒絕不 RESTART、三種停止拒絕不算 APPLIED、舊 ready/APPLIED 仍有效、位置初始化不進行 recovery。
- GCC 產物已重新建置；實機、IAR、C2 WebSocket 及完整 locomotion driver 尚未驗證/實作。
