#!/usr/bin/env python3
import os
while True:
    b = os.read(0, 4096)
    if not b: break
    os.write(1, b)
