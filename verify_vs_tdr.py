"""
自作 Kyohei Clipper と TDR Limiter 6 GE Clipper の比較検証
"""
import pedalboard
import numpy as np
import scipy.signal as ss
import matplotlib.pyplot as plt
from pathlib import Path

SR = 48000
OUT = Path(__file__).parent / "verification"
OUT.mkdir(exist_ok=True)

KYO_VST3 = str(Path.home() / "Library/Audio/Plug-Ins/VST3/Kyohei Clipper.vst3")
TDR_VST3 = "/Library/Audio/Plug-Ins/VST3/TDR Limiter 6 GE.vst3"


def db2lin(db): return 10 ** (db / 20.0)
def lin2db(x, eps=1e-12): return 20 * np.log10(np.maximum(np.abs(x), eps))


def make_kyohei(mode="B.Wall", threshold=-6.0, knee=0.0):
    p = pedalboard.load_plugin(KYO_VST3)
    p.threshold_db = threshold
    p.knee_db = knee
    p.mode = {"B.Wall": "B.Wall", "Open": "Open", "LF": "LF"}[mode]
    p.input_db = 0.0
    p.output_db = 0.0
    return p


def make_tdr(mode="B. Wall", threshold=-6.0, knee=0.0):
    p = pedalboard.load_plugin(TDR_VST3)
    p.compressor_enabled = False
    p.peak_lim_enabled = False
    p.hf_lim_enabled = False
    p.meter_enabled = False
    p.clipper_enabled = True
    p.clipper_mode = mode
    p.clipper_knee_db = knee
    p.clipper_threshold_db = threshold
    p.clipper_gain_db = 0.0
    p.clipper_separation = 0.0
    p.clipper_dry_amount = 0.0
    p.output_drive_db = 0.0
    p.output_ceiling_tp_db = 6.0
    p.output_ceiling_pcm_db = 6.0
    p.quality = "Precise"
    return p


def process(p, sig):
    s = np.stack([sig, sig]).astype(np.float32)
    return p(s, SR, reset=True)[0]


def harmonic_amp(sig, f0):
    n = len(sig); win = ss.windows.hann(n)
    spec = np.fft.rfft(sig * win)
    freqs = np.fft.rfftfreq(n, 1 / SR)
    mag = np.abs(spec) / (n / 4)
    idx = np.argmin(np.abs(freqs - f0))
    lo, hi = max(0, idx - 3), min(len(mag), idx + 4)
    return mag[lo:hi].max()


def measure_sweep(p, freqs, over_dbs):
    m = np.zeros((len(over_dbs), len(freqs)))
    for i, od in enumerate(over_dbs):
        amp = db2lin(-6.0 + od)
        for j, f in enumerate(freqs):
            dur = max(0.2, 30 / f)
            n = int(SR * dur)
            t = np.arange(n) / SR
            sig = amp * np.sin(2 * np.pi * f * t).astype(np.float32)
            y = process(p, sig)
            m[i, j] = lin2db(harmonic_amp(y[n // 4: 3 * n // 4], f))
    return m


def run():
    freqs = np.logspace(np.log10(50), np.log10(16000), 24)
    over_dbs = [3.0, 6.0, 9.0, 12.0]

    configs = [
        ("B. Wall / B.Wall", "B. Wall", "B.Wall"),
        ("Open / Open",      "Open",    "Open"),
        ("LF Clipper / LF",  "LF Clipper", "LF"),
    ]

    fig, axes = plt.subplots(len(configs), 2, figsize=(14, 4.5 * len(configs)))
    colors = plt.cm.viridis(np.linspace(0.1, 0.85, len(over_dbs)))

    for row, (title, tdr_mode, kyo_mode) in enumerate(configs):
        print(f"[{row+1}/3] {title}")
        tdr = make_tdr(mode=tdr_mode, knee=0.0)
        kyo = make_kyohei(mode=kyo_mode, knee=0.0)

        m_tdr = measure_sweep(tdr, freqs, over_dbs)
        m_kyo = measure_sweep(kyo, freqs, over_dbs)

        # 左: 重ねプロット
        ax = axes[row, 0]
        for i, od in enumerate(over_dbs):
            ax.semilogx(freqs, m_tdr[i], color=colors[i], lw=1.5,
                        label=f'TDR +{od}dB', marker='.', ms=4)
            ax.semilogx(freqs, m_kyo[i], color=colors[i], ls='--', lw=1,
                        alpha=0.7, marker='x', ms=4)
        ax.set_title(f"{title}  (solid=TDR, dashed=Kyohei)", fontsize=10)
        ax.set_xlabel("freq [Hz]"); ax.set_ylabel("f1 [dB]")
        ax.legend(fontsize=7, ncol=2); ax.grid(alpha=0.3, which='both')

        # 右: 差分
        ax = axes[row, 1]
        for i, od in enumerate(over_dbs):
            diff = m_kyo[i] - m_tdr[i]
            ax.semilogx(freqs, diff, color=colors[i], lw=1, marker='.', ms=4,
                        label=f'+{od}dB (σ={diff.std():.2f}, max={np.abs(diff).max():.2f})')
        ax.axhline(0, color='k', ls=':', lw=0.5)
        ax.set_title("Kyohei - TDR [dB]", fontsize=10)
        ax.set_xlabel("freq [Hz]"); ax.set_ylabel("error [dB]")
        ax.set_ylim(-6, 6); ax.legend(fontsize=7); ax.grid(alpha=0.3, which='both')

        rmse = np.sqrt(np.mean((m_kyo - m_tdr) ** 2))
        print(f"  RMSE = {rmse:.2f} dB")

    fig.suptitle("Kyohei Clipper vs TDR L6 Clipper — verification sweep", fontsize=11)
    fig.tight_layout()
    fig.savefig(OUT / "verify_vs_tdr.png", dpi=120)
    print(f"\nsaved: {OUT / 'verify_vs_tdr.png'}")


if __name__ == "__main__":
    run()
