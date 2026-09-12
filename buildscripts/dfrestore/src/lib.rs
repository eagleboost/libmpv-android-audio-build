//! dfrestore —— DeepFilterNet3 实时语音恢复的 C ABI wrapper。
//!
//! 面向 VoyTrace 的 mpv audio filter（af_dfrestore.c）。底层复用官方
//! deep_filter crate 的 tract ONNX 推理（纯 Rust，无 torch 依赖），模型
//! 为内嵌的 DeepFilterNet3（default-model feature，编进 .so，免分发）。
//!
//! 架构（v1.4.4 起的大栈工作线程）：
//! - Android bionic 线程默认栈远小于桌面（mpv 音频线程上跑 tract 模型
//!   编译的深递归会爆栈 → SIGSEGV，实测手机上开启 Restore 后 0.6-2s
//!   崩溃，桌面 Windows 同代码正常）。因此全部重活（初始化 + 逐帧
//!   推理）都跑在本 wrapper 自建的 32MB 栈工作线程上，C ABI 保持同步。
//! - 底层 DfTract 是 mono 的，每声道一个实例。
//! - 底层 process 要求每帧恰好 hop_size（48kHz 下 960 样本，20ms）；
//!   wrapper 内部缓冲任意长度输入，凑满一帧才推理。
//! - 输出相对输入有固有延迟（算法 lookahead），流式语义：预热期直通，
//!   之后消费 FIFO。
//! - 任何错误（初始化失败、推理 panic）绝不静音：初始化失败返回 NULL，
//!   运行期错误直通。
//!
//! 定版配方（盲听收敛，双集验证）：DeepFilterNet3 + atten_lim 12dB +
//! post-filter beta 0.05。

use df::tract::{DfParams, DfTract, ReduceMask, RuntimeParams};
use ndarray::{ArrayView2, ArrayViewMut2};
use std::collections::VecDeque;
use std::ffi::c_float;
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::sync::mpsc::{channel, Sender};

const MAX_CHANNELS: usize = 8;
const WORKER_STACK: usize = 32 * 1024 * 1024;

// ---- 日志：Android → logcat（tag "dfrestore"），其它平台 → stderr ----
#[cfg(target_os = "android")]
#[link(name = "log")]
extern "C" {
    fn __android_log_print(
        prio: i32,
        tag: *const std::ffi::c_char,
        fmt: *const std::ffi::c_char,
        ...
    ) -> i32;
}

#[cfg(target_os = "android")]
fn dflog(msg: &str) {
    use std::ffi::CString;
    let tag = CString::new("dfrestore").unwrap();
    let fmt = CString::new("%s").unwrap();
    let cmsg = CString::new(msg.replace('\0', " ")).unwrap();
    unsafe {
        __android_log_print(4 /* INFO */, tag.as_ptr(), fmt.as_ptr(), cmsg.as_ptr());
    }
}

#[cfg(not(target_os = "android"))]
fn dflog(msg: &str) {
    eprintln!("[dfrestore] {msg}");
}

/// 工作线程命令。
enum Command {
    /// 处理一帧（各声道输入，hop 长度），返回各声道输出。
    Process(Vec<Vec<f32>>, Sender<Vec<Vec<f32>>>),
    /// 重置全部状态。
    Reset,
}

/// 工作线程持有的状态（只在工作线程上访问）。
struct Worker {
    states: Vec<DfTract>,
    channels: usize,
    hop: usize,
    atten_lim_db: f32,
    pf_beta: f32,
}

