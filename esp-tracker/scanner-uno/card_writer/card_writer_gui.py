#!/usr/bin/env python3
"""Enrollment-station GUI for the offline-fallback RFID cards.

Talks over USB serial to the Arduino Uno running card_writer/src/main.cpp
(NOT the gate scanner firmware — a separate, standalone sketch, see that
file's own comments). This program never touches an RC522 itself; the
Arduino does the actual MIFARE read/write and this just sends line
commands and shows the result.

Card format: scanner-uno/src/card.h. Whatever this writes must decode
correctly on the scanner, or the offline-SMS fallback silently does
nothing when the network is down — the one case it exists for.

Setup:
    pip install pyserial
    python3 card_writer_gui.py
"""

import queue
import re
import threading
import tkinter as tk
from tkinter import ttk, messagebox

import serial
import serial.tools.list_ports

BAUD = 9600
CARD_WAIT_HINT_S = 5   # matches CARD_WAIT_MS in the Arduino sketch


def normalize_ph_phone(raw: str):
    """Accepts 09171234567 / 9171234567 / +639171234567 / 639171234567
    (spaces/dashes ignored) and returns the bare 10 digits card.h expects
    (no country code, no leading 0), or None if it doesn't look like a
    PH mobile number."""
    digits = re.sub(r"[^0-9]", "", raw)
    if digits.startswith("63") and len(digits) == 12:
        digits = digits[2:]
    elif digits.startswith("0") and len(digits) == 11:
        digits = digits[1:]
    if len(digits) == 10 and digits[0] == "9":
        return digits
    return None


class WriterLink:
    """Owns the serial connection and the background thread that talks to
    it. GUI-thread callers hand it a command + callback; results come back
    marshalled onto the Tk main loop via `poll()`."""

    def __init__(self):
        self.ser = None
        self.reply_q = queue.Queue()

    def connect(self, port: str):
        self.close()
        self.ser = serial.Serial(port, BAUD, timeout=CARD_WAIT_HINT_S + 3)
        # The Uno resets when the USB-serial port opens; give the sketch
        # time to reach setup()'s "READY" line before we send anything.
        line = self.ser.readline().decode(errors="replace").strip()
        if line != "READY":
            # Some boards print nothing until the first command post-reset —
            # not fatal, just means we missed the banner. Try a PING instead
            # of failing outright.
            self.ser.write(b"PING\n")
            line = self.ser.readline().decode(errors="replace").strip()
            if line != "PONG":
                self.close()
                raise IOError(f"no response from board on {port} (got: {line!r})")

    def close(self):
        if self.ser and self.ser.is_open:
            self.ser.close()
        self.ser = None

    def connected(self):
        return self.ser is not None and self.ser.is_open

    # Lines the board only ever sends as a final answer to a command — any
    # other line (e.g. the TYPE: diagnostic before an AUTH_FAIL) is treated
    # as informational and logged, not mistaken for the actual result.
    TERMINAL_PREFIXES = ("OK:", "ERR:", "DATA:", "PONG")

    def send_command_async(self, line: str, on_done):
        """Runs the blocking serial round-trip on a worker thread; on_done
        is called on the Tk main thread with the response string (or an
        exception instance on failure). Any diagnostic lines the board
        sends before the terminal one are passed along too, for logging."""
        def worker():
            try:
                self.ser.reset_input_buffer()
                self.ser.write((line + "\n").encode())
                diag = []
                resp = "ERR:TIMEOUT"
                for _ in range(5):   # a couple of diagnostic lines, then the real answer
                    raw = self.ser.readline().decode(errors="replace").strip()
                    if not raw:
                        break
                    if raw.startswith(self.TERMINAL_PREFIXES):
                        resp = raw
                        break
                    diag.append(raw)
                self.reply_q.put((on_done, resp, diag))
            except Exception as exc:  # surfaced to the GUI, not swallowed
                self.reply_q.put((on_done, exc, []))
        threading.Thread(target=worker, daemon=True).start()


