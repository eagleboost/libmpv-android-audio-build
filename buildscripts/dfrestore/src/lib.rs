//! dfrestore —— DeepFilterNet3 实时语音恢复的 C ABI wrapper。
//!
//! 面向 VoyTrace 的 mpv audio filter（af_dfrestore.c）。底层复用官方
//! deep_filter crate 的 tract ONNX 推理（纯 Rust，无 torch 依赖），模型
//! 为内嵌的 DeepFilterNet3（default-model feature，编进 .so，免分发）。
//!
//! 设计（见 VoyTrace tasks/audio-restoration-voice-restore.md §11）：
//! - 底层 DfTract 是 mono 的，本 wrapper 支持多声道：每声道一个实例。
//! - 底层 process 要求每帧恰好 hop_size（48kHz 下 960 样本，20ms）；
//!   wrapper 内部缓冲任意长度输入，凑满一帧才推理。
//! - 输出相对输入有固有延迟（算法 lookahead），流式语义：预热期直通，
//!   之后消费 FIFO。
//! - 任何错误（初始化失败、推理失败）绝不静音：初始化失败返回 NULL，
//!   运行期错误直通。
//!
//! 定版配方（盲听收敛，双集验证）：DeepFilterNet3 + atten_lim 12dB +
//! post-filter beta 0.05。

use df::tract::{DfParams, DfTract, ReduceMask, RuntimeParams};
use ndarray::{ArrayView2, ArrayViewMut2};
use std::collections::VecDeque;
use std::ffi::c_float;
use std::panic::{catch_unwind, AssertUnwindSafe};

const MAX_CHANNELS: usize = 8;

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

pub struct DfRestore {
    states: Vec<DfTract>,
    channels: usize,
    hop: usize,
    /// 每声道输入累积缓冲（凑满 hop 触发推理）
    in_buf: Vec<Vec<f32>>,
    in_fill: Vec<usize>,
    /// 输出 FIFO（per 声道）
    out_fifo: Vec<VecDeque<f32>>,
}

impl DfRestore {
    fn new(channels: usize, atten_lim_db: f32, pf_beta: f32) -> Result<Self, String> {
        dflog(&format!("init: ch={channels} atten={atten_lim_db} beta={pf_beta}"));
        let mut states = Vec::with_capacity(channels);
        for ch in 0..channels {
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
        Ok(DfRestore {
            states,
            channels,
            hop,
            in_buf: vec![vec![0.0; hop]; channels],
            in_fill: vec![0; channels],
            out_fifo: (0..channels).map(|_| VecDeque::new()).collect(),
        })
    }
}

/// 创建实例（内嵌 DeepFilterNet3 模型）。失败返回 NULL。
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
            for ch in 0..r.channels {
                let hop = r.hop;
                let input: Vec<f32> = r.in_buf[ch].clone();
                let mut output = vec![0.0f32; hop];
                let inp = ArrayView2::from_shape((1, hop), &input).unwrap();
                let out = ArrayViewMut2::from_shape((1, hop), &mut output).unwrap();
                let res = catch_unwind(AssertUnwindSafe(|| r.states[ch].process(inp, out)));
                let ok = matches!(res, Ok(Ok(_)));
                if let Err(e) = &res {
                    dflog(&format!("process panicked on ch {ch}: {e:?}"));
                }
                // 推理失败 → 该帧直通（input 原样入 FIFO），绝不静音
                if ok {
                    for v in output.iter() {
                        if v.is_finite() {
                            r.out_fifo[ch].push_back(*v);
                        } else {
                            r.out_fifo[ch].push_back(0.0);
                        }
                    }
                } else {
                    r.out_fifo[ch].extend(input.iter().copied());
                }
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
#[no_mangle]
pub unsafe extern "C" fn dfrestore_set_atten_lim(r: *mut DfRestore, lim_db: f32) {
    if let Some(r) = unsafe { r.as_mut() } {
        for st in r.states.iter_mut() {
            st.set_atten_lim(lim_db);
        }
    }
}

/// 更新后置滤波 beta（0 = 关闭）。
#[no_mangle]
pub unsafe extern "C" fn dfrestore_set_post_filter_beta(r: *mut DfRestore, beta: f32) {
    if let Some(r) = unsafe { r.as_mut() } {
        for st in r.states.iter_mut() {
            st.set_pf_beta(beta);
        }
    }
}

/// 重置全部状态（seek / 切歌 / 变速时调用）。
/// 复用 DfTract::init()（清空滚动缓冲与内部状态），不重建模型。
#[no_mangle]
pub unsafe extern "C" fn dfrestore_reset(r: *mut DfRestore) {
    if let Some(r) = unsafe { r.as_mut() } {
        for st in r.states.iter_mut() {
            let _ = st.init();
        }
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
