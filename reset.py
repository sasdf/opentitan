import os
import time
import random

while True:
    os.system("opentitantool --interface=hyper340 --exec=\"gpio apply RESET\" --exec=\"gpio remove RESET\" no-op")
    time.sleep(random.uniform(0.2, 0.6))
