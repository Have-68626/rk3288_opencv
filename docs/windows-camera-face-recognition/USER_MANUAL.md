# Windows 摄像头人脸识别测试系统：用户操作手册

## 1. 用途与边界
- 本程序用于 Windows 10/11 x64 环境下验证：摄像头采集、实时预览、多脸识别、结构化日志、以及离线评估流程的可复现性。
- 默认不联网；识别库与日志仅落盘到本机 `storage/` 目录（可在配置文件中修改）。

## 2. 隐私提示
- 本程序会调用摄像头获取视频帧，并在本地进行人脸检测与识别。
- 如果摄像头无法打开，请点击窗口右上区域的“隐私设置”按钮，进入 Windows 摄像头隐私页面授权。

## 3. 快速运行
1. 先按 README/DEVELOP 的说明完成依赖与编译。
2. 运行可执行文件 `win_camera_face_recognition.exe`。
3. 在窗口顶部选择：
   - 摄像头（下拉框）
   - 分辨率（1080p/720p/480p）与 FPS（15/30/60）
   - 点击“应用相机”（切换会执行能力验证→重启采集→重置检测器；总耗时>300ms 或失败会自动回滚）
   - 预览比例（裁剪填充/信箱）
   - 翻转（翻转X/翻转Y）

## 3.1 显示设置面板（应用内实时切换）
窗口第二行提供“显示设置”面板，用于在不重启程序的前提下切换显示相关参数：
- 显示器：选择目标显示输出（多显示器环境下按系统输出名区分）
- 分辨率/刷新率：选择系统枚举到的模式组合（硬件能力决定可选范围）
- VSync：开启/关闭垂直同步
- 缓冲：SwapChain 双缓冲/三缓冲
- 全屏：切换窗口/全屏
- 系统模式：默认关闭；仅在用户显式勾选后，才会进行独占全屏与系统显示模式切换
- sRGB：默认开启；用于控制输出的色彩处理策略
- gamma / 色温(K)：渲染链路的显示增强参数（支持持久化）
- AA / Aniso：抗锯齿与各向异性过滤（按能力检测提供选项；默认关闭）

点击“应用显示”后，程序会执行：验证 → 应用 → 失败回滚。若切换失败，会自动回滚到上一次可用配置并弹窗提示。

## 4. 注册（Enroll）
1. 在“注册”左侧输入框填写人员 ID（例如 `alice`）。
2. 面对摄像头，点击“注册”。
3. 程序会自动采集多帧样本（默认 12 帧，可在 ini 调整），并更新本地库：
   - 默认路径：`storage/win_face_db.yml`

## 5. 检测（DNN）
- 默认使用 OpenCV DNN SSD(MobileNet) 人脸检测模型进行每帧推理，并以“最新帧优先”策略工作（在压力过大时会自动动态提高 stride 降级）。
- 画面叠加层显示：
  - 半透明白色圆角描边（2px，圆角 5px）
  - 置信度百分比（0%~100%）

## 5.1 模型配置（禁止硬编码路径）
- 配置文件：`config/windows_camera_face_recognition.ini`
  - `[dnn] model_path`：模型文件路径（支持相对路径）
  - `[dnn] config_path`：模型配置文件路径（如 pbtxt / prototxt）
- 环境变量覆盖（优先级高于 ini）：
  - `RK_WCFR_DNN_MODEL`
  - `RK_WCFR_DNN_CONFIG`

## 6. 日志与落盘
- 默认日志目录：`storage/win_logs/`
  - `recognition.csv`
  - `recognition.jsonl`
- 显示/渲染相关日志（用于闪屏诊断、72h 压测与基准报告）：
  - `render_metrics.jsonl`
  - `event_log.jsonl`
- 日志字段包括：时间戳、摄像头信息、帧序号、分辨率、FPS、人脸框、识别结果、距离、置信度、错误分类等。
- 日志带轮转：单文件超过 `max_file_bytes` 会按 `.1/.2/...` 滚动（数量上限 `max_roll_files`）。

## 6.2 输出端口（本地 HTTP + SSE + 外部 POST）
- 本地服务（默认 `127.0.0.1:8080`，可在 ini 或环境变量 `RK_WCFR_HTTP_PORT` 修改）：
  - `GET /api/faces`：返回最新检测结果 JSON
  - `GET /api/faces/stream`：SSE 持续推送最新检测结果（data: JSON）
- 外部推送（可选）：
  - ini `[poster] enable=1` 且配置 `post_url` 后，后台线程将以节流+退避重试方式 POST 推送 JSON（不阻塞预览）
  - 环境变量覆盖：`RK_WCFR_POST_URL`

### 6.2.1 /api/faces JSON 字段（简版）
- `timestamp_ms`：采集帧时间（毫秒）
- `frame`：源帧尺寸（像素）
- `preview`：预览区尺寸与比例模式（用于 `display_bbox` 口径）
- `perf`：`infer_ms` / `drop_rate` / `stride`
- `faces[]`：
  - `bbox`：源帧坐标
  - `display_bbox`：映射到预览区坐标（裁剪/信箱模式下会自动裁剪到可见区域）
  - `confidence`：0~1

## 6.1 指标口径说明
- 闪屏率与切换耗时的口径见 [DISPLAY_METRICS.md](DISPLAY_METRICS.md)

## 7. 常见问题
### 7.1 摄像头打不开
- 检查：Windows 设置 → 隐私和安全性 → 摄像头 → 允许应用访问摄像头
- 检查：是否有其他软件占用（会议软件/浏览器/安全软件）
- 如仍失败：将终端输出与 `storage/win_logs/recognition.jsonl` 放入 `ErrorLog/` 目录便于分析

### 7.3 需要进行稳定性/72h 压测
程序支持命令行模式（详见 DEVELOP.md 的复现命令）：
- 稳定性测试：遍历多组分辨率+刷新率组合并记录结果
- 72h 压测：定期输出内存/句柄/帧时间与异常事件日志

### 7.4 A/B/C 测试模式与报告
- A：`--test_a`（UI/布局/比例切换基础回归）
- B：`--test_b --test_b_repeats 10`（分辨率/FPS 组合切换回归）
- C：`--test_c --test_c_seconds 30`（HTTP + SSE 端口回归）
- 报告目录：默认 `docs/windows-camera-face-recognition/`，可用 `--test_report_dir` 修改

### 7.2 识别效果差
- 增加注册样本数量（不同光照/角度）
- 调整 `identify_threshold`（降低阈值可降低误识，但可能增加拒识）
- 使用更大的人脸尺寸（靠近镜头、提高分辨率、提高 `min_face_size_px`）
