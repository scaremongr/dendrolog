#!/usr/bin/env python3
"""Deterministic test-log generator for DendroLog.

Emits lines in the primary format the parser detects without a schema:
    yyyy-MM-dd HH:mm:ss,zzz LEVEL [component] message

Examples:
    python generate_test_log.py out.log --lines 100000
    python generate_test_log.py big.log --size 600M --multiline-pct 10
    python generate_test_log.py grow.log --lines 1000 --append-batches 20 \
        --append-lines 50 --append-interval 1.0
    python generate_test_log.py - --lines 500 --append-batches 100  # stdout stream
"""

import argparse
import random
import sys
import time
from datetime import datetime, timedelta

LEVELS = ["TRACE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL"]
LEVEL_WEIGHTS = [1.5, 15, 70, 8, 5, 0.5]

COMPONENTS = [
    "core.engine", "net.dispatcher", "db.pool", "auth.service", "ui.render",
    "cache.lru", "sched.timer", "fs.watcher", "api.rest", "queue.consumer",
]

TEMPLATES = [
    "Request {rid} processed in {ms} ms (status={code})",
    "Connection from 192.168.{a}.{b}:{port} accepted",
    "Cache hit ratio {pct}% over {n} lookups",
    "Worker {wid} picked task #{tid} from queue depth={n}",
    "Retrying operation '{op}' attempt {k}/5 after {ms} ms",
    "Session {sid} expired, user u{uid} logged out",
    "Обработан пакет №{tid} из очереди (размер {n} байт)",   # non-ASCII on purpose
    "Синхронизация с узлом node-{wid} завершена за {ms} мс",  # non-ASCII on purpose
    "Flushed {n} dirty pages to disk in {ms} ms",
    "Heartbeat OK, lag={ms}ms, peers={k}",
]

ERROR_TEMPLATES = [
    "Failed to open handle {rid}: access denied",
    "Timeout after {ms} ms waiting for db.pool",
    "Unhandled exception in worker {wid}: NullReferenceException",
    "Ошибка записи в сегмент {tid}: диск переполнен",         # non-ASCII on purpose
    "Checksum mismatch in chunk {tid} (expected {rid})",
]

STACK_FRAMES = [
    "    at com.dendro.core.Engine.process(Engine.java:{n})",
    "    at com.dendro.net.Dispatcher.handle(Dispatcher.java:{n})",
    "    at java.base/java.util.concurrent.ThreadPoolExecutor.runWorker(ThreadPoolExecutor.java:{n})",
    "    at com.dendro.db.Pool.acquire(Pool.java:{n})",
]


def parse_size(text: str) -> int:
    text = text.strip().upper()
    mult = 1
    for suffix, m in (("K", 1 << 10), ("M", 1 << 20), ("G", 1 << 30)):
        if text.endswith(suffix):
            return int(float(text[:-1]) * m)
    return int(float(text) * mult)


def fill(template: str, rng: random.Random) -> str:
    return template.format(
        rid="%08x" % rng.getrandbits(32),
        ms=rng.randint(1, 4999),
        code=rng.choice([200, 200, 200, 204, 304, 404, 500]),
        a=rng.randint(0, 255), b=rng.randint(1, 254), port=rng.randint(1024, 65535),
        pct=rng.randint(1, 99), n=rng.randint(1, 99999),
        wid=rng.randint(1, 32), tid=rng.randint(1, 999999),
        op=rng.choice(["fsync", "connect", "publish", "compact"]),
        k=rng.randint(1, 5), sid="%012x" % rng.getrandbits(48),
        uid=rng.randint(1, 9999),
    )