impl Worker {
    fn new(channels: usize, atten_lim_db: f32, pf_beta: f32) -> Result<Self, String> {
        dflog(&format!("init: ch={channels} atten={atten_lim_db} beta={pf_beta}"));
        let mut states = Vec::with_capacity(channels);
        for ch in 0..channels {
            // 与 Python torch 推理对齐：不做 lsnr 跳级（零掩码会过度压制
            // 弱语音段），每声道独立 mask。
            let r_params = RuntimeParams::default_with_ch(1)
                .with_thresholds(-100.0, 100.0, 100.0)
                .with_mask_reduce(ReduceMask::NONE)
                .with_atten_lim(atten_lim_db)
                .with_post_filter(pf_beta);
            dflog(&format!("init: loading default model (ch {ch})..."));
            let df_params = match catch_unwind(AssertUnwindSafe(DfParams::default)) {
                Ok(p) => p,
                Err(e) => {
                    let msg = format!("DfParams::default panicked: {e:?}");
                    dflog(&msg);
                    return Err(msg);
                }
            };
            dflog(&format!("init: model loaded, creating DfTract (ch {ch})..."));
            let m = match catch_unwind(AssertUnwindSafe(|| {
                DfTract::new(df_params, &r_params)
            })) {
                Ok(Ok(m)) => m,
                Ok(Err(e)) => {
                    let msg = format!("init runtime: {e:?}");
                    dflog(&msg);
                    return Err(msg);
                }
                Err(e) => {
                    let msg = format!("DfTract::new panicked: {e:?}");
                    dflog(&msg);
                    return Err(msg);
                }
            };
            states.push(m);
        }
        let hop = states[0].hop_size;
        dflog(&format!("init: ok, hop={hop}"));
        Ok(Worker { states, channels, hop, atten_lim_db, pf_beta })
    }

    fn reset(&mut self) {
        if let Ok(fresh) = Worker::new(self.channels, self.atten_lim_db, self.pf_beta) {
            self.states = fresh.states;
        } else {
            dflog("reset: re-init failed, keeping old state");
        }
    }

    fn process_hop(&mut self, inputs: Vec<Vec<f32>>) -> Vec<Vec<f32>> {
        let mut outs = Vec::with_capacity(self.channels);
        for (ch, input) in inputs.iter().enumerate() {
            let hop = self.hop;
            let mut output = vec![0.0f32; hop];
            let res = catch_unwind(AssertUnwindSafe(|| {
                let inp = ArrayView2::from_shape((1, hop), input).unwrap();
                let out = ArrayViewMut2::from_shape((1, hop), &mut output).unwrap();
                self.states[ch].process(inp, out)
            }));
            match res {
                Ok(Ok(_)) => outs.push(output),
                Ok(Err(e)) => {
                    dflog(&format!("process error ch {ch}: {e:?}"));
                    outs.push(input.clone()); // 直通
                }
                Err(e) => {
                    dflog(&format!("process panicked ch {ch}: {e:?}"));
                    outs.push(input.clone()); // 直通
                }
            }
        }
        outs
    }
}

pub struct DfRestore {
    cmd_tx: Sender<Command>,
    hop: usize,
    channels: usize,
    /// 每声道输入累积缓冲（凑满 hop 触发推理）
    in_buf: Vec<Vec<f32>>,
    in_fill: Vec<usize>,
    /// 输出 FIFO（per 声道）
    out_fifo: Vec<VecDeque<f32>>,
}

impl DfRestore {
    fn new(channels: usize, atten_lim_db: f32, pf_beta: f32) -> Result<Self, String> {
        let (ready_tx, ready_rx) = channel::<Result<usize, String>>();
        let (cmd_tx, cmd_rx) = channel::<Command>();
        let ch_count = channels;

        // 全部重活跑在 32MB 大栈线程上（Android mpv 音频线程栈小，
        // tract 模型编译会爆栈）。
        let _worker = std::thread::Builder::new()
            .name("dfrestore-worker".into())
            .stack_size(WORKER_STACK)
            .spawn(move || {
                let mut worker = match Worker::new(ch_count, atten_lim_db, pf_beta) {
                    Ok(w) => {
                        let _ = ready_tx.send(Ok(w.hop));
                        w
                    }
                    Err(e) => {
                        let _ = ready_tx.send(Err(e));
                        return;
                    }
                };
                while let Ok(cmd) = cmd_rx.recv() {
                    match cmd {
                        Command::Process(inputs, reply) => {
                            let outs = worker.process_hop(inputs);
                            let _ = reply.send(outs);
                        }
                        Command::Reset => worker.reset(),
                    }
                }
            })
            .map_err(|e| format!("spawn worker: {e:?}"))?;

        let hop = ready_rx
            .recv()
            .map_err(|_| "worker died during init".to_string())?
            .map_err(|e| format!("init: {e}"))?;

        Ok(DfRestore {
            cmd_tx,
            hop,
            channels,
            in_buf: vec![vec![0.0; hop]; channels],
            in_fill: vec![0; channels],
            out_fifo: (0..channels).map(|_| VecDeque::new()).collect(),
        })
    }

