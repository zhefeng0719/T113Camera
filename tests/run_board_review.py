"""Opt-in board regression; requires Paramiko and T113_BOARD_PASSWORD.

Uploads test executables, exercises event1, and saves logs under output/review-logs.
It does not delete existing USB media or install an application service.
"""
import os
from pathlib import Path
import re
import shlex
import struct
import time
import paramiko

ROOT = Path(__file__).resolve().parents[1]
LOGS = ROOT / "output" / "review-logs"
BOARD = "/root/Project/T113Camera"
USB = "/mnt/usb/sda/T113Camera"


def main():
    LOGS.mkdir(parents=True, exist_ok=True)
    client = paramiko.SSHClient()
    client.load_system_host_keys()
    client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    client.connect(os.environ.get("T113_BOARD_IP", "192.168.1.77"),
                   username="root", password=os.environ["T113_BOARD_PASSWORD"], timeout=5)
    sftp = client.open_sftp()
    tools = ("media_regression_test", "jpeg_render_test", "record_writer_test",
             "muxer_integrity_test", "playback_timing_test")
    app = BOARD + "/t113_camera.review"
    pid = None

    def run(name, command, timeout=60):
        _, stdout, stderr = client.exec_command(command, timeout=timeout)
        output = stdout.read().decode(errors="replace") + stderr.read().decode(errors="replace")
        code = stdout.channel.recv_exit_status()
        (LOGS / (name + ".log")).write_text(output, encoding="utf-8")
        print(name, "exit=", code, "\n", "\n".join(output.splitlines()[-8:]), flush=True)
        if code:
            raise RuntimeError(name + " failed")
        return output

    def key(code, duration=0.08):
        for value in (1, 0):
            payload = struct.pack("<IIHHi", 0, 0, 1, code, value) + struct.pack("<IIHHi", 0, 0, 0, 0, 0)
            octal = "".join("\\%03o" % byte for byte in payload)
            # SFTP uses pwrite offsets; input devices require ordinary sequential write().
            _, stdout, _ = client.exec_command("printf '" + octal + "' > /dev/input/event1")
            if stdout.channel.recv_exit_status():
                raise RuntimeError("key injection failed")
            if value:
                time.sleep(duration)
        time.sleep(0.55)

    try:
        for name in tools:
            sftp.put(str(ROOT / "output" / name), "/tmp/" + name)
            sftp.chmod("/tmp/" + name, 0o755)
        sftp.put(str(ROOT / "output" / "t113_camera"), app)
        sftp.chmod(app, 0o755)
        run("logic", "/tmp/media_regression_test")
        run("writer", "/tmp/record_writer_test")
        run("integrity", "/tmp/muxer_integrity_test /tmp")
        paths = USB + "/DCIM/record1.jpg " + USB + "/DCIM/yln.jpg"
        run("jpeg_pixels", "/tmp/jpeg_render_test " + paths)
        run("jpeg_copy", "VIDEO_SRC=copy /tmp/jpeg_render_test " + paths)
        output = run("record", app + " 5")
        recorded = re.search(r"saved\s*:\s*(\S+)", output).group(1)
        run("record_inspect", "/tmp/muxer_integrity_test --inspect " + shlex.quote(recorded))
        run("pause", "/tmp/playback_timing_test " + shlex.quote(recorded))
        run("replay", app + " ui " + shlex.quote(recorded))

        # Walk the real UI list three times without enabling container first-frame previews.
        count = sum(len([n for n in sftp.listdir(USB + "/" + folder)
                         if n.lower().endswith((".jpg", ".jpeg", ".avi", ".mp4", ".mkv",
                                                 ".mjpeg", ".mjpg", ".mp3", ".aac", ".m4a", ".wav"))])
                    for folder in ("AUDIO", "DCIM", "VIDEO"))
        _, stdout, stderr = client.exec_command("echo $$; exec " + app, timeout=90)
        pid = int(stdout.readline())
        time.sleep(2)
        for _ in range(min(count, 64) * 3):
            key(407)
        key(158, 1.8)  # Camera mode.
        key(158)       # Start recording.
        time.sleep(3)
        key(158)       # Stop and commit.
        key(158, 1.8)  # Return to list, then select more photos.
        for _ in range(min(count, 64)):
            key(407)
        client.exec_command("kill -INT " + str(pid))
        output = stdout.read().decode(errors="replace") + stderr.read().decode(errors="replace")
        code = stdout.channel.recv_exit_status()
        pid = None
        (LOGS / "ui_cycles.log").write_text(output, encoding="utf-8")
        previews = output.count("jpeg preview:")
        print("UI cycles: exit=", code, "JPEG previews=", previews, flush=True)
        if code or previews < 9 or any(x in output for x in
                                      ("disp vsync wait:", "G2D_CMD_BITBLT_H yuv:", "PREVIEW ERROR")):
            raise RuntimeError("UI switching regression failed")
    finally:
        if pid:
            client.exec_command("kill -INT " + str(pid))
        for name in tools:
            try:
                sftp.remove("/tmp/" + name)
            except OSError:
                pass
        sftp.close()
        client.close()


if __name__ == "__main__":
    main()