ERROR_HINTS = {
    "NO_CARD": "No card detected — hold the card flat on the reader and try again.",
    "AUTH_FAIL": "Card rejected the key — likely a blank/foreign card or a wiped one.",
    "WRITE_FAIL": "Write failed mid-sector — try again; if it repeats, the card may be worn out.",
    "VERIFY_FAIL": "Wrote but could not read back to verify — try again.",
    "VERIFY_MISMATCH": "Read-back didn't match what was written — try again.",
    "BAD_MAGIC": "Card has no valid attendance data (blank or different format).",
    "BAD_CRC": "Card data is corrupted (failed integrity check).",
    "BAD_CMD": "Internal error: bad command sent to board.",
    "TIMEOUT": "Board didn't respond — check the USB connection and port.",
}


class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("RFID Card Writer — Attendance Offline Fallback")
        self.geometry("480x520")
        self.link = WriterLink()

        self._build_connection_row()
        self._build_form()
        self._build_actions()
        self._build_log()

        self.after(100, self._pump_queue)
        self._refresh_ports()

    # ---------------------------------------------------------- layout ----
    def _build_connection_row(self):
        row = ttk.Frame(self, padding=8)
        row.pack(fill="x")

        ttk.Label(row, text="Port:").pack(side="left")
        self.port_var = tk.StringVar()
        self.port_combo = ttk.Combobox(row, textvariable=self.port_var, width=22, state="readonly")
        self.port_combo.pack(side="left", padx=4)

        ttk.Button(row, text="Refresh", command=self._refresh_ports).pack(side="left", padx=2)
        self.connect_btn = ttk.Button(row, text="Connect", command=self._toggle_connect)
        self.connect_btn.pack(side="left", padx=2)

        self.conn_status = ttk.Label(row, text="not connected", foreground="#a00")
        self.conn_status.pack(side="left", padx=8)

    def _build_form(self):
        frame = ttk.LabelFrame(self, text="Card details", padding=10)
        frame.pack(fill="x", padx=8, pady=4)

        ttk.Label(frame, text="Parent phone (PH mobile):").grid(row=0, column=0, sticky="w", pady=4)
        self.phone_var = tk.StringVar()
        ttk.Entry(frame, textvariable=self.phone_var, width=28).grid(row=0, column=1, sticky="w")
        ttk.Label(frame, text="e.g. 09171234567", foreground="#666").grid(row=0, column=2, sticky="w", padx=6)

        ttk.Label(frame, text="Student ID (0-65535):").grid(row=1, column=0, sticky="w", pady=4)
        self.id_var = tk.StringVar()
        ttk.Entry(frame, textvariable=self.id_var, width=28).grid(row=1, column=1, sticky="w")

        ttk.Label(frame, text="Student name (first name, ≤20 chars):").grid(row=2, column=0, sticky="w", pady=4)
        self.name_var = tk.StringVar()
        ttk.Entry(frame, textvariable=self.name_var, width=28).grid(row=2, column=1, sticky="w")

    def _build_actions(self):
        frame = ttk.Frame(self, padding=8)
        frame.pack(fill="x")
        self.write_btn = ttk.Button(frame, text="Write Card", command=self._on_write, state="disabled")
        self.write_btn.pack(side="left", padx=4)
        self.read_btn = ttk.Button(frame, text="Read / Verify Card", command=self._on_read, state="disabled")
        self.read_btn.pack(side="left", padx=4)
        self.clear_btn = ttk.Button(frame, text="Clear fields", command=self._clear_fields)
        self.clear_btn.pack(side="left", padx=4)

        self.hint = ttk.Label(self, text="Connect to the writer board, fill in the form, then tap Write Card and hold a blank card on the reader.", wraplength=460, foreground="#333")
        self.hint.pack(fill="x", padx=10, pady=(0, 4))

    def _build_log(self):
        frame = ttk.LabelFrame(self, text="Activity", padding=6)
        frame.pack(fill="both", expand=True, padx=8, pady=4)
        self.log = tk.Text(frame, height=12, state="disabled", wrap="word")
        self.log.pack(fill="both", expand=True)

    # --------------------------------------------------------- helpers ----
    def _log(self, msg: str):
        self.log.configure(state="normal")
        self.log.insert("end", msg + "\n")
        self.log.see("end")
        self.log.configure(state="disabled")

    def _refresh_ports(self):
        ports = [p.device for p in serial.tools.list_ports.comports()]
        self.port_combo["values"] = ports
        if ports and not self.port_var.get():
            self.port_var.set(ports[0])

    def _toggle_connect(self):
        if self.link.connected():
            self.link.close()
            self.connect_btn.configure(text="Connect")
            self.conn_status.configure(text="not connected", foreground="#a00")
            self.write_btn.configure(state="disabled")
            self.read_btn.configure(state="disabled")
            self._log("Disconnected.")
            return

        port = self.port_var.get()
        if not port:
            messagebox.showwarning("No port", "Select a serial port first.")
            return
        try:
            self.link.connect(port)
        except Exception as exc:
            messagebox.showerror("Connection failed", str(exc))
            self._log(f"Connect failed on {port}: {exc}")
            return

        self.connect_btn.configure(text="Disconnect")
        self.conn_status.configure(text=f"connected: {port}", foreground="#080")
        self.write_btn.configure(state="normal")
        self.read_btn.configure(state="normal")
        self._log(f"Connected to {port}.")

    def _clear_fields(self):
        self.phone_var.set("")
        self.id_var.set("")
        self.name_var.set("")

    def _set_busy(self, busy: bool, hint: str = None):
        state = "disabled" if busy else "normal"
        self.write_btn.configure(state=state)
        self.read_btn.configure(state=state)
        if hint:
            self.hint.configure(text=hint)

    # ----------------------------------------------------------- write ----
    def _on_write(self):
        phone = normalize_ph_phone(self.phone_var.get())
        if not phone:
            messagebox.showerror("Invalid phone", "Enter a valid PH mobile number, e.g. 09171234567.")
            return

        id_text = self.id_var.get().strip()
        if not id_text.isdigit() or not (0 <= int(id_text) <= 65535):
            messagebox.showerror("Invalid student ID", "Student ID must be a number from 0 to 65535.")
            return

        name = self.name_var.get().strip()
        if not name or len(name) > 20:
            messagebox.showerror("Invalid name", "Name must be 1-20 characters.")
            return
        if "," in name:
            messagebox.showerror("Invalid name", "Name can't contain a comma.")
            return

        cmd = f"WRITE,{phone},{int(id_text)},{name}"
        self._set_busy(True, f"Hold the card on the reader now — writing (up to {CARD_WAIT_HINT_S}s)...")
        self._log(f"> {cmd}")
        self.link.send_command_async(cmd, self._handle_write_reply)

    def _handle_write_reply(self, resp):
        self._set_busy(False, "Connect to the writer board, fill in the form, then tap Write Card and hold a blank card on the reader.")
        if isinstance(resp, Exception):
            self._log(f"! {resp}")
            messagebox.showerror("Write failed", str(resp))
            return
        self._log(f"< {resp}")
        if resp.startswith("OK:"):
            uid = resp[3:]
            messagebox.showinfo("Card written", f"Success. Card UID: {uid}")
        elif resp.startswith("ERR:"):
            code = resp[4:]
            messagebox.showerror("Write failed", ERROR_HINTS.get(code, code))
        else:
            messagebox.showwarning("Unexpected response", resp)

    # ------------------------------------------------------------ read ----
    def _on_read(self):
        self._set_busy(True, f"Hold the card on the reader now — reading (up to {CARD_WAIT_HINT_S}s)...")
        self._log("> READ")
        self.link.send_command_async("READ", self._handle_read_reply)

    def _handle_read_reply(self, resp):
        self._set_busy(False, "Connect to the writer board, fill in the form, then tap Write Card and hold a blank card on the reader.")
        if isinstance(resp, Exception):
            self._log(f"! {resp}")
            messagebox.showerror("Read failed", str(resp))
            return
        self._log(f"< {resp}")
        if resp.startswith("DATA:"):
            phone, student_id, name = resp[5:].split(",", 2)
            messagebox.showinfo("Card contents", f"Phone: {phone}\nStudent ID: {student_id}\nName: {name}")
        elif resp.startswith("ERR:"):
            code = resp[4:]
            messagebox.showerror("Read failed", ERROR_HINTS.get(code, code))
        else:
            messagebox.showwarning("Unexpected response", resp)

    # ----------------------------------------------------------- pump -----
    def _pump_queue(self):
        try:
            while True:
                cb, resp, diag = self.link.reply_q.get_nowait()
                for d in diag:
                    self._log(f"< {d}")
                cb(resp)
        except queue.Empty:
            pass
        self.after(100, self._pump_queue)

    def destroy(self):
        self.link.close()
        super().destroy()


if __name__ == "__main__":
    App().mainloop()
