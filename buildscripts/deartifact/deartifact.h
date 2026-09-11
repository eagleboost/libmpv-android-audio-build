#ifndef DEARTIFACT_H
#define DEARTIFACT_H

#include <stdint.h>
#include <stdbool.h>

/* ---------------------------------------------------------------------------
 * De-Artifact Filter —— 针对低码率 MP3 编码伪影的实时修复滤镜。
 *
 * 设计文档：VoyTrace tasks/audio-restoration-voice-restore.md §8
 * 实测参数依据（32 kb/s MP3 样本）：
 *   - 伪影主战场 4–11kHz（尤其 5–9kHz）
 *   - 5–9kHz 帧间 flux：伪影 57.3% vs 语音基带基线 36.8% → 阈值 42–50%
 *   - >14kHz 基本无能量，不处理
 *
 * 管线：STFT(N=1024, Hann, hop=512)
 *   ① mid 通道 5–9kHz 带内能量相对帧间 flux → artifact 判定
 *      （连续 2 帧触发 + ~46ms 释放防抖）
 *   ② 4–11kHz 逐 bin Artifact Mask（5–9k 权重 1.0，边缘线性降到 0.3）
 *   ③ 只对 mask>0 的 bin：频率 3-tap 平滑 + 时间 0.7/0.3 平滑
 *      + 增益衰减（LOW 6dB / HIGH 12dB）
 *   ④ De-Esser（仅 HIGH）：5–8kHz 齿音频段动态压低
 *   ⑤ 动态低通兜底：artifact 帧 cutoff 收到 LOW 13k / HIGH 11k，
 *      cutoff 帧间平滑防 zipper noise
 *   ISTFT（WOLA，window² 归一化）→ 输出
 *
 * 流式语义：STFT 固有 (N-H)=512 样本延迟。输出流 = 前 512 个样本直通 +
 * 之后逐帧处理的 WOLA 结果；流结束时用 deartifact_flush 排出尾部残留。
 * 注意：处理结果相对输入整体延迟 512 样本（~11.6ms @44.1k），前缀为
 * 未处理的原始样本。
 *
 * 安全性：strength=OFF 或任何内部异常（NaN/参数非法）→ 直通，绝不静音。
 * 状态：seek/切歌调 deartifact_reset()。
 * 线程模型：非线程安全（mpv af 音频滤镜单线程调用，原型期够用）。
 * ------------------------------------------------------------------------- */

typedef enum {
    DEARTIFACT_OFF = 0,
    DEARTIFACT_LOW = 1,
    DEARTIFACT_HIGH = 2,
} deartifact_strength_t;

#define DEARTIFACT_FRAME 1024   /* STFT 帧长 */
#define DEARTIFACT_HOP    512   /* 50% overlap */
#define DEARTIFACT_MAX_CH   8   /* 支持的最大声道数 */

/* 创建实例。失败返回 NULL。 */
void *deartifact_create(int sample_rate, int channels, int strength);

/* 释放实例。NULL 安全。 */
void deartifact_destroy(void *ctx);

/* 处理一块 interleaved float PCM：读入 frames 个样本帧，写出 frames 个
 * 样本帧到 out（延迟流，见文件头说明）。in/out 可相同（OFF 直通时安全）。
 * 返回 0 成功；参数非法返回 -1（此时 out 未定义，调用方应直通）。 */
int deartifact_process(void *ctx, const float *in, float *out, int frames);

/* 排出流末尾残留 FIFO（最多 max 个样本帧），返回实际排出数。
 * 流结束（EOF/暂停冲刷）时调用。 */
int deartifact_flush(void *ctx, float *out, int max);

/* 当前 FIFO 中待排出的样本帧数（诊断用）。 */
int deartifact_pending(void *ctx);

/* 诊断：输出 [总帧数, artifact 激活帧数, de-esser 激活帧数]。 */
void deartifact_debug_stats(void *ctx, uint64_t *out3);

/* 清空全部帧状态（flux 历史 / 上一帧频谱 / cutoff / FIFO / 累积器）。
 * seek、切歌、变速时调用，避免跨段串扰。 */
void deartifact_reset(void *ctx);

#endif
