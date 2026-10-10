"""Opt-in mount-loss regression; never physically disconnects USB or deletes existing media.

Requires T113_BOARD_PASSWORD. Temporarily unmounts/remounts the current vfat USB,
restores its original mount options in finally, and installs only after success.
"""
import os  # Credentials come from the environment, not committed source.
import re  # Extract only paths created by this test.
import shlex  # Quote every filesystem argument sent to the board shell.
import struct  # Build the board's 32-bit input_event records.
import time  # Bounded process waits and event spacing.
from pathlib import Path  # Local output and log paths.
import paramiko  # SSH commands and SFTP executable transfer.

ROOT = Path(__file__).resolve().parents[1]  # Repository root.
LOGS = ROOT / "output" / "hotplug-logs"  # Generated evidence, excluded from Git.
BOARD = "/root/Project/T113Camera"  # Existing application installation directory.


def main():
    LOGS.mkdir(parents=True, exist_ok=True)  # Keep complete test output locally.
    client = paramiko.SSHClient()  # Own one SSH connection for the complete run.
    client.load_system_host_keys()  # Reuse known board identity when available.
    client.set_missing_host_key_policy(paramiko.AutoAddPolicy())  # Match existing local test tooling.
    client.connect(os.getenv("T113_BOARD_IP", "192.168.1.77"), username="root",
                   password=os.environ["T113_BOARD_PASSWORD"], timeout=5)  # Explicit opt-in.
    sftp = client.open_sftp()  # Transfer binaries only, no source files on the board.
    temporary = "/tmp/t113-hotplug-%d" % int(time.time())  # Unique helper directory.
    app = BOARD + "/t113_camera.hotplug"  # Stage without replacing the working application.
    active = None  # Track exactly the process this script started.
    device = root = options = alternate = None  # Recovery uses verified original values.
    created = []  # Only this run's recordings may be deleted.
    helpers = []  # Exact remote helper paths, no recursive cleanup.

    def run(name, command, expected=0, timeout=30):
        _, stdout, stderr = client.exec_command(command, timeout=timeout)  # Bounded SSH command.
        output = stdout.read().decode(errors="replace") + stderr.read().decode(errors="replace")
        code = stdout.channel.recv_exit_status()  # Remote exit code, not output text.
        (LOGS / (name + ".log")).write_text(output, encoding="utf-8")  # Preserve failures too.
        if name == "normal_record":
            for path in re.findall(r"record not committed: (\S+)", output):
                created.append(path + ".part")  # Failed test recordings also require exact-path cleanup.
        print(name, "exit=", code, output[-1400:], flush=True)  # Concise live progress.
        if expected is not None and code != expected:
            raise RuntimeError(name + " failed")  # finally restores the mount.
        return output

    def mounted():
        _, stdout, _ = client.exec_command("cat /proc/mounts")  # Read kernel mount state.
        return [line.split() for line in stdout.read().decode().splitlines()]  # Board paths have no spaces.

    def restore():
        if not root:
            return  # Initial discovery failed; do not guess a device.
        entries = mounted()  # Avoid mounting the same volume twice.
        for entry in entries:
            if entry[1] == alternate:
                run("restore_alternate", "umount " + shlex.quote(alternate))  # Test-only mount.
        entries = mounted()  # Reconcile after removing the alternate mount.
        if not any(entry[1] == root for entry in entries):
            run("restore_mount", "mount -t vfat -o %s %s %s" % tuple(map(shlex.quote, (options, device, root))))
        else:
            run("restore_options", "mount -o %s %s" % (shlex.quote("remount," + options), shlex.quote(root)))

    def launch(arguments=""):
        nonlocal active  # A process is always stopped before starting another.
        _, stdout, stderr = client.exec_command("echo $$; exec " + shlex.quote(app) + " " + arguments)
        pid = int(stdout.readline().strip())  # Shell exec preserves this PID.
        active = (pid, stdout, stderr)  # Retain streams until task completion.
        time.sleep(2)  # Allow LVGL/hardware initialization before manipulating mounts.
        if stdout.channel.exit_status_ready():
            finish("early_exit", None)  # Record early failure before raising.
            raise RuntimeError("application exited before test action")

    def finish(name, expected=0, signal=None, timeout=15):
        nonlocal active  # Release only the process owned by this test.
        pid, stdout, stderr = active
        if signal and not stdout.channel.exit_status_ready():
            run(name + "_signal", "kill -%s %d" % (signal, pid))  # Graceful SIGINT only.
        deadline = time.monotonic() + timeout  # Never wait indefinitely for a driver hang.
        while not stdout.channel.exit_status_ready() and time.monotonic() < deadline:
            time.sleep(0.05)  # Let the application join its media workers.
        if not stdout.channel.exit_status_ready():
            raise RuntimeError(name + " timed out; inspect board before retrying")
        output = stdout.read().decode(errors="replace") + stderr.read().decode(errors="replace")
        code = stdout.channel.recv_exit_status(); active = None  # No live process remains.
        (LOGS / (name + ".log")).write_text(output, encoding="utf-8")  # Save full application evidence.
        print(name, "exit=", code, output[-1800:], flush=True)
        if expected is not None and code != expected:
            raise RuntimeError(name + " unexpected exit")
        return output

    def key(code, duration=0.08):
        for value in (1, 0):  # Kernel input receives press, then release.
            payload = struct.pack("<IIHHi", 0, 0, 1, code, value) + struct.pack("<IIHHi", 0, 0, 0, 0, 0)
            escaped = "".join("\\%03o" % byte for byte in payload)  # printf performs sequential write, unlike SFTP.
            run("key", "printf '" + escaped + "' > /dev/input/event1")
            if value:
                time.sleep(duration)  # Long presses use the real application threshold.
        time.sleep(0.25)  # Debounce between independent actions.

    try:
        run("process_guard", "! pidof t113_camera; ! pidof t113_camera.hotplug")  # Do not interrupt a user-owned session.
        candidates = [entry for entry in mounted() if entry[0].startswith("/dev/sd") and entry[1].startswith("/mnt/usb/")]
        if len(candidates) != 1 or candidates[0][2] != "vfat":
            raise RuntimeError("test requires exactly one vfat USB volume")  # No multi-disk guessing.
        device, root, _, options = candidates[0][:4]  # Preserve the exact original mount.
        alternate = "/mnt/usb/t113-hotplug-test"  # Test mount point is inside the project's USB namespace.
        run("prepare", "mkdir %s; mkdir %s" % (shlex.quote(temporary), shlex.quote(alternate)))
        sftp.put(str(ROOT / "output" / "t113_camera"), app); sftp.chmod(app, 0o755)  # Stage candidate.
        for tool in ("storage_monitor_test", "storage_test", "record_writer_test", "muxer_integrity_test", "media_regression_test"):
            path = temporary + "/" + tool; helpers.append(path)  # Remember exactly what was uploaded.
            sftp.put(str(ROOT / "output" / tool), path); sftp.chmod(path, 0o755)
            run(tool, shlex.quote(path) + (" /tmp" if tool == "muxer_integrity_test" else ""))

        launch()  # Idle list mode must survive removal and a different mount path.
        run("idle_unmount", "sync; umount " + shlex.quote(root)); time.sleep(0.8)
        run("alternate_mount", "mount -t vfat -o %s %s %s" % tuple(map(shlex.quote, (options, device, alternate)))); time.sleep(1)
        run("alternate_unmount", "umount " + shlex.quote(alternate)); time.sleep(0.8)
        restore(); time.sleep(1)  # Restore while the same UI process is still alive.
        output = finish("idle_dynamic_path", signal="INT")
        if "state=0" not in output or ("root=" + alternate) not in output:
            raise RuntimeError("idle list did not reconcile mount changes")

        ro = ",".join(["ro"] + [part for part in options.split(",") if part not in ("rw", "ro")])  # Preserve encoding and masks.
        run("readonly_mount", "mount -o %s %s" % (shlex.quote("remount," + ro), shlex.quote(root)))
        launch(); key(158, 1.7)  # Attempt camera mode; read-only storage must reject it.
        output = finish("readonly_ui", signal="INT")
        if "state=1" not in output:
            raise RuntimeError("read-only state was not detected")
        restore()  # Recording tests require the original writable mount.

        launch(); key(158); key(158)  # First sorted item is the existing Abdulla.mp3: play, then pause.
        run("paused_detach", "umount -l " + shlex.quote(root)); time.sleep(1)
        restore(); time.sleep(1)  # Same-path reinsertion must not resume the old audio task.
        output = finish("paused_unplug", signal="INT")
        if "state=0" not in output or "state=2" not in output:
            raise RuntimeError("paused playback did not return to list mode")

        video = root + "/T113Camera/VIDEO/guimie.mp4"  # Existing H264/AAC material, never delete it.
        launch("ui " + shlex.quote(video))  # Exercise the inner container playback loop.
        run("video_detach", "umount -l " + shlex.quote(root))
        finish("video_unplug", expected=1)  # Removal is cancellation, not a completed movie.
        restore()  # New session receives a fresh mount ID.

        launch("30")  # Recording must stop early and retain only an uncommitted .part.
        run("record_detach", "umount -l " + shlex.quote(root))
        output = finish("record_unplug", expected=1)  # Must not return RESULT: PASS.
        match = re.search(r"record not committed: (\S+)", output)
        if not match or "RESULT      : PASS" in output:
            raise RuntimeError("cancelled recording was not reported as uncommitted")
        restore()  # Inspect the retained file through the new mount.
        partial = match.group(1) + ".part"; created.append(partial)  # Only this exact test-created file is disposable.
        run("record_not_published", "test ! -e %s; test -f %s" % (shlex.quote(match.group(1)), shlex.quote(partial)))

        launch(); key(158, 1.7); time.sleep(1); key(158); time.sleep(1)  # Camera mode, then start recording.
        run("ui_record_detach", "umount -l " + shlex.quote(root)); time.sleep(1)
        restore(); time.sleep(1)  # UI must return to the list without resuming the recording.
        output = finish("ui_record_unplug", signal="INT")
        match = re.search(r"record not committed: (\S+)", output)
        if not match or "ui record   : start" not in output or "state=0" not in output:
            raise RuntimeError("UI recording removal did not cancel and return to list")
        partial = match.group(1) + ".part"; created.append(partial)  # This run's uncommitted recording only.
        run("ui_record_not_published", "test ! -e %s; test -f %s" % (shlex.quote(match.group(1)), shlex.quote(partial)))

        run("normal_record", shlex.quote(app) + " 3", timeout=20)  # Successful recording still works after remounting.
        text = (LOGS / "normal_record.log").read_text(encoding="utf-8")
        match = re.search(r"saved\s*:\s*(\S+)", text)
        if not match:
            raise RuntimeError("normal recording did not publish")
        created.append(match.group(1))  # Track the new recording, not any pre-existing name.
        run("normal_inspect", shlex.quote(temporary + "/muxer_integrity_test") + " --inspect " + shlex.quote(match.group(1)))
        staged = BOARD + "/t113_camera.install"  # Replace the binary only when no task is executing it.
        sftp.put(str(ROOT / "output" / "t113_camera"), staged); sftp.chmod(staged, 0o755)
        sftp.posix_rename(staged, BOARD + "/t113_camera")  # Atomic install of tested executable.
        print("HOTPLUG REGRESSION: PASS; physical unplug still requires manual validation", flush=True)
    finally:
        if active:
            finish("cleanup_process", expected=None, signal="INT")  # No forced reset or SIGKILL.
        restore()  # Always put the USB mount back before deleting only this run's files.
        for path in created + helpers + [app]:
            try:
                sftp.remove(path)  # Exact known file; never wipe the USB or media directories.
            except FileNotFoundError:
                pass
        if alternate:
            try:
                sftp.rmdir(alternate)  # Empty test mount point only.
            except OSError:
                pass
        try:
            sftp.rmdir(temporary)  # Nonrecursive removal fails safely if unexpected content exists.
        except OSError:
            pass
        sftp.close(); client.close()  # Release local transport resources.


if __name__ == "__main__":
    main()  # Run only on explicit invocation, never during compilation.
