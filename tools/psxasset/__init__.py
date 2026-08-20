"""psxasset — game-agnostic PSX disc asset extraction toolkit.

Framework tool. Format decoders (TIM, VAG), an ISO9660 walker, a container
framework driven by a per-game registry (``games/*.toml``), and a signature
scanner for unknown blobs. Stdlib only, like every other tool in this repo.

Run ``python3 tools/psxasset --help`` or ``python3 -m psxasset --help`` from
``tools/``.
"""

from __future__ import annotations

__version__ = "0.1.0"
