# MPP Pipeline 代码分析报告

## 📋 文件概述

**文件**: `/home/ubuntu/workspace/big_core/mpp_pipeline/mpp_pipeline.c`

**目的**: K230 MPP 硬件管线 MVP（最小可行产品），验证单进程多线程模型下的视频采集→编码流程

**周期**: Week 1 队长任务（多进程架构延后）

---

## 🎯 核心设计架构

```
GC2093 传感器 (1920x1080@30fps, 10-bit)
    ↓ MIPI CSI
[VICAP 虚拟输入] (VI_CH0)
    ↓ 硬件绑定 (kd_mpi_sys_bind) 零CPU拷贝
[VENC 视频编码器] (CH0)
    ↓
[码流采集线程] → 打印 "Get NALU, Size: xxxx"
    ↓
日志输出 @ 15fps (目标帧率)
```

### 关键点
- **硬件Bind**（VI→VENC）: 采集的帧自动送入编码器，CPU不参与数据流
- **单进程多线程**: Main线程初始化 + Stream线程采集
- **内存模型**: VB池管理所有DMA缓冲区（无CPU memcpy）

---

## 🔧 初始化流程（5步）

### Step 1: VB池初始化
**函数**: `vb_init()`

```c
config.max_pool_cnt = 2;  // 两个池

Pool[0]: 输入帧缓冲 (FRAME_BUF_SIZE)
  - 块数: 6 块
  - 块大小: 4.0 MiB (对齐到4KB)
  - 模式: NOCACHE (绕过CPU缓存，防止一致性问题)

Pool[1]: 编码输出码流 (STREAM_BUF_SIZE)
  - 块数: 15 块
  - 块大小: 1.0 MiB
  - 模式: NOCACHE
```

**内存计算**:
- 1080P NV12 单帧 = 1920 × 1080 × 1.5 = 3,110,400 字节 ≈ 3.0 MB
- FRAME_BUF_SIZE = (1920×1080×2 + 0xFFF) & ~0xFFF = 0x3F5000 ≈ **4.0 MB**
- STREAM_BUF_SIZE = (1920×1080/2 + 0xFFF) & ~0xFFF = 0xFD800 ≈ **1.0 MB**
- **总占用**: 6×4MB + 15×1MB = **39 MB** (K230 DDR 512MB 充足)

### Step 2: VENC编码通道初始化
**函数**: `venc_init()`

```c
attr.venc_attr.type = K_PT_H265;           // 编码格式
attr.venc_attr.profile = VENC_PROFILE_H265_MAIN;
attr.venc_attr.pic_width = 1920;
attr.venc_attr.pic_height = 1080;

// 码率控制 (CBR)
attr.rc_attr.rc_mode = K_VENC_RC_MODE_CBR;
attr.rc_attr.cbr.src_frame_rate = 30;      // 传感器输入
attr.rc_attr.cbr.dst_frame_rate = 15;      // 目标输出
attr.rc_attr.cbr.bit_rate = 4000;          // 4 Mbps
attr.rc_attr.cbr.gop = 30;                 // I帧间隔 (2s)
```

关键决策:
- ✅ **H.265 Main Profile**: 硬件编码器标准配置
- ✅ **CBR 4000kbps**: 恒定码率，稳定网络传输
- ✅ **30→15fps** 帧率转换: 硬件自动下采样（不是丢帧）

### Step 3: VICAP配置（虚拟摄像头输入）
**函数**: `vicap_config()`

**传感器查询**:
```c
kd_mpi_vicap_get_sensor_info(GC2093_MIPI_CSI0_1920X1080_30FPS_10BIT_LINEAR, &sensor_info);
// 返回: GC2093, 1920x1080, 30fps, CSI_NUM=0, MIPI_LANES=2
```

**设备配置**:
```c
dev_attr.acq_win = {1920, 1080};           // 采集窗口
dev_attr.mode = VICAP_WORK_ONLINE_MODE;    // 实时模式
dev_attr.pipe_ctrl.ae_enable = 1;          // 自动曝光
dev_attr.pipe_ctrl.awb_enable = 1;         // 自动白平衡
```

**通道配置**:
```c
chn_attr.out_win = {1920, 1080};
chn_attr.pix_format = PIXEL_FORMAT_YUV_SEMIPLANAR_420;  // NV12
chn_attr.alignment = 12;                    // 4096字节对齐 (DMA要求)
chn_attr.buffer_num = 6;                    // 与VB池协调
chn_attr.buffer_size = 3MB;
```

### Step 4: 硬件绑定
**函数**: `vi_bind_venc()`

```c
k_mpp_chn vi_chn = {K_ID_VI, VICAP_CHN, VICAP_CHN};
k_mpp_chn venc_chn = {K_ID_VENC, 0, 0};
kd_mpi_sys_bind(&vi_chn, &venc_chn);
// 从此 VI采集的帧 → 自动流入VENC，零CPU干预
```

### Step 5: 启动数据流
**函数**: `vicap_start()`

```c
kd_mpi_vicap_start_stream(VICAP_DEV);  // 开始硬件采集
// VENC 自动开始接收 VI 输出的帧
```

---

## 📊 码流采集线程

**函数**: `stream_thread()`

### 工作流程
```c
while (g_running) {
  1. kd_mpi_venc_query_status()      // 查询有多少编码完成的pack
  2. malloc(pack_cnt)                 // 分配pack数组
  3. kd_mpi_venc_get_stream()         // 阻塞等待编码数据 (timeout=-1)
  4. 遍历pack, 打印非Header的NALU大小
  5. kd_mpi_venc_release_stream()     // 返还缓冲区给VENC复用
  6. free(pack)
}
```