    fn process_hop(&mut self) -> bool {
        let inputs: Vec<Vec<f32>> = (0..self.channels)
            .map(|ch| self.in_buf[ch].clone())
            .collect();
        let (tx, rx) = channel();
        if self.cmd_tx.send(Command::Process(inputs, tx)).is_err() {
            return false;
        }
        let Ok(outs) = rx.recv() else {
            return false;
        };
        for ch in 0..self.channels {
            self.out_fifo[ch].extend(outs[ch].iter().copied());
        }
        true
    }
}

/// 创建实例（内嵌 DeepFilterNet3 模型，大栈工作线程）。失败返回 NULL。
/// atten_lim_db 定版 12.0；pf_beta 定版 0.05（0 关闭后置滤波）。
#[no_mangle]
pub unsafe extern "C" fn dfrestore_init(
    channels: i32,
    atten_lim_db: f32,
    pf_beta: f32,
) -> *mut DfRestore {
    if channels <= 0 || channels as usize > MAX_CHANNELS {
        return std::ptr::null_mut();
    }
    match DfRestore::new(channels as usize, atten_lim_db, pf_beta) {
        Ok(r) => Box::into_raw(Box::new(r)),
        Err(_) => std::ptr::null_mut(),
    }
}

/// hop 大小（样本数，48kHz 下 960）。无效句柄返回 0。
#[no_mangle]
pub unsafe extern "C" fn dfrestore_hop_size(r: *const DfRestore) -> usize {
    match r.as_ref() {
        Some(r) => r.hop,
        None => 0,
    }
}

/// 就地处理一块 interleaved float PCM（frames × channels 个样本）。
/// 返回 0 成功；句柄无效返回 -1（调用方应直通）。
#[no_mangle]
pub unsafe extern "C" fn dfrestore_process(
    r: *mut DfRestore,
    samples: *mut c_float,
    frames: i32,
) -> i32 {
    let Some(r) = (unsafe { r.as_mut() }) else {
        return -1;
    };
    if samples.is_null() || frames <= 0 {
        return -1;
    }
    let view = unsafe { std::slice::from_raw_parts_mut(samples, frames as usize * r.channels) };
    for i in 0..frames as usize {
        // 输入样本累积进各声道缓冲
        for ch in 0..r.channels {
            let s = view[i * r.channels + ch];
            r.in_buf[ch][r.in_fill[ch]] = if s.is_finite() { s } else { 0.0 };
            r.in_fill[ch] += 1;
        }
        let all_full = r.in_fill.iter().all(|&f| f >= r.hop);
        if all_full {
            if !r.process_hop() {
                // 工作线程死亡：该帧直通，绝不静音
                for ch in 0..r.channels {
                    r.out_fifo[ch].extend(r.in_buf[ch].iter().copied());
                }
            }
            for ch in 0..r.channels {
                r.in_fill[ch] = 0;
            }
        }
        // 输出：FIFO 有数据则消费（延迟 hop 样本），否则直通当前样本
        for ch in 0..r.channels {
            if let Some(v) = r.out_fifo[ch].pop_front() {
                view[i * r.channels + ch] = v;
            }
        }
    }
    0
}

/// 更新衰减上限（dB）。运行时可调。
/// 注：大栈线程架构下此函数为 no-op（需重建实例才能改参）；Restore UI
/// 是开关语义（移除/添加滤镜），用不到运行时调参。
#[no_mangle]
pub unsafe extern "C" fn dfrestore_set_atten_lim(r: *mut DfRestore, _lim_db: f32) {
    let _ = r;
}

/// 更新后置滤波 beta（0 = 关闭）。同上，no-op。
#[no_mangle]
pub unsafe extern "C" fn dfrestore_set_post_filter_beta(r: *mut DfRestore, _beta: f32) {
    let _ = r;
}

/// 重置全部状态（seek / 切歌 / 变速时调用）。
#[no_mangle]
pub unsafe extern "C" fn dfrestore_reset(r: *mut DfRestore) {
    if let Some(r) = unsafe { r.as_mut() } {
        let _ = r.cmd_tx.send(Command::Reset);
        for ch in 0..r.channels {
            r.in_fill[ch] = 0;
            r.out_fifo[ch].clear();
        }
    }
}

/// 释放实例。
#[no_mangle]
pub unsafe extern "C" fn dfrestore_free(r: *mut DfRestore) {
    if !r.is_null() {
        drop(unsafe { Box::from_raw(r) });
    }
}
