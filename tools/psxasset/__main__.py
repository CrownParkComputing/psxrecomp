from __future__ import annotations

import sys
from pathlib import Path

if __package__ in (None, ""):
    # Invoked as ``python3 tools/psxasset`` — make the package importable.
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
    from psxasset.cli import main
else:
    from .cli import main

sys.exit(main())