class Generator:
    def __init__(self, args):
        self.rng = random.Random(args.seed)
        self.ts = datetime(2026, 7, 10, 9, 0, 0)
        self.args = args
        self.lines_written = 0
        self.records_written = 0
        self.level_counts = {lv: 0 for lv in LEVELS}

    def next_record(self):
        """Return a list of physical lines for one logical record."""
        rng = self.rng
        # Time moves forward 0..1500 ms; occasional bursts stay in the same ms.
        if rng.random() < 0.1:
            step = 0
        else:
            step = rng.randint(1, 1500)
        self.ts += timedelta(milliseconds=step)
        level = rng.choices(LEVELS, weights=LEVEL_WEIGHTS)[0]
        comp = rng.choice(COMPONENTS)
        if level in ("ERROR", "FATAL"):
            msg = fill(rng.choice(ERROR_TEMPLATES), rng)
        else:
            msg = fill(rng.choice(TEMPLATES), rng)
        stamp = self.ts.strftime("%Y-%m-%d %H:%M:%S,") + "%03d" % (self.ts.microsecond // 1000)
        lines = [f"{stamp} {level} [{comp}] {msg}"]
        if rng.random() * 100 < self.args.multiline_pct:
            depth = rng.randint(2, 6)
            lines.append("com.dendro.errors.ProcessingException: " + msg)
            for _ in range(depth):
                lines.append(fill(rng.choice(STACK_FRAMES), rng))
        self.records_written += 1
        self.level_counts[level] += 1
        self.lines_written += len(lines)
        return lines


def open_out(args):
    if args.path == "-":
        return sys.stdout.buffer, False
    mode = "ab" if args.append_only else "wb"
    return open(args.path, mode), True


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("path", help="output file, or '-' for stdout")
    ap.add_argument("--lines", type=int, default=0, help="approx number of physical lines")
    ap.add_argument("--size", type=parse_size, default=0, help="approx byte size, e.g. 600M")
    ap.add_argument("--multiline-pct", type=float, default=5.0, help="%% of records with stack traces")
    ap.add_argument("--crlf", action="store_true", help="use \\r\\n line endings")
    ap.add_argument("--bom", action="store_true", help="write UTF-8 BOM")
    ap.add_argument("--no-trailing-newline", action="store_true", help="omit EOL after the last line")
    ap.add_argument("--seed", type=int, default=42)
    ap.add_argument("--append-batches", type=int, default=0, help="after initial write, append N batches")
    ap.add_argument("--append-lines", type=int, default=50, help="lines per append batch")
    ap.add_argument("--append-interval", type=float, default=1.0, help="seconds between batches")
    ap.add_argument("--append-only", action="store_true", help="do not truncate, only append batches")
    args = ap.parse_args()

    if not args.lines and not args.size:
        args.lines = 100_000

    eol = b"\r\n" if args.crlf else b"\n"
    gen = Generator(args)
    out, is_file = open_out(args)
    bytes_written = 0
    pending_eol = False  # EOL is written lazily so --no-trailing-newline can drop the last one

    def emit(line: str):
        nonlocal bytes_written, pending_eol
        if pending_eol:
            out.write(eol)
            bytes_written += len(eol)
        data = line.encode("utf-8")
        out.write(data)
        bytes_written += len(data)
        pending_eol = True

    if args.bom and not args.append_only:
        out.write(b"\xef\xbb\xbf")
        bytes_written += 3

    if not args.append_only:
        while True:
            if args.lines and gen.lines_written >= args.lines:
                break
            if args.size and bytes_written >= args.size:
                break
            for line in gen.next_record():
                emit(line)

    if pending_eol and not args.no_trailing_newline:
        out.write(eol)
        pending_eol = False
    out.flush()

    for batch in range(args.append_batches):
        time.sleep(args.append_interval)
        start = gen.lines_written
        while gen.lines_written - start < args.append_lines:
            for line in gen.next_record():
                emit(line)
        if pending_eol and not args.no_trailing_newline:
            out.write(eol)
            pending_eol = False
        out.flush()
        print(f"batch {batch + 1}/{args.append_batches}: {gen.lines_written} lines total", file=sys.stderr)

    if is_file:
        out.close()

    summary = (f"lines={gen.lines_written} records={gen.records_written} bytes~{bytes_written} "
               + " ".join(f"{lv}={gen.level_counts[lv]}" for lv in LEVELS))
    print(summary, file=sys.stderr)


if __name__ == "__main__":
    main()
