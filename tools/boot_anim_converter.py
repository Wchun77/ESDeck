#!/usr/bin/env python3
"""
ESDeck 開機動畫轉檔工具

把一段影片轉成 ESDeck 開機動畫用的 JPEG 影格序列:
    frame_0000.jpg, frame_0001.jpg, frame_0002.jpg, ...

放到 SD 卡的 assets/boot/ 資料夾下,開機時 ESP32 會自動偵測並逐幀解碼播放。
之所以輸出 JPEG 而不是原始 RGB565,是因為 800x480 的原始畫面一幀高達
750KB,遠超過 SD 卡實際可讀寫速度,會嚴重卡頓;JPEG 壓縮後一幀通常只有
幾十 KB,ESP32 端用 esp_jpeg 元件即時解碼,大幅降低 SD 卡讀取負擔。

僅依賴系統安裝的 ffmpeg,不需要 Pillow。

需求套件: PySide6
    pip install PySide6 --break-system-packages
另外系統需要安裝 ffmpeg 並加入 PATH。
"""

import os
import re
import shutil
import subprocess
import sys

from PySide6.QtCore import Qt, QThread, Signal
from PySide6.QtWidgets import (
    QApplication, QWidget, QVBoxLayout, QHBoxLayout, QFormLayout,
    QLabel, QLineEdit, QPushButton, QFileDialog, QComboBox,
    QDoubleSpinBox, QSpinBox, QTextEdit, QProgressBar, QMessageBox,
    QGroupBox,
)

SCREEN_W = 800
SCREEN_H = 480

ASPECT_MODES = {
    "裁切填滿畫面 (Crop)": "crop",
    "留黑邊完整顯示 (Pad)": "pad",
    "強制拉伸至滿版 (Stretch)": "stretch",
}


def build_ffmpeg_vf(mode: str, width: int, height: int) -> str:
    """依照選擇的縮放模式組出 ffmpeg -vf 濾鏡字串。"""
    if mode == "crop":
        return (
            f"scale={width}:{height}:force_original_aspect_ratio=increase,"
            f"crop={width}:{height}"
        )
    if mode == "pad":
        return (
            f"scale={width}:{height}:force_original_aspect_ratio=decrease,"
            f"pad={width}:{height}:(ow-iw)/2:(oh-ih)/2:color=black"
        )
    # stretch: 直接無視長寬比縮放
    return f"scale={width}:{height}"


class ConvertWorker(QThread):
    log = Signal(str)
    progress = Signal(int)         # 0-100
    finished_ok = Signal(int)      # 產生的影格數
    finished_err = Signal(str)

    def __init__(self, input_path, output_dir, duration, fps, quality,
                 aspect_mode, parent=None):
        super().__init__(parent)
        self.input_path = input_path
        self.output_dir = output_dir
        self.duration = duration
        self.fps = fps
        self.quality = quality
        self.aspect_mode = aspect_mode

    def run(self):
        try:
            self._run()
        except Exception as e:  # noqa: BLE001
            self.finished_err.emit(str(e))

    def _run(self):
        if shutil.which("ffmpeg") is None:
            self.finished_err.emit("找不到 ffmpeg,請先安裝並加進系統 PATH。")
            return

        os.makedirs(self.output_dir, exist_ok=True)

        # 清掉舊的 frame_*.jpg,避免新舊影格數量不一致造成殘留幀
        for name in os.listdir(self.output_dir):
            if re.fullmatch(r"frame_\d{4}\.jpg", name):
                os.remove(os.path.join(self.output_dir, name))

        vf = build_ffmpeg_vf(self.aspect_mode, SCREEN_W, SCREEN_H)
        out_pattern = os.path.join(self.output_dir, "frame_%04d.jpg")

        total_frames = max(1, round(self.duration * self.fps))

        cmd = [
            "ffmpeg", "-y",
            "-i", self.input_path,
            "-t", str(self.duration),
            "-r", str(self.fps),
            "-vf", vf,
            "-q:v", str(self.quality),
            "-start_number", "0",
            out_pattern,
        ]
        self.log.emit("執行指令: " + " ".join(cmd))

        proc = subprocess.Popen(
            cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            universal_newlines=True, bufsize=1,
        )

        frame_re = re.compile(r"frame=\s*(\d+)")
        for line in proc.stdout:
            line = line.rstrip()
            if line:
                self.log.emit(line)
            m = frame_re.search(line)
            if m:
                cur = int(m.group(1))
                pct = min(99, int(cur * 100 / total_frames))
                self.progress.emit(pct)

        ret = proc.wait()
        if ret != 0:
            self.finished_err.emit(f"ffmpeg 執行失敗 (exit code {ret}),請看上方紀錄。")
            return

        actual_frames = len([
            n for n in os.listdir(self.output_dir)
            if re.fullmatch(r"frame_\d{4}\.jpg", n)
        ])
        if actual_frames == 0:
            self.finished_err.emit("ffmpeg 執行完成,但沒有產生任何影格,請確認影片路徑正確。")
            return

        self.progress.emit(100)
        self.finished_ok.emit(actual_frames)


