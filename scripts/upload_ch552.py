from pathlib import Path
import subprocess
import time

Import("env")


def serial_port_exists(port_name):
    if not port_name:
        return False
    try:
        from serial.tools import list_ports
        return any(p.device.upper() == port_name.upper() for p in list_ports.comports())
    except Exception:
        return True


def wait_for_port_disappear(port_name, timeout=3.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if not serial_port_exists(port_name):
            return True
        time.sleep(0.1)
    return False


def ch552_usb_present():
    try:
        result = subprocess.run(
            [
                "powershell",
                "-NoProfile",
                "-Command",
                "Get-CimInstance Win32_PnPEntity | "
                "Where-Object { $_.PNPDeviceID -match 'VID_1209|VID_4348|CH55|CH552' } | "
                "Select-Object -First 1 -ExpandProperty PNPDeviceID",
            ],
            capture_output=True,
            text=True,
            timeout=5,
        )
        return bool(result.stdout.strip())
    except Exception:
        return True


def touch_1200bps(port_name):
    if not port_name:
        return False

    try:
        import serial
    except ImportError:
        print("pyserial is not available; skip 1200bps bootloader touch")
        return False

    print(f"Touching {port_name} at 1200bps to enter CH552 bootloader...")
    sequences = (
        ((False, False), (True, True), (False, False)),
        ((True, True), (False, False)),
        ((False, True), (False, False)),
    )
    for sequence in sequences:
        try:
            port = serial.Serial()
            port.port = port_name
            port.baudrate = 1200
            port.timeout = 0.2
            port.dtr = sequence[0][0]
            port.rts = sequence[0][1]
            port.open()
            for dtr, rts in sequence:
                port.dtr = dtr
                port.rts = rts
                time.sleep(0.25)
            port.close()
        except Exception as exc:
            print(f"1200bps touch failed or port already gone: {exc}")
            break

        if wait_for_port_disappear(port_name, timeout=3.0):
            print(f"{port_name} disappeared; bootloader is likely active")
            time.sleep(1.0)
            return True

    time.sleep(2.0)
    return False


def touch_boot_command(port_name):
    if not port_name:
        return False

    try:
        import serial
    except ImportError:
        print("pyserial is not available; skip serial boot command")
        return False

    try:
        print(f"Sending boot command to {port_name}...")
        with serial.Serial(port_name, 57600, timeout=0.2) as port:
            port.dtr = True
            port.rts = True
            time.sleep(0.2)
            port.write(b"boot\n")
            port.flush()
        if wait_for_port_disappear(port_name, timeout=3.0):
            print(f"{port_name} disappeared after boot command")
            time.sleep(1.0)
            return True
    except Exception as exc:
        print(f"Serial boot command failed or port already gone: {exc}")
    return False


def upload_ch552(source, target, env):
    project = Path(env.subst("$PROJECT_DIR"))
    tool = project / "tools" / "ch55x" / "vnproch55x.exe"
    firmware = Path(str(source[0]))
    if firmware.suffix.lower() != ".hex":
        firmware = firmware.with_suffix(".hex")

    if not tool.exists():
        raise RuntimeError(f"Missing uploader: {tool}")
    if not firmware.exists():
        raise RuntimeError(f"Missing firmware hex: {firmware}")

    upload_port = env.subst("$UPLOAD_PORT")
    had_serial = serial_port_exists(upload_port)
    entered_bootloader = False

    if had_serial:
        entered_bootloader = touch_boot_command(upload_port)
        if not entered_bootloader:
            entered_bootloader = touch_1200bps(upload_port)
    elif not ch552_usb_present():
        raise RuntimeError(
            f"{upload_port} is not present and no CH552 USB device was found; "
            "replug the CH552 board before uploading."
        )

    if had_serial and not entered_bootloader and serial_port_exists(upload_port):
        raise RuntimeError(
            f"{upload_port} did not enter bootloader; send boot manually or replug in download mode."
        )

    cmd = [str(tool), "-r", "16", "-t", "CH552", str(firmware)]
    print("Uploading with:", " ".join(cmd))
    subprocess.check_call(cmd, cwd=str(project))


env.Replace(UPLOADCMD=upload_ch552)
