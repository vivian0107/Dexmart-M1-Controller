# M1：C2 Remote Control 至左右差速驅動

軟體控制鏈已實作，尚未燒錄或驗證實車。實機 node ID、轉向、速度比例、CAN 收發器與 Control Mode 均由使用者回覆為「未確認」，所以交付設定的 `commissioning_verified` 為 false，不能啟動馬達。

## 控制鏈

```text
C2 Remote Control（既有 20 Hz joystick_command / joystick_brake）
  → 既有 WebsocketAdapterNode / ROS manual velocity
  → 既有 runtime 的手動所有權、token、locomotion Move capability
  → dexmart_m1_ugv / M1Api（SI：m/s、rad/s）
  → ST-LINK USB VCP，115200 8N1，M1M1 版本協議
  → NUCLEO-G474RE 的 m1_motion
  → Classic 11-bit CAN，1 Mbps，CiA402 SDO
  → 左右 M1，Profile Velocity（6060 = 3）
```

沒有更動 C2 的搖桿事件或舊 UGV 的控制協議。M1 使用獨立機型，不能把它宣告成四輪 `kuang_shin_ugv02`。
前進是正 vx，後退是負 vx；正 vyaw 左轉，負 vyaw 右轉；不支援橫移。左右輪分別使用可校正的正負方向。
C2 的右方向鍵／yaw 滑桿右推會送正 normalized yaw，因此本機型設定 `manual_yaw_sign=-1`，在手動輸入邊界轉成向右轉。使用前後搖桿與 yaw 滑桿，或上下／左右方向鍵；A/D 的橫移軸不適用此差速底盤。
幾何採用使用者提供的輪徑 0.440 m、減速比 10、輪距 0.739 m。兩輪同比例限幅保留轉彎曲率，最高不超過設定的馬達 rpm，也不超過 4000 rpm。
初始設定只開放 0.15 m/s、0.3 rad/s 的手動輸入與 100 motor rpm 上限；這不是 4000 rpm 的實車測試授權。

## 已完成行為

- M1 驅動器以 SDO 存取 6040、6060、60FF、6083、6084、6085，逐筆確認左右回覆；不依賴未確認的 PDO mapping，也不寫 EEPROM、Factor Group 或自動清故障。
- ARM 先讀 RAM Control Mode = 5、60A9、6096:1/2，核對與校正設定一致；再確認兩輪 disabled、無錯誤且速度為零，寫入零目標／速度模式／加減速，依 CiA402 狀態逐步使能。
- MOVE 回覆 `targets_written` 代表左右目標均收到 SDO ACK，不代表已達速度。SDO 左右更新是依序進行，不保證同一硬體時刻生效。
- STOP 等待零目標寫入之後讀回的左右 606C = 0 及 6041 bit 12；回覆 `standstill_feedback` 只代表驅動器回報靜止。DISARM／ESTOP 再確認兩輪 disabled。
- STM32 非零命令 500 ms 未更新、SDO 100 ms 未回覆、回授過期、EMCY、驅動器重啟、CAN bus-off 或 UART 丟資料都會鎖住運動，並盡力送兩輪零速／Quick Stop。需要明確 DISARM → ARM 才可重新運轉。
- 主機驗證 session、遞增命令序號、命令名稱及 accepted → complete；命令確認失敗會阻擋後續運動。STOP 能中止尚未完成的 ARM。
- 使用既有手動控制所有權與 300 ms 輸入 watchdog。沒有新增繞過 runtime 的搖桿直通捷徑。
- 左右健康與安全條件進入既有 runtime。C2 的 M1 顯示為 Left M1 / Right M1；未設定、失聯、過期不會顯示 OK。OK 表示驅動器健康，不表示已啟動。
- 僅完成手動控制：位置 odometry 明確無效，既有 encoder guard 會阻擋自主任務；沒有虛構位置、電池百分比或六輪狀態。

STM32 main loop 中的 watchdog 無法處理 MCU 完全掛住，也無法在 CAN 線已斷時把停止指令送進 M1。上電、重啟與斷線的獨立停車／斷能保護必須在實機驗證；不能假設泛用 CANopen 物件表中的 consumer heartbeat 在此 M1 韌體可用。

## 建置與啟動

STM32 repo `D:/stm32/M1CN016RE_Controller`：