class MainWindow(QWidget):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("ESDeck 開機動畫轉檔工具")
        self.resize(680, 560)
        self.worker = None
        self._build_ui()

    def _build_ui(self):
        root = QVBoxLayout(self)

        # -------- 輸入影片 --------
        in_group = QGroupBox("輸入影片")
        in_layout = QHBoxLayout(in_group)
        self.input_edit = QLineEdit()
        self.input_edit.setPlaceholderText("選擇要轉換的影片檔...")
        btn_browse_in = QPushButton("瀏覽...")
        btn_browse_in.clicked.connect(self.browse_input)
        in_layout.addWidget(self.input_edit)
        in_layout.addWidget(btn_browse_in)
        root.addWidget(in_group)

        # -------- 輸出資料夾 --------
        out_group = QGroupBox("輸出資料夾 (對應 SD 卡的 assets/boot/)")
        out_layout = QHBoxLayout(out_group)
        self.output_edit = QLineEdit()
        self.output_edit.setPlaceholderText("選擇輸出資料夾,例如 SD 卡的 assets/boot")
        btn_browse_out = QPushButton("瀏覽...")
        btn_browse_out.clicked.connect(self.browse_output)
        out_layout.addWidget(self.output_edit)
        out_layout.addWidget(btn_browse_out)
        root.addWidget(out_group)

        # -------- 參數設定 --------
        param_group = QGroupBox("轉換參數")
        form = QFormLayout(param_group)

        self.duration_spin = QDoubleSpinBox()
        self.duration_spin.setRange(0.5, 30.0)
        self.duration_spin.setSingleStep(0.5)
        self.duration_spin.setValue(3.0)
        self.duration_spin.setSuffix(" 秒")
        form.addRow("動畫長度:", self.duration_spin)

        self.fps_spin = QSpinBox()
        self.fps_spin.setRange(1, 30)
        self.fps_spin.setValue(12)
        self.fps_spin.setSuffix(" fps")
        form.addRow("幀率:", self.fps_spin)

        self.aspect_combo = QComboBox()
        self.aspect_combo.addItems(list(ASPECT_MODES.keys()))
        form.addRow("縮放方式 (800x480 非常見比例):", self.aspect_combo)

        self.quality_spin = QSpinBox()
        self.quality_spin.setRange(2, 20)
        self.quality_spin.setValue(5)
        form.addRow("JPEG 品質 (2=最佳/檔案較大, 20=較差/檔案較小):", self.quality_spin)

        self.est_label = QLabel("")
        form.addRow("預估影格數:", self.est_label)
        self.duration_spin.valueChanged.connect(self._update_estimate)
        self.fps_spin.valueChanged.connect(self._update_estimate)
        self._update_estimate()

        root.addWidget(param_group)

        # -------- 動作按鈕 --------
        action_layout = QHBoxLayout()
        self.convert_btn = QPushButton("開始轉換")
        self.convert_btn.clicked.connect(self.start_convert)
        action_layout.addWidget(self.convert_btn)
        root.addLayout(action_layout)

        self.progress_bar = QProgressBar()
        self.progress_bar.setRange(0, 100)
        root.addWidget(self.progress_bar)

        # -------- 紀錄輸出 --------
        self.log_edit = QTextEdit()
        self.log_edit.setReadOnly(True)
        root.addWidget(self.log_edit)

    def _update_estimate(self):
        n = max(1, round(self.duration_spin.value() * self.fps_spin.value()))
        self.est_label.setText(f"{n} 幀")

    def browse_input(self):
        path, _ = QFileDialog.getOpenFileName(
            self, "選擇影片檔", "",
            "影片檔 (*.mp4 *.mov *.avi *.mkv *.webm);;所有檔案 (*)",
        )
        if path:
            self.input_edit.setText(path)

    def browse_output(self):
        path = QFileDialog.getExistingDirectory(self, "選擇輸出資料夾")
        if path:
            self.output_edit.setText(path)

    def start_convert(self):
        input_path = self.input_edit.text().strip()
        output_dir = self.output_edit.text().strip()

        if not input_path or not os.path.isfile(input_path):
            QMessageBox.warning(self, "錯誤", "請先選擇有效的輸入影片檔。")
            return
        if not output_dir:
            QMessageBox.warning(self, "錯誤", "請先選擇輸出資料夾。")
            return

        self.convert_btn.setEnabled(False)
        self.progress_bar.setValue(0)
        self.log_edit.clear()

        aspect_mode = ASPECT_MODES[self.aspect_combo.currentText()]

        self.worker = ConvertWorker(
            input_path=input_path,
            output_dir=output_dir,
            duration=self.duration_spin.value(),
            fps=self.fps_spin.value(),
            quality=self.quality_spin.value(),
            aspect_mode=aspect_mode,
        )
        self.worker.log.connect(self.append_log)
        self.worker.progress.connect(self.progress_bar.setValue)
        self.worker.finished_ok.connect(self.on_finished_ok)
        self.worker.finished_err.connect(self.on_finished_err)
        self.worker.start()

    def append_log(self, text):
        self.log_edit.append(text)

    def on_finished_ok(self, frame_count):
        self.convert_btn.setEnabled(True)
        self.append_log(f"\n轉換完成,共產生 {frame_count} 幀。")
        QMessageBox.information(
            self, "完成",
            f"轉換完成,共產生 {frame_count} 幀 JPEG 影格。\n"
            f"請把輸出資料夾內的 frame_*.jpg 全部複製到 SD 卡的 assets/boot/ 底下。",
        )

    def on_finished_err(self, message):
        self.convert_btn.setEnabled(True)
        self.append_log(f"\n錯誤: {message}")
        QMessageBox.critical(self, "轉換失敗", message)


def main():
    app = QApplication(sys.argv)
    win = MainWindow()
    win.show()
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
