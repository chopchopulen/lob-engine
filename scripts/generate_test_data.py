"""
generate_test_data.py

Creates a synthetic Nasdaq ITCH 5.0 binary file for testing.
This simulates a realistic order book for AAPL over ~30 minutes
with real buy/sell pressure patterns.

Run:
    python3 scripts/generate_test_data.py data/test.bin

Then run the C++ engine on it:
    ./lob_engine data/test.bin AAPL data/features_AAPL.csv

Then analyze:
    python3 scripts/analysis.py data/features_AAPL.csv
"""

import struct
import random
import math
import sys
from pathlib import Path

# ── ITCH 5.0 message encoding helpers ────────────────────────────────────────

def u16_be(v):  return struct.pack('>H', v)
def u32_be(v):  return struct.pack('>I', v)
def u64_be(v):  return struct.pack('>Q', v)
def u48_be(v):
    # 6-byte big-endian (ITCH uses 48-bit timestamps)
    b = struct.pack('>Q', v)
    return b[2:]  # drop top 2 bytes

def stock_bytes(ticker):
    # 4-char right-padded with spaces
    return ticker.ljust(4)[:4].encode('ascii')

def write_msg(f, body: bytes):
    # Each ITCH message is prefixed by a 2-byte length
    f.write(u16_be(len(body)))
    f.write(body)

def add_order_msg(ts_ns, order_ref, side, shares, ticker, price_units):
    """
    'A' message (Add Order, no MPID attribution)
    Layout:
      [0]     msg type = 'A'
      [1-2]   stock_locate = 1
      [3-4]   tracking = 0
      [5-10]  timestamp (48-bit ns)
      [11-18] order_ref (64-bit)
      [19]    side ('B' or 'S')
      [20-23] shares (32-bit)
      [24-27] stock (4 chars)
      [28-31] price (32-bit, 1/10000 dollars)
    """
    body = (b'A'
            + u16_be(1)            # stock_locate
            + u16_be(0)            # tracking_number
            + u48_be(ts_ns)        # timestamp
            + u64_be(order_ref)    # order_ref
            + side.encode('ascii') # 'B' or 'S'
            + u32_be(shares)       # shares
            + stock_bytes(ticker)  # stock
            + u32_be(price_units)) # price
    return body

def delete_order_msg(ts_ns, order_ref):
    """'D' message — cancel this order"""
    body = (b'D'
            + u16_be(1)
            + u16_be(0)
            + u48_be(ts_ns)
            + u64_be(order_ref))
    return body

def execute_order_msg(ts_ns, order_ref, exec_shares):
    """'E' message — this order was (partially) filled"""
    body = (b'E'
            + u16_be(1)
            + u16_be(0)
            + u48_be(ts_ns)
            + u64_be(order_ref)
            + u32_be(exec_shares)
            + u64_be(order_ref + 90000000))  # match_number
    return body

# ── Simulation ────────────────────────────────────────────────────────────────

def simulate(path: str, ticker: str = 'AAPL'):
    """
    Simulates an order book with:
    - A mid-price that does a random walk (realistic drift + noise)
    - Buy/sell orders placed symmetrically around mid
    - Occasional executions (trades)
    - Gradual cancellations as orders age
    """
    random.seed(42)
    Path(path).parent.mkdir(parents=True, exist_ok=True)

    # Market open: 9:30 AM EST = 34200 seconds * 1e9 ns
    ts_ns       = 34200 * 1_000_000_000
    step_ns     = 100_000  # 0.1 ms between messages (realistic HFT)

    order_ref   = 1_000_000
    mid_price   = 15000000  # $150.00 in 1/10000 units
    tick_size   = 100       # $0.01

    # Track live orders: {order_ref: (side, price, shares)}
    live_orders = {}
    order_count = 0
    total_msgs  = 0

    # Simulate ~1.8M messages (represents ~30 min of activity for a liquid stock)
    target_msgs = 1_800_000

    with open(path, 'wb') as f:
        while total_msgs < target_msgs:

            # ── Evolve mid-price (random walk with mean reversion) ──────────
            # This creates realistic price dynamics: small random steps,
            # pulled back toward a long-run mean.
            drift     = -0.001 * (mid_price - 15000000)  # mean reversion
            noise     = random.gauss(0, tick_size * 2)
            mid_price = int(mid_price + drift + noise)
            mid_price = max(14000000, min(16000000, mid_price))  # clamp $140-$160

            # ── Add some new limit orders ────────────────────────────────────
            for _ in range(random.randint(3, 8)):
                side   = random.choice(['B', 'S'])
                # Keep orders 1-5 ticks from mid — gives realistic $0.01-$0.05 spread
                offset = random.randint(1, 5) * tick_size
                if side == 'B':
                    price = mid_price - offset
                else:
                    price = mid_price + offset

                shares = random.choice([100, 200, 300, 500])
                ref    = order_ref
                order_ref += 1

                write_msg(f, add_order_msg(ts_ns, ref, side, shares, ticker, price))
                live_orders[ref] = (side, price, shares)
                order_count += 1
                total_msgs  += 1
                ts_ns       += step_ns

            # ── Cancel some old orders ───────────────────────────────────────
            if live_orders and random.random() < 0.4:
                ref = random.choice(list(live_orders.keys()))
                write_msg(f, delete_order_msg(ts_ns, ref))
                del live_orders[ref]
                total_msgs += 1
                ts_ns      += step_ns

            # ── Simulate a trade ─────────────────────────────────────────────
            if live_orders and random.random() < 0.05:
                ref = random.choice(list(live_orders.keys()))
                _, _, shares = live_orders[ref]
                exec_shares  = min(shares, random.choice([100, 200]))
                write_msg(f, execute_order_msg(ts_ns, ref, exec_shares))

                if exec_shares >= shares:
                    del live_orders[ref]
                else:
                    side, price, _ = live_orders[ref]
                    live_orders[ref] = (side, price, shares - exec_shares)

                total_msgs += 1
                ts_ns      += step_ns

    print(f"Generated {total_msgs:,} messages for {ticker}")
    print(f"Final mid-price: ${mid_price / 10000:.2f}")
    print(f"Written to: {path}")

if __name__ == '__main__':
    out = sys.argv[1] if len(sys.argv) > 1 else 'data/test.bin'
    simulate(out)