### 关键指标收集
- 每帧NALU大小打印
- 每150帧 (~10秒) 统计一次:
  - 帧数
  - 平均FPS
  - 总字节数

### 输出示例（预期）
```
[MPP] Get NALU, Size: 2048 bytes  [frame #1]
[MPP] Get NALU, Size: 1876 bytes  [frame #2]
...
[MPP] === Stats: 150 frames in 10.0s, 15.0 fps, total 287400 bytes ===
```

---

## 🧹 清理函数关键设计

**函数**: `pipeline_cleanup()`

采用**状态机**模式保证清理顺序：

```
STATUS_RUNNING
    ↓ (自上而下清理)
STATUS_STREAM_STARTED   → kd_mpi_vicap_stop_stream()
STATUS_BINDED           → kd_mpi_sys_unbind()
STATUS_VICAP_INIT       → kd_mpi_vicap_deinit()
STATUS_VENC_STARTED     → kd_mpi_venc_stop_chn()
STATUS_VENC_CREATED     → kd_mpi_venc_destroy_chn() + kd_mpi_venc_close_fd()
STATUS_VB_INIT          → kd_mpi_vb_exit()
STATUS_IDLE
```

**关键**: 如果Step 3失败，cleanup自动跳过Step 4-5，避免操作未初始化的资源 → **防止死机**

---

## 🔍 可行性检查清单

### ✅ API 验证
- [x] `kd_mpi_vb_set_config()` - 存在
- [x] `kd_mpi_vb_init()` - 存在
- [x] `kd_mpi_venc_create_chn()` - 存在
- [x] `kd_mpi_venc_get_stream()` - 存在
- [x] `kd_mpi_vicap_get_sensor_info()` - 存在
- [x] `kd_mpi_sys_bind()` - 存在

### ✅ 内存模型
- [x] VB池两层设计 (输入+输出) 合理
- [x] 内存计算: 39MB < 512MB ✓
- [x] 对齐要求: 4KB对齐符合DMA规范
- [x] NOCACHE 模式适合硬件缓冲

### ✅ 硬件流程
- [x] 硬件Bind拓扑: VI → VENC 标准配置
- [x] H.265 编码: 硬件支持
- [x] 30→15fps 下采样: 硬件CBS (Constant Frame Rate) 支持
- [x] NV12 格式: 标准视频格式

### ⚠️ 潜在风险

| 风险 | 评估 | 建议 |
|------|------|------|
| **超时参数** | `kd_mpi_venc_get_stream()` 用 `-1` (阻塞) | 生产环境改为 5000ms，防止卡死 |
| **pack 查询** | `query_status.cur_packs` 可能为0 | 当前: 取max(1, cur_packs)，已处理 |
| **malloc失败** | 流线程中malloc如果OOM | 应预分配固定大小pack数组 |
| **信号安全** | `g_running` volatile 访问 | ✓ 使用volatile，符合C标准 |
| **编码延迟** | CBR+GOP30 可能导致延迟 | 现阶段 (MVP) 可接受，生产改GOP值 |
| **传感器不存在** | 若SENSOR_TYPE=50(GC2093)不可用 | 需确认开发板硬件 + SDK 驱动 |

---

## 🚀 预期验收结果

### Week 1 验收标准
- ✅ 持续打印 `"Get NALU, Size: xxxx bytes"` 每帧
- ✅ 帧率稳定 **15 fps**
- ✅ **10分钟不死机**
- ✅ 无内存泄漏

### 成功指标
```
[MPP] Get NALU, Size: 2048 bytes  [frame #1]
[MPP] Get NALU, Size: 1876 bytes  [frame #2]
...
[MPP] Get NALU, Size: 1920 bytes  [frame #150]
[MPP] === Stats: 150 frames in 10.0s, 15.0 fps, total 287400 bytes ===
```
持续10分钟，无卡顿、无崩溃。

---

## ⚙️ 构建方法

```bash
# 进入 big_core 目录
cd /home/ubuntu/workspace/big_core

# 设置环境变量
export RTT_EXEC_PATH="/home/ubuntu/k230_sdk/toolchain/riscv64-linux-musleabi_for_x86_64-pc-linux-gnu/bin"

# 编译
scons -j4

# 输出 ELF
# → build/big_app.elf
```

---

## 📝 总体评估

| 维度 | 评分 | 说明 |
|------|------|------|
| **代码质量** | ⭐⭐⭐⭐⭐ | 完整的初始化-运行-清理流程，状态机防崩溃 |
| **API 正确性** | ⭐⭐⭐⭐⭐ | 所有调用都对应K230 SDK，头文件验证通过 |
| **可行性** | ⭐⭐⭐⭐⭐ | 架构清晰，内存计算精确，硬件绑定合理 |
| **文档完整性** | ⭐⭐⭐⭐☆ | 注释详细，但缺少故障排查指南 |
| **生产可用性** | ⭐⭐⭐☆☆ | MVP阶段，超时参数、pack分配需优化 |

---

## 🎓 快速上手清单

1. **理解架构**: VB池 → VI(传感器) → [硬件Bind] → VENC → 码流输出
2. **记住5步初始化**: VB → VENC → VICAP → Bind → Start
3. **关键函数**: `kd_mpi_sys_bind()` (硬件自动转发)，`kd_mpi_venc_get_stream()` (阻塞等待编码)
4. **清理顺序**: 逆序（从Running → Idle），使用状态机防遗漏
5. **内存**: 39MB是总占用，完全在K230 DDR预算内
6. **验收**: 按1个月历时间看10分钟稳定运行，每秒15帧

---

**结论**: ✅ **代码高度可行，是Week 1 MVP的合理参考实现。推荐直接用作队长任务基础。**
