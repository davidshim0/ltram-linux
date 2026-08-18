import sys
from pathlib import Path

import matplotlib.pyplot as plt
import pandas as pd

TOP_DIR = Path(__file__).parents[2]
RESULTS_DIR = TOP_DIR / "results"

workload = sys.argv[1]
filename = RESULTS_DIR / f"meminfo_{workload}.txt"
df = pd.read_csv(filename)

df.plot(x="ts_s", y=[c for c in df.columns if c.endswith("_kb")])
plt.title(workload)
plt.xlabel("seconds")
plt.ylabel("kB")
plt.savefig(f"meminfo_plot_{workload}.png", bbox_inches="tight")
plt.show()