```powershell
powershell -ExecutionPolicy Bypass -File tools/build_diagnostics.ps1 -Motion -BuildDirectory build/motion
```

產物是 `build/motion/m1_motion.hex`，與原只讀 `build/diagnostics/m1_diagnostics.hex` 分開。此次沒有燒錄。EWARM 原工程仍建置診斷版；運動版目前以 GCC/CMake 建置。
沿用現有接腳：PB9 CAN TX、PB8 CAN RX，須外接合適收發器；ST-LINK VCP 使用 PA2/PA3。實際 CAN 線、共地與終端電阻待現場確認。

AGX 在 newarchitecture workspace：

```bash
colcon build --packages-up-to laki_apps_ros_runtime_runner laki_apps_ros_robot_bringup
source install/setup.bash
ros2 launch laki_apps_ros_robot_bringup robot.launch.py config:=dexmart_m1_ugv.yaml
```

沿用現有 C2 URL 與機器人註冊／金鑰設定，必要時使用既有 `server_url:=...`。
啟動後先取得手動控制權（ENGAGE），再 ACTIVATE；確認完成後才送搖桿。
尚未完成下列實機設定與電池決策前，以上命令是連線／錯誤診斷入口，不能視為可直接開車的部署命令。

## 實機設定仍需完成

修改 `src/apps/ros/robot_runner/config/dexmart_m1_ugv.yaml`，每個值均應來自實際設備讀回與獨立量測：

| 設定 | 所需依據 |
| --- | --- |
| left_node_id / right_node_id | 左右實際 CAN ID，1–127 且不可相同；不要照抄測試的 1、2 |
| left_forward_sign / right_forward_sign | 各輪正 raw 轉動時的實際方向；+1 或 -1 |
| velocity_units_numerator / velocity_rpm_denominator | `raw = motor_rpm × numerator / denominator`；需要獨立轉速／輪轉量測，不能只用同一組 raw 回授自證 |
| profile_acceleration_raw / profile_deceleration_raw / quick_stop_deceleration_raw | 已確認單位與停車行為的原始值，皆須正值 |
| expected_si_velocity | 實機讀回 60A9:0，支援十進位或 0x... |
| expected_velocity_factor_numerator / denominator | 實機讀回 6096:1 / :2 |
| commissioning_verified | 以上項目、馬達匹配、Control Mode 5、剎車與獨立斷能／失聯停止完成確認後才可設 true |

先用只讀診斷版 `PROBE`／原廠工具收集設定。初次方向／比例量測應由現場人員在架高、低速與可立即斷能的條件下進行；測試程式的假驅動器數值不是實車預設。
不得同時執行 Python 診斷 monitor 與運動 runtime 搶用同一個串列埠。

## 尚未解決的電池啟動限制

新控制器沒有電池感測來源。既有 production readiness 要求電池 snapshot，safety policy 也會把缺少電池資料判為 RestrictedOperation，進而阻擋 SelfCheck／啟動。
已備妥 `robot_readiness_m1_remote.yaml` 及可選的 `robot_readiness_policy_path` launch 參數，但單獨使用它仍不會消除 safety 的電池限制。
將缺少電池資料視為 Nominal 的策略修改已被自動審核拒絕，**沒有套用**。需接入真實電池資料，或由使用者明確核准僅供外部供電調試的策略；不會以假的 100% 電量通過自檢。

## 驗證

```powershell
powershell -ExecutionPolicy Bypass -File tools/test_m1_remote_control.ps1
```

此命令用唯讀 mounts，在本機 Docker `laki-ros-app:latest` 建置實際 STM32 C 控制器，透過 Linux PTY 與正式 C++ serial/API/Move capability 交換資料。未連線實體馬達。
涵蓋四方向、停止回授、控制所有權、主機／STM32 watchdog、錯 session ACK、ARM 被 ESTOP 中止、單側掉線、SDO abort、配置錯誤與 session 更新。
另已建置 C2 前端，執行新舊 Driver State 測試及 ROS launch 回歸測試。

尚未驗證：現場 M1 的 SDO 速度控制／狀態行為、真實 CAN 時序、USB 中斷、剎車距離、電源／硬體急停、AGX 新映像與 C2 實際網路操作。
最終驗收必須在 C2 頁面逐項確認前進、後退、左右轉、放開回零、STOP、ESTOP、拔 USB／單側 CAN，以及故障後不自動恢復運轉。
